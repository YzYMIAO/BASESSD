/*
 * controller.c — HIL Controller：生命周期、入口校验、路由与 FLUSH/TRIM 异步命令
 *
 * 职责：维护 controller 状态机；校验 host 请求并路由到 FTL 实例；
 * FLUSH/TRIM 作为单飞行异步 job 每轮推进；不实现 FTL 映射 / GC 策略。
 */
#include "controller.h"

#include "assertion.h"
#include "delay.h"    /* delay_now_ms：请求级 deadline */
#include "lifetime.h" /* 终身计数：FLUSH / TRIM 成功计数 */
#include "log.h"
#include "watchdog.h" /* storage_wdg_kick：format MBR 的同步泵 */

#include <stddef.h>
#include <string.h>

/* 外部：request.c — 按实例边界切 run / 提交子请求 */
ssd_status_e ssd_request_split(ssd_ctl_s *controller, ssd_req_s *request);

QWORD      ssd_controller_run_end(ssd_ctl_s *controller, QWORD lba, QWORD lba_limit);
ftl_ctx_s *ssd_controller_route(const ssd_ctl_s *controller, QWORD lba);

/**
 * @brief 小端写 32 bit（MBR 字段）
 *
 * @param p 目标
 * @param v 值
 */
static void mbr_put32(BYTE *p, DWORD v)
{
    p[0] = (BYTE)(v & 0xFFU);
    p[1] = (BYTE)((v >> 8U) & 0xFFU);
    p[2] = (BYTE)((v >> 16U) & 0xFFU);
    p[3] = (BYTE)((v >> 24U) & 0xFFU);
}

/**
 * @brief 推进一轮 NAND 完成与目标实例的 host 补发
 *
 * MBR 的同步读/写不能只调 `ftl_service_pump()`：未映射 LBA 的首次读会起
 * L2P region 异步加载，组停在 `next=0`，必须由 `ftl_service()` 的 host 补发
 * 重新下发；漏掉它会让 MBR 读/写永远等不到完成。
 *
 * @param ftl 目标 FTL 实例
 */
static void ssd_mbr_service_step(ftl_ctx_s *ftl)
{
    storage_wdg_kick();
    ftl_service_pump();
    ftl_service(ftl);
}

/**
 * @brief LBA0 是否为 format 后未映射的全零扇区
 *
 * FTL 对未映射 LBA 的读取契约是零填充，因此 format 后 LBA0 必然全 0x00。
 * 全 0xFF 不视为空白：那可能是用户实际写入的数据，不能覆盖。
 *
 * @param sector 512 B 扇区
 * @return true  全 0x00；false 有非零字节
 */
static BOOL ssd_lba0_zero(const BYTE sector[512])
{
    DWORD zeros = 0U;

    for (DWORD i = 0U; i < 512U; i++)
    {
        zeros += (sector[i] == 0x00U) ? 1U : 0U;
    }
    return zeros == 512U;
}

/**
 * @brief 写默认 MBR（分区从 LBA 128 起，类型 0x0C）
 *
 * @param controller Controller 上下文（READY）
 * @return SSD_STATUS_OK；容量不足 / 写 / flush 失败返回错误
 */
static ssd_status_e ssd_mbr_write_default(ssd_ctl_s *controller)
{
    const DWORD start_lba = 128U; /* 64 KiB 对齐，与 usb_verify.ps1 的标准布局一致 */
    const DWORD max       = ssd_controller_host_lba_max(controller);

    /* controller / READY / ftl[0] 已由唯一调用者 ensure 校验；这里只守容量前提。 */
    if (max <= start_lba)
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    ftl_ctx_s *ftl = controller->ftl[0];
    BYTE       mbr[512];

    memset(mbr, 0, sizeof(mbr));

    /* 分区项 0：LBA 模式；CHS 对现代 Windows 不参与寻址。 */
    mbr[446] = 0x00U; /* 非活动分区 */
    mbr[447] = 0x02U; /* start CHS: head */
    mbr[448] = 0x03U; /* start CHS: sector */
    mbr[449] = 0x00U; /* start CHS: cylinder */
    mbr[450] = 0x0CU; /* FAT32 LBA */
    mbr[451] = 0xFEU; /* end CHS 占位（LBA 模式） */
    mbr[452] = 0xFFU;
    mbr[453] = 0xFFU;
    mbr_put32(&mbr[454], start_lba);
    mbr_put32(&mbr[458], max - start_lba);
    mbr[510] = 0x55U;
    mbr[511] = 0xAAU;

    volatile BYTE         done   = 0U;
    volatile ssd_status_e status = SSD_STATUS_IO_ERROR;
    ftl_io_req_s          req;

    memset(&req, 0, sizeof(req));
    req.lba           = 0U;
    req.buffer        = mbr;
    req.count         = 1U;
    req.complete_flag = &done;
    req.status_out    = &status;

    if (ftl_submit_write(ftl, &req) != FTL_OK)
    {
        return SSD_STATUS_IO_ERROR;
    }

    for (DWORD spin = 0U; (spin < 1000000U) && (done == 0U); spin++)
    {
        ssd_mbr_service_step(ftl);
    }

    if ((done == 0U) || (status != SSD_STATUS_OK))
    {
        return SSD_STATUS_IO_ERROR;
    }

    /* MBR 映射必须随 Journal/CP 落盘后才能断电；否则 L2P 回滚回 partitionless。 */
    ftl_flush_begin(ftl);

    for (DWORD spin = 0U; spin < 1000000U; spin++)
    {
        ssd_mbr_service_step(ftl);

        if (ftl_flush_step(ftl) != false)
        {
            return SSD_STATUS_OK;
        }
    }

    return SSD_STATUS_IO_ERROR;
}

/**
 * @brief 推进全部 present die 的 FLUSH job；全部完成时回执 flush_req。
 *
 * @param controller Controller 上下文
 */
static void ssd_controller_flush_service(ssd_ctl_s *controller)
{
    if (controller->flush_req == NULL)
    {
        return;
    }

    BOOL all_done = true;

    /* 先推进**全部** present die 再判完成：某一实例未完成时直接 return 会让后面的实例
     * 永远得不到推进（多实例下 FLUSH 卡死）。 */
    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_s *fftl = controller->ftl[die];

        ASSERT(fftl != NULL);

        if (!ftl_flush_step(fftl))
        {
            all_done = false;
        }
    }

    if (!all_done)
    {
        return; /* 还有实例没落定：下一轮继续 */
    }

    /* 全部实例完成：按 HIL 完成通道回执（complete_flag / status_out）。
     * 已超时的请求保留 host 侧 TIMEOUT，不被迟到完成覆盖。 */
    if (controller->flush_req->timed_out == false)
    {
        controller->flush_req->completion_status = SSD_STATUS_OK;
        controller->flush_req->complete_flag     = 1U;
        lifetime_note_cmd(LIFETIME_CMD_FLUSH);
    }
#if SSD_STATS_ENABLE
    ssd_request_note_latency(controller->flush_req);
#endif
    controller->flush_req = NULL;
}

/**
 * @brief 以给定状态完成当前 TRIM 请求并清空 trim_req。
 *
 * @param controller Controller 上下文
 * @param status     回执状态
 */
static void ssd_controller_trim_complete(ssd_ctl_s *controller, ssd_status_e status)
{
    /* 已超时的请求保留 host 侧 TIMEOUT，不被迟到完成覆盖。 */
    if (controller->trim_req->timed_out == false)
    {
        controller->trim_req->completion_status = status;
        controller->trim_req->complete_flag     = 1U;
#if SSD_STATS_ENABLE
        if (status != SSD_STATUS_OK)
        {
            ssd_request_note_bad_status();
        }
#endif
        if (status == SSD_STATUS_OK)
        {
            lifetime_note_cmd(LIFETIME_CMD_TRIM);
        }
    }
#if SSD_STATS_ENABLE
    ssd_request_note_latency(controller->trim_req);
#endif
    controller->trim_req     = NULL;
    controller->trim_started = false;
}

/**
 * @brief 按 run 推进在飞 TRIM（begin/step），全部完成时回执 trim_req。
 *
 * @param controller Controller 上下文
 */
static void ssd_controller_trim_service(ssd_ctl_s *controller)
{
    if (controller->trim_req == NULL)
    {
        return;
    }

    const QWORD end = controller->trim_req->start_lba + controller->trim_req->block_count;

    if (controller->trim_lba >= end)
    {
        ssd_controller_trim_complete(controller, SSD_STATUS_OK);
        return;
    }

    const QWORD run_end = ssd_controller_run_end(controller, controller->trim_lba, end);
    ftl_ctx_s  *tftl    = ssd_controller_route(controller, controller->trim_lba);

    /* 路由只可能因宏配置错误失败；正常配置下恒非 NULL */
    ASSERT(tftl != NULL);

    if (!controller->trim_started)
    {
        const DWORD        lba   = (DWORD)controller->trim_lba;
        const DWORD        count = (DWORD)(run_end - controller->trim_lba);
        const ftl_status_e bst   = ftl_trim_begin(tftl, lba, count);

        if (bst == FTL_AGAIN)
        {
            return; /* 实例上已有 job：下一轮重试 */
        }
        if (bst != FTL_OK)
        {
            ssd_controller_trim_complete(controller, SSD_STATUS_IO_ERROR);
            return;
        }
        controller->trim_started = true;
    }

    const ftl_status_e st = ftl_trim_step(tftl);

    if (st == FTL_AGAIN)
    {
        return; /* 本轮未完成：下一轮继续 */
    }
    if (st != FTL_OK)
    {
        ssd_controller_trim_complete(controller, SSD_STATUS_IO_ERROR);
        return;
    }

    controller->trim_started = false;
    controller->trim_lba     = run_end;

    if (controller->trim_lba >= end)
    {
        ssd_controller_trim_complete(controller, SSD_STATUS_OK);
    }
}

/**
 * @brief 按全局 LBA 路由到对应 FTL 实例（region 粒度）。
 *
 * 唯一实现：controller submit 与 request split 共用；换哈希策略只改这里。
 *
 * @param controller Controller 上下文（调用者保证非 NULL）
 * @param lba        host 全局 LBA
 * @return 路由到的 FTL 实例
 */
ftl_ctx_s *ssd_controller_route(const ssd_ctl_s *controller, QWORD lba)
{
    /* 唯一调用者 ssd_controller_submit / ssd_request_split 均已判过 controller */
    ASSERT(controller != NULL);

    const BYTE die = lba_2_die(lba);

    /* 路由取模结果本应在 [0,DIE_MAX) 内 */
    ASSERT(die < DIE_MAX);

    return controller->ftl[die];
}

/**
 * @brief host 写启动许可（按 LBA 路由到 FTL 实例）
 *
 * @param controller Controller 上下文
 * @param lba        写起始 LBA
 * @return true 允许启动；false 暂缓
 */
BOOL ssd_controller_host_write_admit(const ssd_ctl_s *controller, QWORD lba)
{
    if ((controller == NULL) || (controller->state != SSD_CONTROLLER_READY))
    {
        return false;
    }

    ftl_ctx_s *ftl = ssd_controller_route(controller, lba);

    return (ftl != NULL) && (ftl_host_write_is_admitted(ftl) != false);
}

/**
 * @brief boot 后确保空白介质带默认 MBR
 *
 * 读 LBA0：已有 55AA 引导签名则保持原样；LBA0 全 0x00（format 后未映射）
 * 则写默认 MBR。LBA0 有数据但无引导签名时不动，避免覆盖用户自建布局。
 * 调用点必须在 ftl_boot_recover 回填 free/op/spare 池之后。
 *
 * @param controller Controller 上下文（READY；非空）
 * @return SSD_STATUS_OK 已有 MBR / 已写入 / LBA0 有数据保持原样
 *         SSD_STATUS_INVALID_ARGUMENT 状态或实例非法
 *         SSD_STATUS_IO_ERROR LBA0 读失败或 MBR 写失败
 */
ssd_status_e ssd_controller_ensure_default_mbr(ssd_ctl_s *controller)
{
    if ((controller == NULL) || (controller->state != SSD_CONTROLLER_READY) || (controller->ftl[0] == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    ftl_ctx_s *ftl = controller->ftl[0];
    BYTE       sector[512];

    memset(sector, 0, sizeof(sector));

    volatile BYTE         done   = 0U;
    volatile ssd_status_e status = SSD_STATUS_IO_ERROR;
    ftl_io_req_s          req;

    memset(&req, 0, sizeof(req));
    req.lba           = 0U;
    req.buffer        = sector;
    req.count         = 1U;
    req.complete_flag = &done;
    req.status_out    = &status;

    if (ftl_submit_read(ftl, &req) != FTL_OK)
    {
        LOGE("[HIL ] default MBR: LBA0 read submit failed\r\n");
        return SSD_STATUS_IO_ERROR;
    }

    for (DWORD spin = 0U; (spin < 1000000U) && (done == 0U); spin++)
    {
        ssd_mbr_service_step(ftl);
    }

    if ((done == 0U) || (status != SSD_STATUS_OK))
    {
        LOGE("[HIL ] default MBR: LBA0 read failed\r\n");
        return SSD_STATUS_IO_ERROR;
    }

    if ((sector[510] == 0x55U) && (sector[511] == 0xAAU))
    {
        return SSD_STATUS_OK; /* 已有 MBR / 文件系统引导扇区，不覆盖 */
    }

    if (ssd_lba0_zero(sector) == false)
    {
        /* LBA0 有数据但不是 MBR：可能是用户自建的分区方案，保持原样。 */
        LOGW("[HIL ] default MBR: LBA0 has data without 55AA; leave unchanged\r\n");
        return SSD_STATUS_OK;
    }

    if (ssd_mbr_write_default(controller) != SSD_STATUS_OK)
    {
        LOGE("[HIL ] default MBR: write failed\r\n");
        return SSD_STATUS_IO_ERROR;
    }

    LOGI("[HIL ] default MBR written (partition LBA 128)\r\n");
    return SSD_STATUS_OK;
}

/**
 * @brief Controller 最小状态机（C1）
 *
 * 管理 RESET → INITIALIZING → READY / FAILED 转换：
 *   · ssd_controller_init 的槽位校验结果驱动转换：全部通过 → READY，
 *     任一槽位缺失/拓扑不符 → FAILED（终态）；
 *   · READY → FAILED 预留给运行时致命错误（当前无调用点）；
 *   · 非法转换直接忽略（保持现态）。
 *
 * SHUTDOWN 不在枚举内（学习主控无热移除/优雅掉电退出路径）；若后续加枚举值，
 * 只扩展本函数的转换表，不动调用方。
 *
 * @param controller Controller 上下文（NULL 时安全返回）
 * @param next_state 目标状态
 */
void ssd_controller_state_transition(ssd_ctl_s *controller, ssd_ctrl_state_e next_state)
{
    ssd_ctrl_state_e prev_state;

    if (controller == NULL)
    {
        return;
    }

    prev_state = controller->state;

    switch (controller->state)
    {
        case SSD_CONTROLLER_RESET:
            /* boot 起点：RESET → INITIALIZING（ssd_controller_init 入口） */
            if (next_state == SSD_CONTROLLER_INITIALIZING)
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_INITIALIZING:
            /* 槽位校验结束：全部通过 → READY；任一失败 → FAILED */
            if ((next_state == SSD_CONTROLLER_READY) || (next_state == SSD_CONTROLLER_FAILED))
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_READY:
            /* 运行时致命错误 → FAILED（当前无调用点，保留语义） */
            if (next_state == SSD_CONTROLLER_FAILED)
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_FAILED:
        default:
            /* FAILED 为终态，不响应任何转换 */
            break;
    }

    /* FAILED 是终态，进入即宣告 controller 不可用；在此单点留痕 */
    LOGE_IF((prev_state != SSD_CONTROLLER_FAILED) && (controller->state == SSD_CONTROLLER_FAILED), LOG_TAG_CTRL "state -> FAILED\r\n");
}

/**
 * @brief 初始化 Controller，按 compact die 号绑定 FTL 实例表。
 *
 * 只检查 present die：ftl_by_die[die] 必须非空，且实例 topo 必须与
 * die 表分解出的 (ch,ce,lun) 一致；同时清零 host queue。
 *
 * @param controller Controller 上下文
 * @param ftl_by_die die → 实例指针表（前 DIE_MAX 项有效）
 * @return SSD_STATUS_OK               成功
 *         SSD_STATUS_INVALID_ARGUMENT 入参非法 / 槽位缺失 / 拓扑不符
 */
ssd_status_e ssd_controller_init(ssd_ctl_s *controller, ftl_ctx_s *const *ftl_by_die)
{
    if ((controller == NULL) || (ftl_by_die == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

#if SSD_STATS_ENABLE
    ssd_request_stats_register();
#endif

    controller->flush_req    = NULL;
    controller->trim_req     = NULL;
    controller->trim_lba     = 0U;
    controller->trim_started = false;
    controller->host_lba_max = (DWORD)FTL_LBA_MAX;

    ssd_host_queue_init(&controller->host_queue);

    /* 稀疏拓扑 / 重复 init：先清空 die 槽，缺失项保持 NULL。 */
    memset(controller->ftl, 0, sizeof(controller->ftl));

    /* 逐 present die 校验并绑定 FTL 实例：稀疏拓扑允许部分 die 槽为 NULL。
     * 状态机（C1）：入口 RESET → INITIALIZING，校验结果驱动 READY/FAILED。 */
    ssd_controller_state_transition(controller, SSD_CONTROLLER_INITIALIZING);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const BYTE ch   = die_2_ch(die);
        const BYTE ce   = die_2_ce(die);
        const BYTE lun  = die_2_lun(die);
        ftl_ctx_s *slot = ftl_by_die[die];

        if (slot == NULL)
        {
            LOGE(LOG_TAG_CTRL "init failed: null FTL slot die=%hhu ch=%hhu ce=%hhu lun=%hhu\r\n", die, ch, ce, lun);
            ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
            return SSD_STATUS_INVALID_ARGUMENT;
        }
        /* die 索引必须等于实例绑定拓扑：保证 LBA 路由与芯片定位语义一致 */
        if ((ftl_topo_get(slot)->ch != ch) || (ftl_topo_get(slot)->ce != ce) || (ftl_topo_get(slot)->lun != lun))
        {
            LOGE(LOG_TAG_CTRL "init failed: topo mismatch die=%hhu slot ch=%u ce=%u lun=%u\r\n", die, ch, ce, lun);
            ssd_controller_state_transition(controller, SSD_CONTROLLER_FAILED);
            return SSD_STATUS_INVALID_ARGUMENT;
        }
        controller->ftl[die] = slot;
    }

    ssd_controller_state_transition(controller, SSD_CONTROLLER_READY);
    return SSD_STATUS_OK;
}

/**
 * @brief 查询请求是否完成。
 *
 * @param request 请求（NULL 视为已完成）
 * @return 1 = 完成；0 = 未完成
 */
BYTE ssd_controller_is_complete(const ssd_req_s *request)
{
    if (request == NULL)
    {
        return 1U;
    }
    return request->complete_flag;
}

/**
 * @brief 超时后请求是否已可安全释放
 *
 * @param controller Controller 上下文
 * @param request    请求（非空）
 * @return true = 已无 FTL / controller 引用
 */
BOOL ssd_controller_request_reclaimable(const ssd_ctl_s *controller, const ssd_req_s *request)
{
    ASSERT(controller != NULL);
    ASSERT(request != NULL);

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
        case SSD_REQUEST_WRITE:
            return request->sub_remaining == 0U;

        case SSD_REQUEST_FLUSH:
            return controller->flush_req != request;

        case SSD_REQUEST_TRIM:
            return controller->trim_req != request;

        default:
            return true;
    }
}

/**
 * @brief host LBA 区间是否落在设备容量内。
 *
 * 区间为 [lba, lba + count)，count > 0。
 *
 * @param controller Controller 上下文
 * @param lba        起始 LBA
 * @param count      连续 LBA 数
 * @return true = 完整落在 [0, host_lba_max)；false = 非法/越界
 */
BOOL ssd_controller_lba_range_ok(const ssd_ctl_s *controller, QWORD lba, DWORD count)
{
    /* 容量口径 = 运行时 host_lba_max（已扣实际坏块），不是编译期 FTL_LBA_MAX。
     * lba 与 count 的上界之和 < 2^33，`QWORD` 加法不会回绕；先判起点再判终点。 */
    const DWORD max = ssd_controller_host_lba_max(controller);

    if ((count == 0U) || (lba >= max))
    {
        return false;
    }

    return (lba + count) <= max;
}

/**
 * @brief 提交一条 host 请求（READ/WRITE/FLUSH/TRIM）。
 *
 * READ/WRITE 按 FTL 实例拆 run 后异步提交；FLUSH/TRIM 只登记并启动异步 job。
 * 请求对象必须活到 complete_flag 置 1。
 *
 * @param controller Controller 上下文（必须 READY）
 * @param request    主机请求
 * @return SSD_STATUS_OK               已受理（部分 run 失败也以 OK 受理，错误异步回执）
 *         SSD_STATUS_INVALID_ARGUMENT 状态/范围/缓冲非法
 *         SSD_STATUS_BUSY             FLUSH/TRIM 单飞行冲突
 *         SSD_STATUS_NOT_SUPPORTED    未知 opcode
 */
ssd_status_e ssd_controller_submit(ssd_ctl_s *controller, ssd_req_s *request)
{
    ssd_status_e ret;

    if ((controller == NULL) || (request == NULL))
    {
        return SSD_STATUS_INVALID_ARGUMENT;
    }

#if SSD_STATS_ENABLE
    request->start_timed = delay_time_now_us(&request->start_us);
#endif

    if (controller->state != SSD_CONTROLLER_READY)
    {
        LOGW(LOG_TAG_CTRL "submit rejected: state=%d\r\n", (int)controller->state);

        /* 同步拒绝也要把请求标成完成：轮询型调用者不会因为返回码而跳过 poll。 */
        request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
        request->complete_flag     = 1U;
        return SSD_STATUS_INVALID_ARGUMENT;
    }

    /* 主机 IO 到来后的后台让路由 ftl_service 的档位判定（pending/groups → IDLE/
     * NORMAL/BUSY）承担；Controller 只负责受理与拆分，不维护调度钩子。 */

    /* 清零完成标志 / 状态聚合 / deadline */
    request->complete_flag     = 0U;
    request->completion_status = SSD_STATUS_OK;
    request->agg_status        = SSD_STATUS_OK;
    request->sub_status        = SSD_STATUS_OK;
    request->start_ms          = delay_now_ms();
    request->timed_out         = false;

    /* 组装 IO 请求模板（lba/buffer 由各路径覆盖）。
     * status_out 是 FTL 组完成写入口；必须指向 sub_status，不能直接改 host 可见的
     * completion_status（迟到完成会在超时 tombstone 之后把它覆盖成 OK）。 */
    request->io.complete_flag   = &request->complete_flag;
    request->io.status_out      = &request->sub_status;
    request->io.on_complete     = NULL;
    request->io.on_complete_ctx = NULL;

    switch (request->opcode)
    {
        case SSD_REQUEST_READ:
        case SSD_REQUEST_WRITE:
            /* HIL 入口统一校验 LBA 范围（H29：host 入口是不可信输入）。USB/CLI
             * 前端也各自检查；这里再收一次，保证直接调用 controller 的测试/新前端
             * 不会把越界 LBA 交给 FTL（FTL 只按编译期上限检查，不知道运行时容量）。 */
            if (!ssd_controller_lba_range_ok(controller, request->start_lba, request->block_count))
            {
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            /* 数据缓冲必须存在：READ 回填 / WRITE 数据源都从这里走（H29）。 */
            if (request->buffer == NULL)
            {
                LOGE(LOG_TAG_CTRL "null data buffer count=%lu\r\n", request->block_count);
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            /* 缓冲容量校验：block_count 个映射单元 × 512B 必须不超过调用者给的
             * buffer_size（CW 级映射下 1 个 LBA = 1 个 CW = 512B 扇区，不是 2KB 页）。
             * 真实隐患：wr 0 64 会写爆 CLI 的 g_wr_buf。
             * ⚠ 保留 `(size_t)`：先加宽再相乘，避免 32 位运算溢出后绕过本检查
             *   （H44 的「刻意加宽避免溢出」例外）。 */
            if (request->buffer_size < ((size_t)request->block_count * FTL_IU_BYTES))
            {
                LOGE(LOG_TAG_CTRL "buffer too small count=%lu need=%zu have=%zu\r\n", request->block_count, (size_t)request->block_count * FTL_IU_BYTES, request->buffer_size);
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            /* 读/写统一走 run 拆分（含单块）：plane 配对在 FTL 内，HIL 只按实例切段 */
            ret = ssd_request_split(controller, request);
            if (ret != SSD_STATUS_OK)
            {
                if (request->sub_remaining == 0U)
                {
                    /* 一个 run 都没在飞：同步失败，可以立即完成。 */
                    request->completion_status = ret;
                    request->complete_flag     = 1U;
                    return ret;
                }

                /* 部分 run 已在飞：请求**已受理**，不能在这里置 complete_flag
                 * （否则调用者会释放 bounce/请求对象，而在飞 run 仍持有它）。
                 * 错误已锁存到 agg_status，等全部 run 完成后由 on_sub_complete 回执。 */
#if SSD_STATS_ENABLE
                ssd_request_note_cmd(request->opcode);
#endif
                return SSD_STATUS_OK;
            }

            /* 拆分成功 = 已受理。**不能**返回 request->completion_status：
             * 同步完成的 run 可能已把它写成错误，而其他 run 仍在飞；前端若据此
             * 判同步失败并释放请求，会让在飞 run 的 on_complete_ctx 悬垂。
             * 最终状态只经 complete_flag 异步回执（与函数契约一致）。 */
#if SSD_STATS_ENABLE
            ssd_request_note_cmd(request->opcode);
#endif
            return SSD_STATUS_OK;

        case SSD_REQUEST_TRIM:
            /* TRIM 是**异步命令**：submit 只登记范围，ssd_controller_service 每轮
             * 按 run 调 ftl_trim_begin/step；完成后置 complete_flag。
             * 单飞行：同类请求已有一条在飞时返回 BUSY。 */
            if (controller->trim_req != NULL)
            {
                request->completion_status = SSD_STATUS_BUSY;
                request->complete_flag     = 1U;
                return SSD_STATUS_BUSY;
            }
            if (!ssd_controller_lba_range_ok(controller, request->start_lba, request->block_count))
            {
                request->completion_status = SSD_STATUS_INVALID_ARGUMENT;
                request->complete_flag     = 1U;
                return SSD_STATUS_INVALID_ARGUMENT;
            }

            controller->trim_req       = request;
            controller->trim_lba       = request->start_lba;
            controller->trim_started   = false;
            request->completion_status = SSD_STATUS_OK;
#if SSD_STATS_ENABLE
            ssd_request_note_cmd(request->opcode);
#endif
            break;

        case SSD_REQUEST_FLUSH:
            /* FLUSH 是**异步命令**：提交只登记请求并启动各实例的
             * flush job，立即返回；完成由 ssd_controller_service 每轮推进后置
             * complete_flag。这样 host 回调（tud_task 内）不再自旋，USB 事件
             * 队列不会被饿死。单飞行：同类请求已有一条在飞时返回 BUSY。 */
            if (controller->flush_req != NULL)
            {
                request->completion_status = SSD_STATUS_BUSY;
                request->complete_flag     = 1U;
                return SSD_STATUS_BUSY;
            }

            {
                for (BYTE die = 0U; die < DIE_MAX; die++)
                {
                    ftl_ctx_s *fftl = controller->ftl[die];

                    /* present die 由 ssd_controller_init 绑满，恒非空 */
                    ASSERT(fftl != NULL);
                    ftl_flush_begin(fftl);
                }
            }

            controller->flush_req      = request;
            request->completion_status = SSD_STATUS_OK;
#if SSD_STATS_ENABLE
            ssd_request_note_cmd(request->opcode);
#endif
            break;

        default:
            request->completion_status = SSD_STATUS_NOT_SUPPORTED;
            request->complete_flag     = 1U;
            break;
    }

    return request->completion_status;
}

/**
 * @brief 推进 Controller 的异步命令（FLUSH / TRIM）。
 *
 * 主循环每轮调用一次（FTL 服务之后、USB 任务之前）；无在飞命令时立即返回。
 *
 * @param controller Controller 上下文
 */
void ssd_controller_service(ssd_ctl_s *controller)
{
    if (controller == NULL)
    {
        return;
    }

    /* 请求级 deadline：FLUSH 可撤销；TRIM 没有 abort API，只解开 host 回执，
     * 继续 step 到 FTL 自己收尾（trim_req 清空后前端才 reclaim）。 */
    const DWORD now_ms = delay_now_ms();

    if (controller->flush_req != NULL)
    {
        if ((controller->flush_req->timed_out == false) && (ssd_request_expired(controller->flush_req->start_ms, now_ms) != false))
        {
            LOGW(LOG_TAG_CTRL "FLUSH timeout %lums\r\n", (DWORD)(now_ms - controller->flush_req->start_ms));
            ssd_request_mark_timeout(controller->flush_req);
        }

        if (controller->flush_req->timed_out != false)
        {
            ssd_controller_cancel_flush(controller);
        }
    }

    if (controller->trim_req != NULL)
    {
        if ((controller->trim_req->timed_out == false) && (ssd_request_expired(controller->trim_req->start_ms, now_ms) != false))
        {
            LOGW(LOG_TAG_CTRL "TRIM timeout %lums\r\n", (DWORD)(now_ms - controller->trim_req->start_ms));
            ssd_request_mark_timeout(controller->trim_req);
        }

        if (controller->trim_req->timed_out != false)
        {
            if (controller->trim_started != false)
            {
                ftl_ctx_s *tftl = ssd_controller_route(controller, controller->trim_lba);

                ASSERT(tftl != NULL);
                ftl_trim_abort(tftl);
            }

            /* host 回执已给；TRIM job 已中止，立即摘指针让前端 reclaim。 */
            controller->trim_req     = NULL;
            controller->trim_started = false;
        }
    }

    ssd_controller_flush_service(controller);
    ssd_controller_trim_service(controller);
}

/**
 * @brief 放弃在飞的 FLUSH 回执（host 复位 / 拔出）。
 *
 * 摘掉 flush_req 并清各实例的 flush job 标志；在飞数据操作不回滚。
 *
 * @param controller Controller 上下文
 */
void ssd_controller_cancel_flush(ssd_ctl_s *controller)
{
    if (controller == NULL)
    {
        return;
    }

    /* 摘回执指针 + 清各实例的 job 标志：避免旧 job 在请求对象被复用后才完成，
     * 把回执写到新请求上。在飞的数据操作有自己的服务路径，不需要回滚。 */
    controller->flush_req = NULL;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_s *ftl = controller->ftl[die];

        ASSERT(ftl != NULL);
        ftl_flush_abort(ftl);
    }
}
