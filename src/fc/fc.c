/*
 * fc.c — FC 调度与服务体系（初始化编排 / 提交 / 完成 / 服务主循环）
 *
 * 分工：
 *   fc_pool.c  —— 命令/描述符/缓冲三池、空闲链表、生命周期与链遍历；
 *   fc.c（本文件）—— 队列/调度器、提交/填充/完成、数据路径编排、服务主循环、诊断；
 *   fc_pool.h / fc_internal.h —— 池私有契约 / 控制器与调度器共享定义
 *                                （私有头在 src/fc/internal/，FTL 不可见）。
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数；句柄取 cmd->cmd_id → 逐条
 *   fc_command_configure_descriptor(cmd_id, ...) 组装 desc 链
 *   → fc_submit(cmd_id) → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release(cmd_id) 回收。
 *   （掩码建链与数据搬运在 FTL 侧：ftl_cmd_configure_descriptors / ftl_cmd_write_data）
 *
 * 文件结构（按职责分区，函数定义顺序即依赖顺序）：
 *    1. 静态资源        队列存储 + 返回暂存 + 填充计数 + g_fc_controller
 *    2. 初始化          fc_init（编排：池 + 队列 + NAND dispatch + CRC/RAID）
 *    3. 提交            fc_sched_submit
 *    4. 完成            fc_complete_from_nand
 *    5. Stripe RAID     fc_stripe_release_parity、fc_stripe_submit
 *    6. 调度/轮询       fc_poll_* 辅助 + fc_service
 *    7. CQ 出口         fc_poll_completion、fc_poll_completions
 *    8. 提交与诊断      submit/service/诊断（池相关公开 API 在 fc_pool.c）
 *
 * 服务主循环（fc_service）：
 *   ① fc_nand_dispatch_poll() 推进 NAND 层全部 LUN，每完成经 sink 当场 fc_complete_from_nand()
 *   ② 无完成 → fc_poll_fill() 一次扫描 HSQ/SQ，按 LUN 保序把能放下的命令（含链）
 *      提交进 NAND per-LUN FIFO（分派 fc_poll_dispatch_single() /
 *      fc_poll_dispatch_chain()；后者内部构建 request → submit/submit_chain，
 *      raid=ENCODE 走 fc_stripe_submit）
 *
 * 状态机：FREE → ALLOCATED → QUEUED → ACTIVE → COMPLETED。
 */

#include "fc.h"

#include "assertion.h"
#include "fc_crc_engine.h"
#include "fc_data_path.h"
#include "fc_ecc_engine.h"
#include "fc_internal.h"
#include "fc_nand_dispatch.h"
#include "fc_queue.h"
#include "fc_raid_engine.h"
#include "fc_random_engine.h"
#include "log.h"
#include "ssd_compiler.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════
 * 1. 静态资源
 *
 * 静态资源区：FC 控制面固定资源，当前位于默认 .bss（AXI SRAM）。
 * 不使用动态内存。后续根据 Cache 和 Buffer Pool 设计，
 * 可将各数组分别放入指定链接段。
 *
 *   池布局：
 *     command_pool / descriptor_pool / buffer_pool —— 三池本体
 *     descriptor_owner[desc] —— desc 归属的命令 ID（O(1) 反查）
 *     command_state / command_free_next —— 命令状态 + 空闲链表
 *     descriptor_free_next —— desc 空闲链表
 *     buffer_free_next / buffer_state —— buffer 空闲链表 + 占用状态
 *
 *   ⚠ 「一条命令拥有哪些 desc」**只有一份真值**：命令的 desc 链
 *     （command->desc_id + descriptor_pool[].next_id）。不设另一条
 *     「所有权链」（descriptor_owned_next[] + command_descriptor_head[]）为
 *     「desc 已分配但尚未挂进数据链」的中间态兜底，（省 640B）。
 *
 *   ⚠ **desc 出池即挂链**：fc_command_configure_descriptor 分配 desc 后
 *     先把字段填好、立刻 append，再分配 buffer。于是「已分配 desc 全部在链上」
 *     恒成立，失败时沿链释放即完备——不需要任何「链外半成品」的回滚逻辑
 *     （中间态本身不存在，不依赖就地回滚）。代价：链上允许存在
 *     buffer_id == FC_BUF_ID_INVALID 的 desc（FC_BUFFER_POOL_DEPTH < FC_DESCRIPTOR_POOL_DEPTH，
 *     这是最现实的失败点），所有走链的读者都要容忍它。
 * ══════════════════════════════════════════════════════════ */

static fc_cmd_id_t     g_fc_sq_storage[FC_SQ_DEPTH];
static fc_cmd_id_t     g_fc_hsq_storage[FC_HSQ_DEPTH];
static fc_cmd_status_s g_fc_cq_storage[FC_CQ_DEPTH];

// GET/SET_FEATURES 的 4 字节内联数据暂存，按 cmd_id（= 命令池槽位号）索引。
// 为什么需要：SQ 命令提交后不可写（见 fc_command.h「SQ 不可变」），而引擎需要一个
// 稳定的 4 字节落点写回寄存器值；返回值唯一的归宿是 CQ 的 param 联合体，
// 故这里只做提交→完成之间的中转。索引用的 cmd_id 是命令池槽位号，
// 恒 < FC_COMMAND_POOL_DEPTH。
// 与 buffer pool 同放 noncacheable 段：READ_META / GET_FEATURES 的 4 字节
// 返回段由引擎经 FMC 写回，避免 CPU 缓存与总线视图不一致。
static BYTE g_fc_return_staging[FC_COMMAND_POOL_DEPTH][4] NONCACHEABLE_BUFFER;

/* ── 排障计数（仅 debug 档）──
 * 声明、读写接口与条件编译集中在这里；调用点不再带 #if。
 * release 档的空函数在 -O2 下会被内联掉，零代码。 */
#ifndef NDEBUG
static DWORD s_fill_calls;
static DWORD s_fill_pops;

static void fc_fill_calls_inc(void)
{
    s_fill_calls++;
}

static void fc_fill_pops_inc(void)
{
    s_fill_pops++;
}

static void fc_fill_counters_read(DWORD *calls, DWORD *pops)
{
    *calls = s_fill_calls;
    *pops  = s_fill_pops;
}

static void fc_fill_counters_reset(void)
{
    s_fill_calls = 0U;
    s_fill_pops  = 0U;
}
#else
static void fc_fill_calls_inc(void)
{
}

static void fc_fill_pops_inc(void)
{
}

static void fc_fill_counters_read(DWORD *calls, DWORD *pops)
{
    (void)calls;
    (void)pops;
}

static void fc_fill_counters_reset(void)
{
}
#endif

// 全局单例控制器：定义在 fc.c，fc_pool.c 经 fc_internal.h 的 extern 访问；FTL 不可见
fc_ctl_s g_fc_controller;

/* ══════════════════════════════════════════════════════════
 * 2. 初始化
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 初始化 FC 控制器：清三池/队列/状态位图并挂指针
 *
 * 三池本体是文件作用域 static，地址在固件生命周期内固定；可重复调用以重置
 * （重置 fill 调试计数、池/队列、NAND dispatch 软件状态与引擎 armed 闩锁）。
 *
 * @note **调用前须确保 FC 已 quiesce**：本函数会清空在途命令/完成并 disarm NAND
 *       引擎；运行中调用会丢失未完成的命令。初始化没有失败路径（CRC 自检失败会
 *       直接 ASSERT），故不返回状态码。
 */
SSD_CODE_FAST void fc_init(void)
{
    fc_ctl_s *controller = &g_fc_controller;

    /* ── ① 控制器清零 + 三池/簿记/空闲链表（池本体在 fc_pool.c） ── */
    memset(controller, 0, sizeof(*controller));
    fc_fill_counters_reset();
    fc_pool_init(controller);

    /* ── ② 三条环形队列：SQ（普通命令）/ HSQ（高优先，如 read retry）/
     *    CQ（完成项）。init 只登记存储与深度，head/tail/count 归零。 ── */
    memset(g_fc_sq_storage, 0, sizeof(g_fc_sq_storage));
    memset(g_fc_hsq_storage, 0, sizeof(g_fc_hsq_storage));
    memset(g_fc_cq_storage, 0, sizeof(g_fc_cq_storage));
    memset(g_fc_return_staging, 0, sizeof(g_fc_return_staging));
    fc_submission_queue_init(&controller->sq, g_fc_sq_storage, FC_SQ_DEPTH);
    fc_submission_queue_init(&controller->hsq, g_fc_hsq_storage, FC_HSQ_DEPTH);
    fc_completion_queue_init(&controller->cq, g_fc_cq_storage, FC_CQ_DEPTH);

    /* ── ③ 调度器绑定 SQ/HSQ 指针（"忙"查 NAND 侧 fc_nand_dispatch_lun_free） ── */
    controller->scheduler.sq  = &controller->sq;
    controller->scheduler.hsq = &controller->hsq;

    /* ── ④ 本函数里唯一的 NAND 依赖：只清 NAND 侧的通道队列与 per-LUN 链状态
     *    （纯 memset，不碰 FMC 硬件）。硬件那半由 fc_nand_init() 负责，且在 main()
     *    里排在 fc_init() 之后——正因为这里不碰硬件，FC 才能先初始化。 ── */
    fc_nand_dispatch_init();

    // 初始化 RAID 引擎上下文（每个 LUN 独立）
#if CONFIG_FC_RAID
    for (BYTE lun = 0U; lun < LUN_NUM; lun++)
    {
        fc_raid_init_ctx(lun);
    }
#endif /* CONFIG_FC_RAID */

    /* ── ⑤ CRC 硬件（STM32H743 CRC 外设）：写路径编码时给每个 CW 算 CRC ── */
    fc_crc_init();

    // 初始化 Stripe RAID 积累状态
#if CONFIG_FC_RAID
    for (BYTE lun = 0U; lun < LUN_NUM; lun++)
    {
        memset(&controller->stripe_accum[lun], 0, sizeof(controller->stripe_accum[lun]));
    }
    for (WORD index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        controller->stripe_parity_for_data[index] = FC_CMD_ID_INVALID;
    }
#endif /* CONFIG_FC_RAID */
}

/* ══════════════════════════════════════════════════════════
 * 3. 提交
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 命令入队（按 high_priority 选 HSQ/SQ），状态置 QUEUED
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL
 */
static fc_status_e fc_sched_submit(fc_ctl_s *controller, fc_cmd_id_t cmd_id)
{
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    fc_cmd_s       *cmd   = &controller->command_pool[cmd_id];
    fc_sub_queue_s *queue = (cmd->high_priority != 0U) ? &controller->hsq : &controller->sq;
    ASSERT(cmd->cmd_id == cmd_id);

    // 重复提交由 fc_submit/fc_submit_batch 的 ALLOCATED 边界拒绝；本层是唯一入队点，
    // 只负责 push + 状态转移。
    if (fc_submission_queue_push(queue, cmd_id) != FC_STATUS_OK)
    {
        return FC_STATUS_QUEUE_FULL;
    }

    fc_pool_command_state_set(controller, cmd_id, FC_COMMAND_STATE_QUEUED);
    return FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 4. 完成
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief NAND 完成 → 回填 descriptor 状态/ECC → 数据路径 → 组装 CQ 完成项。
 *
 * @param controller FC 控制器
 * @param nand_completion NAND 层完成。
 *
 * @return FC_STATUS_OK / IO_ERROR（CQ 满强制 COMPLETED 防泄漏）。
 *
 * @note Step 1：把 nand_completion 的 per-CW status 写回 desc；ecc_bits 先采用完成项输入，
 * page read 随后由数据路径的实际解码结果覆盖。
 * - Step 2：Page Read 命令做 data path（ECC 解码 + 条件解扰）。
 * - Step 3：组装完成项——page read 从 desc 聚合状态和实际 ECC 计数
 * （失败坐标由 desc 链派生），并把 desc->ecc_bits 回填到 completion.ecc_bit_count[]；
 * 非 page read 直接透传 NAND 状态。
 * - CQ 满 → 强制 COMPLETED（数据路径已跑，CQ entry 丢失是 fatal）。
 */
static fc_status_e fc_complete_from_nand(fc_ctl_s *controller, fc_nand_dispatch_comp_s *nand_completion)
{
    fc_cmd_status_s cq_entry;
    fc_cmd_s       *cmd;
    BOOL            data_path_failed = false;

    // 完成项由 nand_dispatch 侧产出，cmd_id 必在池范围内
    ASSERT(nand_completion != NULL);
    ASSERT(nand_completion->cmd_id < FC_COMMAND_POOL_DEPTH);

    cmd = &controller->command_pool[nand_completion->cmd_id];
    memset(&cq_entry, 0, sizeof(cq_entry));

    // 完成项来自 NAND 层，ID 必须回到同一个命令对象；不一致说明调度/完成关联错位。
    ASSERT_REJECT(cmd->cmd_id == nand_completion->cmd_id,
                  FC_STATUS_IO_ERROR,
                  "complete: cmd=%hu field cmd_id=%hu mismatch",
                  nand_completion->cmd_id,
                  cmd->cmd_id);

    ASSERT_REJECT(controller->command_state[nand_completion->cmd_id] == FC_COMMAND_STATE_ACTIVE,
                  FC_STATUS_IO_ERROR,
                  "complete: cmd=%hu not ACTIVE",
                  nand_completion->cmd_id);

    /* ── Step 1: Write back per-CW status and ECC count from NAND completion ──
     * cw_status[] 的下标是 **flat 绝对 CW 号 flat_cw = plane*CW_NUM + cw**（与请求掩码同空间），
     * 故这里按 desc 自己的 PAA 算出 flat_cw 直接取。 */
    if (cmd->desc_id != FC_DESC_ID_INVALID)
    {
        fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;

        for (WORD steps = 0U; (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH); steps++)
        {
            fc_desc_s *descriptor = &controller->descriptor_pool[desc_id];
            BYTE       flat_cw    = fc_cw_flat(paa_to_plane(descriptor->paa), paa_to_cw(descriptor->paa));

            ASSERT(flat_cw < CW_TOTAL_MAX);
            descriptor->status   = nand_completion->cw_status[flat_cw];
            descriptor->ecc_bits = nand_completion->ecc_bit_count[flat_cw];
            desc_id              = (fc_desc_id_t)descriptor->next_id;
        }
        ASSERT_REJECT(desc_id == FC_DESC_ID_INVALID,
                      FC_STATUS_IO_ERROR,
                      "complete: desc chain not terminated cmd=%hu",
                      nand_completion->cmd_id);
    }

    /* ── Step 2: Data path processing (ECC decode + conditional descramble) ──
     * 仅对 Page Read 命令执行。READ_ID / GET_FEATURES 等命令
     * 返回的数据不是 ECC 编码的 NAND 页数据，不应进入 Data Path。 */
    // 合成失败（execution_status == NOT_READY）没有可解码的数据，且 buffer 可能未分配；
    // 不要进 ECC 解码路径，直接按失败 completion 上报。
    if ((nand_operation_is_page_read(cmd->config.operation) == true) && (nand_completion->execution_status != NAND_RESULT_NOT_READY))
    {
        const fc_status_e data_result = fc_data_path_process_read(cmd->cmd_id);

        // process_read 失败（UNC / ECC 引擎不可用 / buffer 缺失 / 链损坏）都必须
        // 反映到命令级状态，不能只看 desc->status 是否被填过。
        data_path_failed = (data_result != FC_STATUS_OK);
    }

    /* ── Step 3: Populate completion entry ──
     * CQ 只带命令级聚合：status + 返回段 param。per-CW ECC 只留在 desc 链，
     * 聚合 ECC 走 fc_get_cmd_ecc_max()（须在 fc_command_release() 前调用）。 */
    cq_entry.cmd_id = cmd->cmd_id;
    cq_entry.status = 0U;
    /* param 已由上面的整体 memset 清零；仅 GET_FEATURES / OP_READ_META /
     * OP_READ_ONFI_SIGNATURE / OP_READ_COLUMN / BLOCK_LOCK_STATUS 会再填。 */

    /* 命令级状态 = NAND 前/后两次读状态的位 + 数据路径聚合。
     * 前一次（pre_status）：RDY/ARDY/WP —— 命令下发时的器件状态；
     * 后一次（nand_status）：FAIL/FAILC —— 本次操作结果。
     * 命令成功 = 错误位全 0，**不是 RDY**。 */
    {
        const BYTE pre_status  = nand_completion->pre_status;
        const BYTE post_status = nand_completion->nand_status;
        const BYTE exec_status = nand_completion->execution_status;
        DWORD      cmd_status  = 0U;

        // post 不一定是 70h 状态寄存器：BLOCK_LOCK_STATUS 的 7Ah 字节也经
        // DATA_OUT_STATUS 进 nand_status（bit0 是 LT，不是 FAIL），不能按状态位映射。
        const BOOL post_is_70h = (cmd->config.operation != OP_BLOCK_LOCK_STATUS);

        if ((post_is_70h == true) && ((post_status & NAND_STATUS_FAIL) != 0U))
        {
            cmd_status |= FC_CMD_STATUS_NAND_FAIL;
        }
        // FAILC 只在 cache program 的**中间页/末页**有意义；首页（49 与 50）与非 cache op
        // 的状态字节里 bit1 可能是上一批残留，不能映射（与 CHECK_FAIL_CACHE 的
        // 分 op 判据一致：FIRST 只查 FAIL）。
        const BOOL failc_valid = (cmd->config.operation == OP_TWO_PLANE_PROGRAM_CACHE) || (cmd->config.operation == OP_TWO_PLANE_PROGRAM_CACHE_END) || (cmd->config.operation == OP_PROGRAM_PAGE_CACHE);

        if ((post_is_70h == true) && (failc_valid == true) && ((post_status & NAND_STATUS_FAILC) != 0U))
        {
            cmd_status |= FC_CMD_STATUS_FAIL_CACHE;
        }
        if ((pre_status & NAND_STATUS_READY) != 0U)
        {
            cmd_status |= FC_CMD_STATUS_RDY;
        }
        if ((pre_status & NAND_STATUS_ARDY) != 0U)
        {
            cmd_status |= FC_CMD_STATUS_AREADY;
        }
        if ((pre_status & NAND_STATUS_WP) != 0U)
        {
            cmd_status |= FC_CMD_STATUS_WP;
        }

        if (exec_status == NAND_RESULT_TIMEOUT)
        {
            cmd_status |= FC_CMD_STATUS_TIMEOUT;
        }
        else if ((exec_status != NAND_RESULT_OK) && ((cmd_status & (FC_CMD_STATUS_NAND_FAIL | FC_CMD_STATUS_FAIL_CACHE)) == 0U))
        {
            // 构建/提交失败注入（exec_status = NOT_READY）没有状态字节：按 NAND_FAIL 上报。
            cmd_status |= FC_CMD_STATUS_NAND_FAIL;
        }

        if (cmd->desc_id != FC_DESC_ID_INVALID)
        {
            // per-CW：先把 NAND 的 per-CW 结果并进 desc，再把 desc 聚合成命令级。
            // Step 2 的 data path 会覆盖 desc->status；这里把 Step 1 写入的原始
            // NAND 状态位补回来（page read 必需；非 page read 是幂等 OR）。
            // **不只 page read**：program 的 desc 也要拿到 FAIL，否则 FTL 的程序失败
            // 平面归因只能回退 plane0（ftl.c 的 bad_plane_mask）。
            // 失败类别（UNC/CRC/META/RAID）只留在 desc；命令级只保留 DATA_ERROR/META。
            fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
            BYTE         steps   = 0U;

            while ((desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < CW_TOTAL_MAX))
            {
                fc_desc_s *desc    = &controller->descriptor_pool[desc_id];
                // cw_status[] 是 flat 绝对编号（plane*CW_NUM+cw）；desc 链可能只覆盖子集
                // （flat 掩码读），不能拿链序 steps 直接当索引。
                const BYTE flat_cw = fc_cw_flat(paa_to_plane(desc->paa), paa_to_cw(desc->paa));

                // ECC 纠正 bit 数的生产者是 FC 数据路径（Step 2 已写入 desc->ecc_bits）；
                // 这里按同一 flat 下标回填完成项，NAND dispatch 不产生该字段。
                nand_completion->ecc_bit_count[flat_cw] = desc->ecc_bits;
                desc->status |= nand_completion->cw_status[flat_cw];

                if ((desc->status & FC_CW_STATUS_FAIL) != 0U)
                {
                    cmd_status |= FC_CMD_STATUS_NAND_FAIL;
                }
                if ((desc->status & FC_CW_STATUS_META_ERROR) != 0U)
                {
                    cmd_status |= FC_CMD_STATUS_META_ERROR;
                }
                if ((desc->status & (FC_CW_STATUS_UNC | FC_CW_STATUS_CRC_ERROR | FC_CW_STATUS_RAID_ERROR)) != 0U)
                {
                    cmd_status |= FC_CMD_STATUS_DATA_ERROR;
                }
                desc_id = (fc_desc_id_t)desc->next_id;
                steps++;
            }
            ASSERT_REJECT(desc_id == FC_DESC_ID_INVALID,
                          FC_STATUS_IO_ERROR,
                          "complete: desc chain not terminated (aggregate) cmd=%hu",
                          nand_completion->cmd_id);
        }

        if (data_path_failed == true)
        {
            cmd_status |= FC_CMD_STATUS_DATA_ERROR;
        }

        // 非 page read：命令级状态已由上面的 NAND 位给出；下面只处理特殊返回段。
        // OP_BLOCK_LOCK_STATUS：7Ah 的状态字节由引擎直读 DQ 总线送进
        // nand_completion->nand_status（NAND_UOP_DATA_OUT_STATUS），不经数据缓冲 ⇒ 只能从这里取。
        if (cmd->config.operation == OP_BLOCK_LOCK_STATUS)
        {
            cq_entry.param.parameter0 = nand_completion->nand_status;
        }
        // GET_FEATURES / OP_READ_META / OP_READ_ONFI_SIGNATURE / OP_READ_COLUMN：引擎把
        // 寄存器值 / 页内 META / 器件 ONFI 签名 / 按列读的 4 字节写进控制器侧暂存，拷进 CQ
        // 返回段（都用 4 字节暂存 g_fc_return_staging）。
        else if ((cmd->config.operation == OP_GET_FEATURES) || (cmd->config.operation == OP_READ_META) || (cmd->config.operation == OP_READ_ONFI_SIGNATURE) || (cmd->config.operation == OP_READ_COLUMN))
        {
            memcpy(&cq_entry.param.value, g_fc_return_staging[nand_completion->cmd_id], 4U);
        }

        cq_entry.status = cmd_status;
    }

    /* ── Step 4: buffer 归属 = FC 释放（buf_own bit0 = 1，即 01/11）时当场归还 ──
     * 完成项已组装完毕（Step 3 之后没有任何读者碰 buffer），此处归还并把各 desc 的
     * buffer_id 抹成 INVALID ⇒ 随后 fc_command_release 沿链释放时自然跳过，不双重归还。
     * 00/10（上层释放）不在这里动。放在 CQ push 之前，
     * 顺带覆盖 CQ 满强制 COMPLETED 的 fatal 分支。 */
    if ((cmd->config.buf_own & FC_BUF_OWN_FREE_BY_FC) != 0U)
    {
        fc_pool_command_release_buffers(controller, cmd->cmd_id);
    }

    {
        const fc_status_e complete_status = fc_completion_queue_push(&controller->cq, &cq_entry);

        if (complete_status == FC_STATUS_OK)
        {
            fc_pool_command_state_set(controller, nand_completion->cmd_id, FC_COMMAND_STATE_COMPLETED);
            return FC_STATUS_OK;
        }
        if (complete_status == FC_STATUS_QUEUE_FULL)
        {
            // CQ 满：完成项没有进 CQ，FTL 永远不会收到这条命令；这不是“防泄漏”，
            // 而是 fatal 的完成丢失。保留 COMPLETED 只是避免 FC 侧命令槽永久 ACTIVE，
            // 上层必须把本轮 IO_ERROR 当系统故障处理。
            LOGE(LOG_TAG_RES "CQ full: completion lost for cmd=%hu\r\n", nand_completion->cmd_id);
            controller->fatal = true;
            fc_pool_command_state_set(controller, nand_completion->cmd_id, FC_COMMAND_STATE_COMPLETED);
            return FC_STATUS_IO_ERROR;
        }
        return complete_status;
    }
}

/* ══════════════════════════════════════════════════════════
 * 5. Stripe RAID 辅助函数
 *
 * fc_stripe_release_parity()  — 回收 parity 命令的 desc+buffer+command 资源
 * fc_stripe_submit()         — 分配 parity 命令、配置、XOR、构建 MP request、submit
 *
 * 编译开关：CONFIG_FC_RAID (include/fc/fc.h)
 * ══════════════════════════════════════════════════════════ */

#if CONFIG_FC_RAID

/**
 * @brief 回收 parity 命令占用的所有资源（desc 链 + buffer + command 槽位）。
 *
 * @param controller FC 控制器
 * @param parity_id parity 命令 ID。
 *
 * @note 在 NAND completion 路径中调用（parity 数据已完成使命），
 * 把 desc/buffer 挂回空闲链，command 槽位置 FREE 回池。
 */
static void fc_stripe_release_parity(fc_ctl_s *controller, fc_cmd_id_t parity_id)
{
    // 层内 helper：唯一调用者是完成路径的 stripe 清理，parity_id 取自
    // stripe_parity_for_data[]（注册时即 fc_command_alloc() 铸出的池内 ID）。
    ASSERT(parity_id < FC_COMMAND_POOL_DEPTH);

    // 与 fc_pool_command_release 完全同义（desc 链 + buffer + 命令槽位），
    // 直接复用，不另写拷贝。parity 未单独提交，状态应为 ALLOCATED。
    fc_pool_command_release(controller, parity_id);
}

/**
 * @brief 处理 raid=ENCODE 命令的 Stripe 提交路径：
 * ① 分配 parity 命令（command + desc 链 + buffer）
 * ② 配置 parity 命令的 OP 码和 PAA（plane = PLANE_NUM-1）
 * ③ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity
 * ④ 构建 Multi-Plane dispatch request（plane0=data, plane1=parity）
 * ⑤ fc_nand_dispatch_submit()
 *
 * 成功时 parity 命令由 completion 路径自动释放。
 * 失败时已分配资源全部回滚 + 数据命令以 error completion 返回。
 *
 * @param controller FC 控制器
 * @param data_cmd 触发 stripe 的数据命令
 * @param data_cmd_id 数据命令的池内 ID
 *
 * @return FC_STATUS_OK = parity 命令已提交
 *         FC_STATUS_IO_ERROR = 任一步失败，已回滚并给数据命令注 error completion
 *
 * @note 仅处理 PLANE_NUM=2 场景（单数据命令 + 单 parity 命令）。
 */
static fc_status_e fc_stripe_submit(fc_ctl_s *controller, fc_cmd_id_t data_cmd_id)
{
    ASSERT(data_cmd_id < FC_COMMAND_POOL_DEPTH);

    fc_cmd_s *data_cmd = &controller->command_pool[data_cmd_id];
    ASSERT(data_cmd->cmd_id == data_cmd_id);
    fc_cmd_s              *parity_cmd;
    fc_cmd_id_t            parity_id;
    fc_nand_dispatch_req_s mp_req;
    BYTE                   cw_count;
    BYTE                   cw;
    fc_desc_id_t           desc_id;
    fc_cmd_id_t            data_ids[FC_STRIPE_DATA_MAX];

    cw_count = (data_cmd->config.cw_count + 1U);

    // RAID/stripe 的 XOR 按 CW 配对：两个 plane 必须访问**同一组 CW**，
    // flat 掩码（plane 不对称）与 stripe 语义不兼容，直接断言拒绝。
    // 掩码由 **desc 链重建**（链是 CW 集合的唯一真值，不回读 FTL 的输入字段）。
    ASSERT(fc_cw_bitmap_is_plane_symmetric(fc_get_cmd_cw_bitmap(data_cmd_id)));

    // ① 分配 parity cmd
    parity_cmd = fc_command_alloc();
    if (parity_cmd == NULL)
    {
        goto stripe_error;
    }
    parity_id = parity_cmd->cmd_id;

    // ② 配置 parity cmd
    parity_cmd->config           = data_cmd->config;
    parity_cmd->config.raid      = FC_RAID_MODE_NORMAL;         /* parity 命令不需要 RAID */
    parity_cmd->config.operation = OP_PROGRAM_PAGE_MULTI_PLANE; /* 提交时走 Multi-Plane */
    // parity 是 FC 内部命令（buffer 由 FC 申请、FC 释放），不继承 data_cmd 的 buf_own——
    // 否则 data_cmd 若是「上层申请」档，建链入口会因拿不到上层 buffer_id 而断言。
    parity_cmd->config.buf_own   = FC_BUF_OWN_FC_ALLOC_FC_FREE;
    parity_cmd->high_priority    = data_cmd->high_priority;
    parity_cmd->meta             = data_cmd->meta;

    // 调整 PAA：same page/block/lun/cw，plane = PLANE_NUM-1。
    // ch/ce 不在 PAA 里——parity 命令的路由取自它自己的 config（上面已从 data_cmd 复制）。
    parity_cmd->paa = paa_with_plane(data_cmd->paa, PLANE_NUM - 1U);

    // ③ 分配 parity desc + buffer
    // parity 命令是**单 plane**（plane = PLANE_NUM-1），CW 集合 = 连续 0..config.cw_count。
    // 「按掩码建链」在 FTL，FC 内没有掩码枚举；RAID 是 FC 内部机制，
    // 建链只有公共入口 fc_command_configure_descriptor，这里逐条调它挂链
    // （parity 恒为连续 0..config.cw_count，等价于 FTL 侧整段位图 fc_command_plane_span_cw_bitmap）。
    // ⚠ 该入口固定使用单例 g_fc_controller；RAID 暂未启用（CONFIG_FC_RAID=0），
    // 将来启用多实例前需先把这条路径改回 controller 参数化入口。
    // 行地址：同一 plane 内所有 CW 共用 parity_cmd->paa（page/block/lun）。
    {
        const BYTE  parity_plane = paa_to_plane(parity_cmd->paa);
        const DWORD parity_paa   = parity_cmd->paa;
        BYTE        pcw;

        for (pcw = 0U; pcw <= parity_cmd->config.cw_count; pcw++)
        {
            if (fc_command_configure_descriptor(parity_cmd, parity_plane, pcw, parity_paa) != FC_STATUS_OK)
            {
                // 回滚半成品：fc_pool_command_release 一次做完 desc 链 + buffer + 命令槽位。
                fc_pool_command_release(controller, parity_id);
                goto stripe_error;
            }
        }
    }

    // 写入 parity descriptors 的 meta（与数据命令对应 CW 一致）
    {
        fc_desc_id_t data_desc_id   = (fc_desc_id_t)data_cmd->desc_id;
        fc_desc_id_t parity_desc_id = (fc_desc_id_t)parity_cmd->desc_id;

        for (cw = 0U; cw < cw_count; cw++)
        {
            fc_desc_s *pdesc = fc_descriptor_id_to_descriptor(parity_desc_id);
            fc_desc_s *ddesc = fc_descriptor_id_to_descriptor(data_desc_id);
            if ((pdesc != NULL) && (ddesc != NULL))
            {
                pdesc->meta = ddesc->meta;
            }
            parity_desc_id = (pdesc != NULL) ? (fc_desc_id_t)pdesc->next_id : FC_DESC_ID_INVALID;
            data_desc_id   = (ddesc != NULL) ? (fc_desc_id_t)ddesc->next_id : FC_DESC_ID_INVALID;
        }
    }

    // ④ fc_data_path_generate_stripe_parity() — XOR + ECC 编码 parity
    data_ids[0U] = data_cmd_id;
    if (fc_data_path_generate_stripe_parity(data_ids, 1U, parity_id) != FC_STATUS_OK)
    {
        fc_stripe_release_parity(controller, parity_id);
        goto stripe_error;
    }

    // ⑤ 构建 Multi-Plane dispatch request
    memset(&mp_req, 0, sizeof(mp_req));
    mp_req.cmd_id      = data_cmd_id; /* 完成路由回数据命令 */
    mp_req.operation   = OP_PROGRAM_PAGE_MULTI_PLANE;
    mp_req.ch          = data_cmd->ch;
    mp_req.ce          = data_cmd->ce;
    mp_req.lun         = data_cmd->lun;
    mp_req.plane_count = PLANE_NUM;
    mp_req.plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
    mp_req.page_type   = data_cmd->config.page_type;
    // 两 plane 的行地址 + 掩码：stripe 命令 plane 对称（见上方 ASSERT），
    // 两 plane 访问同一组 CW → 掩码 = 两组相同的低位。
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        mp_req.plane_paa[p] = paa_with_plane(data_cmd->paa, p);
        for (cw = 0U; cw < cw_count; cw++)
        {
            mp_req.cw_bitmap |= (1UL << fc_cw_flat(p, cw));
        }
    }

    // 填充 plane 0（= 数据命令）的 CW buffer
    desc_id = (fc_desc_id_t)data_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        // desc_id 由链遍历产生，恒在池内——越界即链损坏 = 调用者 bug，转 ASSERT
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        const fc_desc_s *desc = &controller->descriptor_pool[desc_id];

        mp_req.cw_buffers[cw] = (desc->buffer_id != FC_BUF_ID_INVALID) ? fc_buffer_get_base(desc->buffer_id) : NULL;
        desc_id               = (fc_desc_id_t)desc->next_id;
    }

    // 填充 plane 1（= parity 命令）的 CW buffer
    desc_id = (fc_desc_id_t)parity_cmd->desc_id;
    for (cw = 0U; cw < cw_count; cw++)
    {
        ASSERT(desc_id < FC_DESCRIPTOR_POOL_DEPTH);
        const fc_desc_s *desc = &controller->descriptor_pool[desc_id];

        mp_req.cw_buffers[CW_NUM + cw] = (desc->buffer_id != FC_BUF_ID_INVALID) ? fc_buffer_get_base(desc->buffer_id) : NULL;
        desc_id                        = (fc_desc_id_t)desc->next_id;
    }

    // 注册 parity 命令到数据命令的清理映射表
    controller->stripe_parity_for_data[data_cmd_id] = parity_id;

    // ⑥ fc_nand_dispatch_submit()
    if (fc_nand_dispatch_submit(&mp_req) != NAND_RESULT_OK)
    {
        controller->stripe_parity_for_data[data_cmd_id] = FC_CMD_ID_INVALID;
        fc_stripe_release_parity(controller, parity_id);
        goto stripe_error;
    }

    return FC_STATUS_OK;

stripe_error:
    // 数据命令注 error completion，让 FTL 感知失败
    {
        fc_nand_dispatch_comp_s fail_comp;

        memset(&fail_comp, 0, sizeof(fail_comp));
        fail_comp.cmd_id           = data_cmd_id;
        fail_comp.execution_status = NAND_RESULT_NOT_READY;
        fail_comp.cw_status[0]     = FC_CW_STATUS_FAIL;
        if (fc_complete_from_nand(controller, &fail_comp) != FC_STATUS_OK)
        {
            controller->fatal = true;
        }
    }
    return FC_STATUS_IO_ERROR;
}

#endif /* CONFIG_FC_RAID */

/* ══════════════════════════════════════════════════════════
 * 6. 调度 / 轮询
 *
 * 服务主循环 fc_service 的两条提交路径拆成独立 helper：
 *   fc_poll_dispatch_single — 单命令（count==1）
 *   fc_poll_dispatch_chain  — 链式（count>=2）
 * 两者共用 fc_build_dispatch_request 构建 request；
 * 失败兜底统一走 fc_poll_fail_command。
 *
 * 行为不对称（既有，勿统一）：
 *   - 链式路径构建失败时从循环体内直接返回，放弃整条链。
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 以 NAND_FAIL 合成完成项，把命令注入完成队列（poll 内失败兜底）。
 *
 * @param controller FC 控制器
 * @param cmd_id 失败命令。
 *
 * @note request 构建失败 / submit 失败共用；execution_status 用
 * NOT_READY 表示"未能提交"。失败位必须按命令实际的 flat CW 集合落位；
 * 固定写 cw_status[0] 对 plane1 / CW>0 的命令会被完成回填静默丢弃。
 */
static void fc_poll_fail_command(fc_ctl_s *controller, fc_cmd_id_t cmd_id)
{
    fc_nand_dispatch_comp_s fail_comp;
    const DWORD             cw_bitmap = fc_get_cmd_cw_bitmap(cmd_id);

    memset(&fail_comp, 0, sizeof(fail_comp));
    fail_comp.cmd_id           = cmd_id;
    fail_comp.execution_status = NAND_RESULT_NOT_READY;

    if (cw_bitmap == 0U)
    {
        // 无 desc 的命令没有 per-CW 坐标；保留一个命令级失败落点。
        fail_comp.cw_status[0] = FC_CW_STATUS_FAIL;
    }
    else
    {
        for (BYTE n = 0U; n < CW_TOTAL_MAX; n++)
        {
            if ((cw_bitmap & (1UL << n)) != 0U)
            {
                fail_comp.cw_status[n] = FC_CW_STATUS_FAIL;
            }
        }
    }

    if (fc_complete_from_nand(controller, &fail_comp) != FC_STATUS_OK)
    {
        controller->fatal = true;
    }
}

/**
 * @brief 沿 desc 链把 CW 落位到 request 的 cw_bitmap / cw_buffers[] / plane_paa[]。
 *
 * 前置：链已由 fc_pool_chain_validate 在提交边界校验（归属 / plane 段 / CW 唯一性 /
 * buffer 边界与归属 / 同 plane 行一致 / multi-plane 覆盖 / multi_plane 范围）。
 * 本函数只做 request 落位；残留失败是无 desc 链、buffer INVALID、链损坏/成环、
 * multi_plane 越界（循环上界保护）。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID
 * @param request 输出（header 已填、全零）。
 *
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT / FC_STATUS_IO_ERROR（链损坏/成环）。
 *
 * @note 每个 desc 落位到 **flat 绝对 CW 号 fc_cw_flat(plane, cw)** 那一格：置
 * request->cw_bitmap 的 bit n、写 request->cw_buffers[n]、按 plane 填 plane_paa[]。
 * 链长度即 CW 数（popcount）；plane_paa[] 是 per-plane 行地址，Policy B 下各 plane
 * 可以指向不同 block/page。只由 fc_build_dispatch_request 的“带数据命令”分支调用。
 */
static fc_status_e fc_build_request_data_planes(const fc_ctl_s *controller, fc_cmd_id_t cmd_id, fc_nand_dispatch_req_s *request)
{
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    const fc_cmd_s *cmd = &controller->command_pool[cmd_id];
    ASSERT(cmd->cmd_id == cmd_id);

    // 带数据命令必须有 desc 链；无链的 has_data 命令（READ_STATUS 等）由引擎内部
    // 直接发 70h/78h，不应走 FC 提交路径。
    if (cmd->desc_id == FC_DESC_ID_INVALID)
    {
        return FC_STATUS_INVALID_ARGUMENT;
    }

    const BOOL need_cw_base = (nand_operation_is_page_read(cmd->config.operation) == true) ||
                              (nand_operation_is_page_write(cmd->config.operation) == true);
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    WORD         steps   = 0U;

    // 链遍历必须有界：成环时不能把 dispatch 线程挂死。
    while ((desc_id != FC_DESC_ID_INVALID) && (desc_id < FC_DESCRIPTOR_POOL_DEPTH) && (steps < FC_DESCRIPTOR_POOL_DEPTH))
    {
        const fc_desc_s *desc  = &controller->descriptor_pool[desc_id];
        const BYTE       plane = paa_to_plane(desc->paa);
        const BYTE       n     = fc_cw_flat(plane, paa_to_cw(desc->paa));

        /* 链的坐标/重复/buffer/行一致/覆盖已由 fc_pool_chain_validate 在提交边界校验；
         * 这里直接按已验证的链落位，不再重复断言。 */

        // 带数据命令的每个 desc 都必须有 buffer；INVALID 是建链失败后的半成品，
        // 不能把 NULL 指针交给 NAND 层（release 会 memcpy(NULL)）。
        if (desc->buffer_id == FC_BUF_ID_INVALID)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }

        request->cw_bitmap |= (1UL << n);
        // Page read/write 用 CW 基址（ECC 处理完整 528B）；非 page 用 USER 区。
        request->cw_buffers[n]    = need_cw_base ? fc_buffer_get_base(desc->buffer_id) : fc_buffer_get_user_data(desc->buffer_id);
        // 行地址 per-plane：同一 plane 的 desc 已由 chain_validate 保证行一致，
        // 逐条覆盖写入同一个值即可。
        request->plane_paa[plane] = paa_with_plane(paa_page_base(desc->paa), plane);
        desc_id                   = (fc_desc_id_t)desc->next_id;
        steps++;
    }
    if (desc_id != FC_DESC_ID_INVALID)
    {
        return FC_STATUS_IO_ERROR;
    }

    if (cmd->config.multi_plane > 0U)
    {
        const BYTE plane_count = cmd->config.multi_plane;

        /* 链覆盖已由 chain_validate 保证；这里只防循环/数组越界。 */
        if (plane_count > PLANE_NUM)
        {
            return FC_STATUS_INVALID_ARGUMENT;
        }
        request->plane_count = plane_count;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
    }
    else
    {
        // Single-plane：地址取自 cmd->paa（pba_to_cmd 单 plane 路径直接写它，
        // GC/WL 的单 plane 写也走这条）；chain_validate 保证 desc 行与 cmd->paa 一致。
        request->plane                     = paa_to_plane(cmd->paa);
        request->plane_paa[request->plane] = cmd->paa;
    }

    return FC_STATUS_OK;
}

/**
 * @brief 把 request 的返回段指向控制器侧 4 字节暂存（FEATURES / META / ONFI / COLUMN 共用）。
 *
 * @param request 目标请求（plane / cw_bitmap / cw_buffers 由本函数填）。
 * @param plane   目标 plane。
 * @param cmd_id  命令 ID（暂存下标）。
 *
 * @note 只置该 plane 的 CW0 位：返回段恒为 4 字节。
 */
static void fc_request_use_staging(fc_nand_dispatch_req_s *request, BYTE plane, fc_cmd_id_t cmd_id)
{
    ASSERT(plane < PLANE_NUM);
    ASSERT(cmd_id < FC_COMMAND_POOL_DEPTH);

    const BYTE cw0 = fc_cw_flat(plane, 0U);

    ASSERT(cw0 < CW_TOTAL_MAX);

    request->plane           = plane;
    request->cw_bitmap       = (1UL << cw0);
    request->cw_buffers[cw0] = g_fc_return_staging[cmd_id];
}

/**
 * @brief 由一条已 ACTIVE 的命令构建自包含的 NAND dispatch 请求。
 *
 * @param controller FC 控制器
 * @param cmd_id 命令 ID
 * @param cmd_id 命令 ID
 * @param request 输出请求。
 *
 * @return FC_STATUS_OK / FC_STATUS_INVALID_ARGUMENT（无 desc 链 / buffer INVALID /
 *         multi_plane 越界）/ FC_STATUS_IO_ERROR（链损坏或成环）。
 *         链的归属 / plane 段 / CW 唯一 / buffer 边界与归属 / 行一致 / multi-plane
 *         覆盖已在提交边界由 fc_pool_chain_validate 校验。
 *
 * @note 填充顺序：公共头（operation / ch-ce-lun / page_type / feature_addr）→
 * 按 op 类别填 cw_bitmap / cw_buffers[] / plane_paa[]（细节见各分支注释）→
 * 末尾做 operation 改写（可拆写/擦 → ISSUE；读语义由 dispatch task_load 统一）。
 *
 * 全局不变量：
 * - 索引空间唯一：n = fc_cw_flat(plane, cw)；cw_bitmap 的 bit n、cw_buffers[n]、
 *   completion.cw_status[n] 同一编号。未写的格由 memset(request) 保证为 0。
 * - plane_paa[] 是 per-plane 行地址；命令 plane 集合内每个 plane 都填。
 * - page read/write 用 CW 基址（ECC 处理完整 528B），非 page 用 USER 区。
 * - 链的合法性（归属 / plane 段 / CW 唯一 / buffer 边界与归属 / 行一致 /
 *   multi-plane 覆盖）由 fc_pool_chain_validate 在提交边界保证。
 *
 * @note 本函数不做 META 预填——buffer[0..3] 的 META 由 fc_data_path_encode 的
 * Stage 0 写（两条派发路径都经 encode）。
 */
static fc_status_e fc_build_dispatch_request(fc_ctl_s *controller, fc_cmd_id_t cmd_id, fc_nand_dispatch_req_s *request)
{
    ASSERT_REJECT(cmd_id < FC_COMMAND_POOL_DEPTH,
                  FC_STATUS_IO_ERROR,
                  "build request: cmd=%hu out of range",
                  cmd_id);

    fc_cmd_s *cmd = &controller->command_pool[cmd_id];
    ASSERT(cmd->cmd_id == cmd_id);

    // Pre-resolve descriptor-chain data into a self-contained dispatch request.
    memset(request, 0, sizeof(*request));
    request->cmd_id       = cmd->cmd_id;
    request->operation    = cmd->config.operation;
    request->ch           = cmd->ch;
    request->ce           = cmd->ce;
    request->lun          = cmd->lun;
    request->page_type    = cmd->config.page_type;
    request->feature_addr = cmd->feature_addr;

    if (cmd->config.operation == OP_ERASE_BLOCK)
    {
        // 单 plane 擦除：目标 plane 取自 cmd->paa（本分支无 desc 可用）。
        // **不置 cw_bitmap**——擦除无逐 CW 数据，请求位图是合法空集；
        // completion_all_cws 对空集写 cw_status[0]，CQ 聚合照样读得到擦除成败。
        request->plane                     = paa_to_plane(cmd->paa);
        request->plane_paa[request->plane] = cmd->paa;
    }
    else if (cmd->config.operation == OP_ERASE_BLOCK_MULTI_PLANE)
    {
        BYTE plane;

        // op 语义 = 擦除配对块的全部 PLANE_NUM 个 plane；不读 config.multi_plane。
        // 需要只擦部分 plane 时用单 plane op。
        request->plane_count = PLANE_NUM;
        request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
        for (plane = 0U; plane < PLANE_NUM; plane++)
        {
            request->plane_paa[plane] = paa_with_plane(cmd->paa, plane);
        }
        // 不置 cw_bitmap（同单 plane 擦除）——多 plane 擦除的 plane 集合由
        // plane_count/plane_paa[] 表达，与 CW 位图无关。
    }
    else if ((cmd->config.operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) || (cmd->config.operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT))
    {
        // 无 desc 的「纯加载」op：只把目标页搬进器件页缓冲，**不建 CW 位图、不占 buffer**
        // —— op 表里 has_data = 0（host 侧无数据搬运），故 ftl_cmd_needs_desc_chain 不要求链。
        // 数据随后由**独立的** op 8（05h-E0h）/ op 41（06h-E0h）命令带 desc 链取回。
        // ⚠ 生产读路径**不走这里**：FC 命令恒为 op 12/30（has_data=1，走下面的 data_planes 分支
        // 建好链与位图），op 33/29 只作为 dispatch task_load 读改写的**目标**出现。
        // 本分支只服务「命令本身就是 33/29」的直接下发（测试用）。
        // 行地址取自 cmd->paa（无 desc 可用）：单 plane 取本 plane；多 plane 逐 plane 补 plane 位。
        if (cmd->config.operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT)
        {
            BYTE plane;

            request->plane_count = PLANE_NUM;
            request->plane       = 0U; /* multi-plane：起始 plane 恒 0（uop 硬编码 row[0]/row[1]） */
            for (plane = 0U; plane < PLANE_NUM; plane++)
            {
                request->plane_paa[plane] = paa_with_plane(cmd->paa, plane);
            }
        }
        else
        {
            request->plane                     = paa_to_plane(cmd->paa);
            request->plane_paa[request->plane] = cmd->paa;
        }
        // 不置 cw_bitmap（空集 = 无 CW 可访问）：加载 op 的 uop 无 DATA_OUT，位图/缓冲皆不用。
    }
    else if ((cmd->config.operation == OP_SET_FEATURES) || (cmd->config.operation == OP_GET_FEATURES))
    {
        // Feature 寄存器是 die 级、无 plane 语义；cmd->paa 从不设置（fc_command_alloc
        // 做 (fc_cmd_s){0}），故 plane 恒 0。内联数据落控制器侧暂存：SQ 命令体提交后
        // 不可写；SET 先拷入，GET 由引擎写回，完成时再拷进 CQ 的 param。
        fc_request_use_staging(request, paa_to_plane(cmd->paa), cmd_id);
        if (cmd->config.operation == OP_SET_FEATURES)
        {
            memcpy(g_fc_return_staging[cmd_id], &cmd->param.value, 4U);
        }
        else
        {
            memset(g_fc_return_staging[cmd_id], 0, 4U);
        }
    }
    else if (cmd->config.operation == OP_READ_META)
    {
        // META 只读回读：无 desc 链（has_data = 0），回读指定页、指定 CW 的前 4 字节
        // META（[META:4][CRC:4][USER:512][PARITY:8]，明文、在加扰区前）。
        // CW 由 cmd->paa 的 cw 字段选；NAND 侧据 paa_to_cw(plane_paa[plane]) * CW_SIZE
        // 算列地址，行地址取自同一 PAA。
        fc_request_use_staging(request, paa_to_plane(cmd->paa), cmd_id);
        request->plane_paa[request->plane] = cmd->paa;
        memset(g_fc_return_staging[cmd_id], 0, 4U);
    }
    else if (cmd->config.operation == OP_READ_ONFI_SIGNATURE)
    {
        // ONFI 签名读（90h + 地址 20h）：器件级命令，无 desc 链。
        // **不填 plane_paa**——列地址恒 0x20、行地址恒 0，与介质坐标无关；
        // cmd->paa 只用来定 plane。
        // ⚠ 必须有这一支：否则位图保持空集 ⇒ task_load 一格 cw_buffers[] 都不搬
        // ⇒ NAND 侧拿到残留指针，DATA_OUT 写向野地址。
        fc_request_use_staging(request, paa_to_plane(cmd->paa), cmd_id);
        memset(g_fc_return_staging[cmd_id], 0, 4U);
    }
    else if (cmd->config.operation == OP_READ_COLUMN)
    {
        // 按显式列读：无 desc 链。列地址不走 paa 的 cw 字段，而由命令级 param 的
        // 字节 0/1（小端）传入——cw 只有 2 位，够不到 spare 区（列 2048）；
        // 行地址仍取自 cmd->paa。
        // ⚠ 必须有这一支：否则位图空集 ⇒ task_load 不搬 cw_buffers[] ⇒ DATA_OUT 写野地址。
        fc_request_use_staging(request, paa_to_plane(cmd->paa), cmd_id);
        request->plane_paa[request->plane] = cmd->paa;
        request->column                    = (WORD)((WORD)cmd->param.parameter0 | ((WORD)cmd->param.parameter1 << 8U));
        memset(g_fc_return_staging[cmd_id], 0, 4U);
    }
    else if ((cmd->config.operation == OP_BLOCK_UNLOCK_LOW) || (cmd->config.operation == OP_BLOCK_UNLOCK_HIGH) || (cmd->config.operation == OP_BLOCK_LOCK) || (cmd->config.operation == OP_BLOCK_LOCK_TIGHT) || (cmd->config.operation == OP_BLOCK_LOCK_STATUS))
    {
        // 块锁族：**无 host 数据搬运**（op 表 has_data 全为 0），但 23h/24h/7Ah
        // 都带 3 个地址周期（R1/R2/R3）⇒ 行地址**必须取自 cmd->paa**，否则退化为
        // block 0（本分支之前这些命令落到末尾 else，plane_paa[] 保持 {0} 整体清零值，
        // 而 ROW_0 的 uop 正是 *address = request->row[operand]，见 nand_fmc.c）。
        // 2Ah/2Ch（BLOCK_LOCK / BLOCK_LOCK_TIGHT）无地址周期，纳入本分支只为把「块锁族」
        // 记在一处——对它们只是多填一个不会被下发的行地址。
        // 不置 cw_bitmap：7Ah 的状态字节走 DQ 总线直读（NAND_UOP_DATA_OUT_STATUS →
        // completion->nand_status，不写 request->buffer），不经任何数据缓冲。
        request->plane                     = paa_to_plane(cmd->paa);
        request->plane_paa[request->plane] = cmd->paa;
    }
    else if (nand_operation_has_data(cmd->config.operation))
    {
        const fc_status_e build_result = fc_build_request_data_planes(controller, cmd_id, request);
        if (build_result != FC_STATUS_OK)
        {
            // 失败（无 desc 链 / buffer INVALID / multi_plane 越界 / 链成环）交给
            // 调用方 fail 命令；request 可能只写了一半，调用者拿到非 OK 就不会提交它。
            return build_result;
        }
    }
    else
    {
        // 其余命令（RESET / BLOCK_LOCK / BLOCK_UNLOCK / OTP_DATA_LOCK 等 has_data == 0
        // 且无 desc 链的）：位图保持 0 = **合法空集**（无 CW 可访问）。擦除也走这条口径
        // （上面两个 ERASE 分支只填 plane_paa[]，CW 位保持空集）。
        // ⚠ 别把 READ_STATUS / READ_STATUS_ENHANCED 列进来——两者的 has_data 都是 1，
        // 实际走上面的 fc_build_request_data_planes 分支；它们只是从不由 FC 提交
        // （引擎 WAIT_READY 内部直接用 70h/78h uop）。
    }

    /* ── 下发前改写 operation：只处理多 LUN 的写/擦拆分 ──
     * 读语义（WITH_DATA_OUT → WITHOUT/LOAD）由 dispatch 的 task_load 统一改写，
     * FC 提交路径与直接 dispatch 路径共用同一入口，避免两套 read 改写漂移。 */
    if ((LUN_NUM > 1U) && nand_operation_is_splittable(request->operation))
    {
        request->operation = nand_operation_to_issue(request->operation);
    }

    return FC_STATUS_OK;
}

/**
 * @brief 下发失败兜底：链上每条命令都注 NAND_FAIL 完成项并返回 IO_ERROR。
 *
 * @param controller FC 控制器
 * @param chain 失败链；单命令路径的 count 恒为 1
 * @return 恒为 FC_STATUS_IO_ERROR
 */
static fc_status_e fc_poll_dispatch_abort(fc_ctl_s *controller, const fc_sched_chain_s *chain)
{
    for (BYTE i = 0U; i < chain->count; i++)
    {
        const fc_cmd_id_t cmd_id = chain->ids[i];
        fc_poll_fail_command(controller, cmd_id);
    }
    return FC_STATUS_IO_ERROR;
}

/**
 * @brief 提交调度器选出的单条命令（encode → 可选 RAID → build → submit）。
 *
 * @param controller FC 控制器
 * @param chain 调度器链（count==1）。
 *
 * @return FC_STATUS_OK；FC_STATUS_IO_ERROR（encode/build/submit 失败，命令已注 NAND_FAIL）。
 *
 * @note 命令已在调用方出队；本函数把它从 QUEUED 置 ACTIVE。任一步失败都必须走
 * fc_poll_fail_command 了结，否则命令永久 ACTIVE、FTL 等不到完成。
 * - page write：先 encode（Random + ECC，含 Stage 0 写 META）。
 * - raid=ENCODE：走 fc_stripe_submit（Multi-Plane），由它内部处理成功/失败。
 * - 其余：build request → fc_nand_dispatch_submit。
 * - 只负责占用目标 LUN；完成由后续 fc_service() 推进。
 * - 单命令路径；FTL 的 cache run 走 fc_poll_dispatch_chain。
 */
static fc_status_e fc_poll_dispatch_single(fc_ctl_s *controller, const fc_sched_chain_s *chain)
{
    const fc_cmd_id_t      cmd_id = chain->ids[0U];
    fc_nand_dispatch_req_s request;

    /* ── 1. 取得所有权：QUEUED → ACTIVE ──
     * cmd_id 范围已在出队点由 fc_command_id_to_command 校验；转移合法性由
     * fc_pool_command_state_set 的转移表唯一判定，这里不重复断言。 */
    fc_pool_command_state_set(controller, cmd_id, FC_COMMAND_STATE_ACTIVE);
    fc_cmd_s *cmd = &controller->command_pool[cmd_id];
    ASSERT(cmd->cmd_id == cmd_id);

    const BOOL is_write = nand_operation_is_page_write(cmd->config.operation) == true;

    /* ── 2. 写路径编码：Random 加扰 → ECC（RAID XOR 在 stripe 路径）── */
    if (is_write == true)
    {
        const fc_status_e encode_result = fc_data_path_encode(cmd_id);

        if (encode_result != FC_STATUS_OK)
        {
            LOGW("fc: data path encode failed for op=%hhu cmd_id=%hu status=%d\r\n", cmd->config.operation, cmd_id, encode_result);
            return fc_poll_dispatch_abort(controller, chain);
        }
    }

    /* ── 3. Stripe RAID：raid=ENCODE 的 page write 走 Multi-Plane ── */
#if CONFIG_FC_RAID
    if ((is_write == true) && (cmd->config.raid == FC_RAID_MODE_ENCODE))
    {
        return fc_stripe_submit(controller, cmd_id);
    }
#endif

    /* ── 4. 构建自包含 request 并提交 ── */
    const fc_status_e build_result = fc_build_dispatch_request(controller, cmd_id, &request);
    if (build_result != FC_STATUS_OK)
    {
        LOGW("fc: build request failed for op=%hhu cmd_id=%hu status=%d\r\n", cmd->config.operation, cmd_id, build_result);
        return fc_poll_dispatch_abort(controller, chain);
    }

    const BYTE submit_result = fc_nand_dispatch_submit(&request);

    // 填充轮次已按 lun_free 预留槽位；NOT_READY 属不变量破坏，debug 停机。
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        LOGW("fc: fc_nand_dispatch_submit failed for op=%hhu cmd_id=%hu cw_bitmap=0x%02lX plane_count=%hhu\r\n", cmd->config.operation, cmd_id, request.cw_bitmap, request.plane_count);
        return fc_poll_dispatch_abort(controller, chain);
    }

    return FC_STATUS_OK; /* 只占用 LUN；完成由后续 fc_service() 推进。 */
}

/**
 * @brief 提交调度器选出的链式命令（逐条 encode + build → submit_chain）。
 *
 * @param controller FC 控制器
 * @param chain 调度器链（count>=2）。
 *
 * @return FC_STATUS_OK；FC_STATUS_IO_ERROR（任一步失败，整链已注 NAND_FAIL）。
 *
 * @note 整链先统一置 ACTIVE，再逐条 encode/build；全部构建完成后一次 submit_chain。
 * 任一步失败都把整链注 NAND_FAIL 并返回 IO_ERROR，不留下「前缀 ACTIVE、后缀 QUEUED」
 * 的半状态。使用者：FTL 的 cache read / cache program run（同一 (ch,ce,lun)）。
 */
static fc_status_e fc_poll_dispatch_chain(fc_ctl_s *controller, const fc_sched_chain_s *chain)
{
    const BYTE             chain_count = chain->count;
    fc_nand_dispatch_req_s reqs[FC_SCHEDULER_CHAIN_MAX];
    BYTE                   i;

    /* ── 1. 整链统一取得所有权：QUEUED → ACTIVE ──
     * 链成员 ID 范围已在组链时（fc_poll_chain_length → fc_command_id_to_command）
     * 校验；转移合法性由 setter 的转移表唯一判定，这里不重复断言。 */
    for (i = 0U; i < chain_count; i++)
    {
        const fc_cmd_id_t cmd_id = chain->ids[i];

        fc_pool_command_state_set(controller, cmd_id, FC_COMMAND_STATE_ACTIVE);
    }

    /* ── 2. 逐条 encode + 构建 request ── */
    for (i = 0U; i < chain_count; i++)
    {
        const fc_cmd_id_t cmd_id = chain->ids[i];
        fc_cmd_s         *cmd    = &controller->command_pool[cmd_id];
        ASSERT(cmd->cmd_id == cmd_id);
        const BOOL is_write = nand_operation_is_page_write(cmd->config.operation) == true;

        if (is_write == true)
        {
            const fc_status_e encode_result = fc_data_path_encode(cmd_id);

            if (encode_result != FC_STATUS_OK)
            {
                LOGW("fc: data path encode failed for cmd_id=%hu (chain %hhu/%hhu) status=%d\r\n", cmd_id, i, chain_count, encode_result);
                return fc_poll_dispatch_abort(controller, chain);
            }
        }

        const fc_status_e build_result = fc_build_dispatch_request(controller, cmd_id, &reqs[i]);
        if (build_result != FC_STATUS_OK)
        {
            LOGW("fc: build request failed for cmd_id=%hu (chain %hhu/%hhu) status=%d\r\n", cmd_id, i, chain_count, build_result);
            return fc_poll_dispatch_abort(controller, chain);
        }
    }

    /* ── 3. 整链一次提交 ── */
    const BYTE submit_result = fc_nand_dispatch_submit_chain(reqs, chain_count);

    // 填充轮次已按 lun_free 预留整链槽位；NOT_READY 属不变量破坏，debug 停机。
    ASSERT(submit_result != NAND_RESULT_NOT_READY);
    if (submit_result != NAND_RESULT_OK)
    {
        LOGW("fc: fc_nand_dispatch_submit_chain failed for chain=%hhu status=0x%02hhX\r\n", chain_count, submit_result);
        return fc_poll_dispatch_abort(controller, chain);
    }

    return FC_STATUS_OK; /* 只占用 LUN；完成由后续 fc_service() 推进。 */
}

/**
 * @brief 计算队头命令所属链的长度（链头已弹出，peek 从队头看后续）。
 *
 * @param queue 提交队列，HSQ/SQ 通用（队头已是链头之后的下一条）
 * @param head 已弹出的链头命令。
 *
 * @return 链长（1..FC_SCHEDULER_CHAIN_MAX）。
 *
 * @note head->config.chain == FC_CHAIN_END → 单命令，返回 1。
 * - 否则 peek 后续，收集同一 (ch,ce,lun) 的连续命令，直到 chain==END 或链满。
 * - 只 peek 不消耗——真正弹出在 fc_poll_fill_queue 里，链放不下时可原样留在队列。
 */
static BYTE fc_poll_chain_length(const fc_ctl_s *controller, const fc_sub_queue_s *queue, fc_cmd_id_t head_id)
{
    ASSERT(head_id < FC_COMMAND_POOL_DEPTH);

    const fc_cmd_s *head = &controller->command_pool[head_id];
    ASSERT(head->cmd_id == head_id);
    BYTE need = 1U;

    if (head->config.chain == FC_CHAIN_END)
    {
        return 1U;
    }

    while (need < FC_SCHEDULER_CHAIN_MAX)
    {
        fc_cmd_id_t     next_id = fc_submission_queue_peek(queue, (need - 1U));
        const fc_cmd_s *next;

        if (next_id == FC_CMD_ID_INVALID)
        {
            break; /* 队列尾 */
        }
        next = fc_command_id_to_command(next_id);

        // 链成员必须同一 target（FTL 保证）。debug 下停机；release 下**不要把
        // 不同 LUN 的成员算进本链**——否则 fill 会按链头 LUN 选队列，把错目标
        // 的命令塞进同一条 per-LUN FIFO。截断在链头/已确认前缀处，剩余命令
        // 留在队列，由下一轮独立处理。
        const BOOL same_target = (next->ch == head->ch) && (next->ce == head->ce) && (next->lun == head->lun);

        ASSERT_MSG(same_target, "chain target mismatch: head ch/ce/lun=%hhu/%hhu/%hhu next=%hhu/%hhu/%hhu", head->ch, head->ce, head->lun, next->ch, next->ce, next->lun);
        if (same_target == false)
        {
            break;
        }
        need++;
        if (next->config.chain == FC_CHAIN_END)
        {
            break; /* chain=0：链结束 */
        }
    }
    return need;
}

/**
 * @brief 单队列填充轮次：把能放下的命令（含链）提交进 NAND per-LUN FIFO。
 *
 * @param controller FC 控制器
 * @param queue      待扫描队列（HSQ 或 SQ）
 * @param owner      本队列的链归属标签（SQ/HSQ）；链锁准入用：某 LUN 上有未结束的链时，只放行链所在队列
 * @param suspended  本轮已挂起 LUN 表（HSQ/SQ 共享，见 fc_poll_fill）
 * @param dispatched 本轮是否提交过命令（出参）
 *
 * @return FC_STATUS_OK；FC_STATUS_IO_ERROR（提交失败，剩余命令留在队列）
 *
 * @note **按 LUN 保序**：need = 链长；fc_nand_dispatch_lun_free(lun) >= need 才取，
 * 否则该 LUN 本轮挂起（suspended），后续同 LUN 命令一律跳过。
 * - **链锁准入**：该 LUN 上有未结束的链（chain_owner != NONE）且链不归本队列 →
 * 本命令回推跳过。**只回推、不置 suspended**——置了会把链自己的续批一起锁死。
 * - **链锁更新**：一批下发后看批尾的 chain 位——MORE 继续占着该 LUN（转本队列），
 * END 解锁；下发失败（整链注 NAND_FAIL）也清锁，不留残锁。
 * - **一次扫描**：按进入时的队列快照数量扫描，扫完即止。
 * - **顺序保持**：取不走的命令 pop 后回推队尾——扫完整轮等价于一次旋转，
 * 未取出命令的相对顺序不变（同 LUN 的命令因 LUN 挂起必然一起回推，顺序不破）。
 * 链成员连续且逐个回推，链头永远排在成员之前，链序不破。
 * - 提交失败即中止本轮，剩余命令仍在队列里（未被取出），下轮重试。
 */
static fc_status_e fc_poll_fill_queue(fc_ctl_s *controller, fc_sub_queue_s *queue, fc_chain_owner_e owner, BOOL suspended[CH_NUM][CE_NUM][LUN_NUM], BOOL *dispatched)
{
    WORD total;
    WORD k;

    total = queue->count;

    for (k = 0U; k < total; k++)
    {
        fc_cmd_id_t      id;
        fc_cmd_s        *cmd;
        fc_sched_chain_s chain;
        fc_status_e      dispatch_status;
        BYTE             ch;
        BYTE             ce;
        BYTE             lun;
        BYTE             need;
        BYTE             j;

        (void)fc_submission_queue_pop(queue, &id);
        fc_fill_pops_inc();
        cmd = fc_command_id_to_command(id);

        ch  = cmd->ch;
        ce  = cmd->ce;
        lun = cmd->lun;
        // 路由字段越界是编程错误：值由 FTL 从 PBA 经 pba_to_ch/ce/lun 灌入，
        // PBA 已过 pba_is_valid() 校验。此处必须断言而非防御——suspended[] 按
        // [ch][ce][lun] 直接索引，放行即栈越界写；且越界命令会因
        // fc_nand_dispatch_lun_free()==0 被永久回推（静默活锁），比当场停机更难定位。
        ASSERT((ch < CH_NUM) && (ce < CE_NUM) && (lun < LUN_NUM));
        if (suspended[ch][ce][lun] == true)
        {
            (void)fc_submission_queue_push(queue, id);
            continue;
        }
        // 链锁准入：该 LUN 的填充权归另一条队列的未结束链 → 本命令回推跳过。
        // 为什么「只让拥有者队列喂」就够：链成员在队列里连续（见本函数契约的
        // 「顺序保持」），续批恒是该 LUN 在队列里最老的那几条，按序扫描必然先取到它们；
        // 后入队的同 LUN 命令都排在链后面，只会在链结束（chain_owner 清）之后才被取。
        // ⚠ **只回推、不置 suspended**——置了会把链自己的续批也一并锁死（该 LUN 从此
        // 既不接受另一队列、也不接受本链，活锁）。
        // chain_owner 为 NONE（无未结束的链）时恒假；FTL cache run 打开后只放行链所在队列。
        const fc_chain_owner_e chain_owner = controller->scheduler.chain_owner[ch][ce][lun];

        if ((chain_owner != FC_CHAIN_OWNER_NONE) && (chain_owner != owner))
        {
            (void)fc_submission_queue_push(queue, id);
            continue;
        }

        need = fc_poll_chain_length(controller, queue, id);
        if (fc_nand_dispatch_lun_free(ch, ce, lun) < need)
        {
            // 剩余槽放不下整条链：本轮挂起该 LUN，链头回推（成员后续轮次一并回推）
            suspended[ch][ce][lun] = true;
            (void)fc_submission_queue_push(queue, id);
            continue;
        }

        chain.count   = need;
        chain.ids[0U] = id;
        for (j = 1U; j < need; j++)
        {
            (void)fc_submission_queue_pop(queue, &chain.ids[j]);
            fc_fill_pops_inc();
        }
        k = (k + need - 1U); /* 跳过已取出的链成员 */

        // 单命令走 single，链走 chain——按 need 分流（不写成条件里的三目，
        // 两个分支都是带副作用的提交，塞进 if 里读不出"做了什么事"）
        if (need == 1U)
        {
            dispatch_status = fc_poll_dispatch_single(controller, &chain);
        }
        else
        {
            dispatch_status = fc_poll_dispatch_chain(controller, &chain);
        }

        if (dispatch_status != FC_STATUS_OK)
        {
            // 下发失败 = fc_poll_dispatch_chain 已把整链注 NAND_FAIL（构建失败或 submit
            // 失败都如此），这条链已经死了、不会再续批 ⇒ 必须清锁。不清的话该 LUN 会
            // 一直只认本队列，而那条链又不存在了，等于把 LUN 永久锁死。
            controller->scheduler.chain_owner[ch][ce][lun] = FC_CHAIN_OWNER_NONE;
            return FC_STATUS_IO_ERROR;
        }
        // 链锁更新：看**批尾**（chain.ids[need-1]，本批最后一条）的 chain 位——
        // MORE ⇒ 这条链还没完，该 LUN 的填充权继续归本队列（下一批仍从这条队列取）；
        // END  ⇒ 链在此收尾，解锁，另一条队列可以喂这个 LUN 了。
        // 单命令（need == 1）批尾恒 END ⇒ 恒写 NONE，即生产路径只是多一次无副作用的写。
        const fc_cmd_id_t tail_id = chain.ids[need - 1U];
        const fc_cmd_s   *tail    = &controller->command_pool[tail_id];

        controller->scheduler.chain_owner[ch][ce][lun] = (tail->config.chain == FC_CHAIN_MORE) ? owner : FC_CHAIN_OWNER_NONE;
        *dispatched                                    = true;
    }

    return FC_STATUS_OK;
}

/**
 * @brief 一次填充轮次——扫描 HSQ/SQ，把可下发的命令（含链）提交进 NAND per-LUN FIFO。
 *
 * @param controller FC 控制器。
 *
 * @return FC_STATUS_OK           本轮至少提交了一条。
 *         FC_STATUS_IO_ERROR    提交失败，剩余命令留在队列。
 *         FC_STATUS_QUEUE_EMPTY 无提交且 SQ/HSQ 均为空。
 *         FC_STATUS_NOT_READY   无提交，队列非空但当前无 LUN 可下发（背压）。
 *
 * @note **HSQ 优先**：先扫 HSQ，再扫 SQ；两个队列走同一套扫描逻辑（含链）。
 * - **共享 suspended**：HSQ 因 room 不足挂起某 LUN 后，SQ 本轮也跳过该 LUN——
 * 否则同 LUN 的 SQ 命令会插到 HSQ 命令之前下发，优先级倒挂。
 * - **链锁独占（跨轮次持久，与 suspended 正交）**：链一旦开始，该 LUN 的填充权
 * 归链所在队列独占，直到链的 END 下发——另一条队列的命令一律回推。
 * 与上一条的区别：suspended 管"本趟 room 不足"、每趟清零；chain_owner 管"链没完"、
 * 跨趟保留。两者互不影响（链锁准入只回推，不置 suspended）。
 * - 扫描细节见 fc_poll_fill_queue()。
 */
static fc_status_e fc_poll_fill(fc_ctl_s *controller)
{
    fc_sched_s *scheduler = &controller->scheduler;
    BOOL        suspended[CH_NUM][CE_NUM][LUN_NUM];
    BOOL        dispatched = false;

    memset(suspended, 0, sizeof(suspended));
    fc_fill_calls_inc();

    /* ── HSQ 优先：先扫 HSQ，再扫 SQ；同一套扫描逻辑，共享同一张 suspended 表 ── */
    if (fc_poll_fill_queue(controller, scheduler->hsq, FC_CHAIN_OWNER_HSQ, suspended, &dispatched) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }
    if (fc_poll_fill_queue(controller, scheduler->sq, FC_CHAIN_OWNER_SQ, suspended, &dispatched) != FC_STATUS_OK)
    {
        return FC_STATUS_IO_ERROR;
    }

    if (dispatched == true)
    {
        return FC_STATUS_OK;
    }
    return ((scheduler->sq->count == 0U) && (scheduler->hsq->count == 0U)) ? FC_STATUS_QUEUE_EMPTY : FC_STATUS_NOT_READY;
}

/* ── NAND 完成回调 ──
 * fc_nand_dispatch_poll 每产出一条完成就调一次 fc_nand_completion_sink；每条当场消费
 * （写 desc/ECC、跑数据路径、组 CQ 项、清 stripe parity），不跨条缓存。
 * ctx 直接传 fc_ctl_s *：fatal 是控制器级 sticky 状态，不再额外包一层 sink 结构。 */

/**
 * @brief NAND 完成回调：把一条 NAND 完成当场转成 FC 完成项（含 stripe parity 清理）。
 *
 * @param nand_completion NAND 层完成项
 * @param ctx FC 控制器（fc_ctl_s *）
 *
 * @note 每条完成当场消费（写 desc/ECC、跑数据路径、组 CQ 项、清 stripe parity），
 * 不跨条缓存；fc_complete_from_nand 返回非 OK 时置 controller->fatal。
 */
static void fc_nand_completion_sink(fc_nand_dispatch_comp_s *nand_completion, void *ctx)
{
    fc_ctl_s *controller = (fc_ctl_s *)ctx;

    if (fc_complete_from_nand(controller, nand_completion) != FC_STATUS_OK)
    {
        controller->fatal = true;
    }

    /* ── Stripe parity 清理 ──
     * 如果完成的命令是 stripe 数据命令，释放内部 parity 命令资源。 */
#if CONFIG_FC_RAID
    {
        fc_cmd_id_t completed_id = nand_completion->cmd_id;
        // cmd_id 是 FC 投递给 NAND 的池内 ID，fc_complete_from_nand 已断言过范围
        ASSERT(completed_id < FC_COMMAND_POOL_DEPTH);
        if (controller->stripe_parity_for_data[completed_id] != FC_CMD_ID_INVALID)
        {
            fc_cmd_id_t parity_id                            = controller->stripe_parity_for_data[completed_id];
            controller->stripe_parity_for_data[completed_id] = FC_CMD_ID_INVALID;
            fc_stripe_release_parity(controller, parity_id);
        }
    }
#endif /* CONFIG_FC_RAID */
}

/**
 * @brief FC 服务主入口：先取 NAND 完成，无完成则填充下发
 *
 * @return FC_STATUS_OK          本轮有完成被消费，或有命令被下发。
 *         FC_STATUS_QUEUE_EMPTY 无完成且 SQ/HSQ 均为空。
 *         FC_STATUS_NOT_READY   无完成，队列非空但当前无 LUN 可下发（背压）。
 *         FC_STATUS_IO_ERROR    CQ 满、时间源不可用或命令下发失败。
 */
fc_status_e fc_service(void)
{
    fc_ctl_s *controller = &g_fc_controller;

    // sticky fatal：CQ 满/时间源缺失后不再继续提交，直到 fc_init 清零。
    if (controller->fatal == true)
    {
        return FC_STATUS_IO_ERROR;
    }

    const DWORD delivered = fc_nand_dispatch_poll(fc_nand_completion_sink, controller);

    if (fc_nand_dispatch_fatal() == true)
    {
        controller->fatal = true;
    }
    if (controller->fatal == true)
    {
        return FC_STATUS_IO_ERROR;
    }

    const fc_status_e result = fc_poll_fill(controller);

    if (controller->fatal == true)
    {
        return FC_STATUS_IO_ERROR;
    }

    if (delivered == 0U)
    {
        return result;
    }

    // 完成和提交放同一轮：否则持续有完成时，提交会被一直让位。
    // 填充失败（IO_ERROR）上升为本轮错误；QUEUE_EMPTY/NOT_READY 不改变
    // “本轮已有完成”的成功语义。
    return (result == FC_STATUS_IO_ERROR) ? FC_STATUS_IO_ERROR : FC_STATUS_OK;
}

/* ══════════════════════════════════════════════════════════
 * 7. CQ 出口
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 从 CQ 取一条完成项
 *
 * @param completion 输出：完成项
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_EMPTY
 */
fc_status_e fc_poll_completion(fc_cmd_status_s *completion)
{
    // completion 非空由 fc_completion_queue_pop 的入口断言收口，这里不重复。
    return fc_completion_queue_pop(&g_fc_controller.cq, completion);
}

/**
 * @brief 从 CQ 批量取完成项
 *
 * @param completions 输出数组
 * @param max_count   最多取几条；须 > 0
 * @param count       输出：实际条数
 *
 * @return FC_STATUS_OK             取到 ≥1 条。
 *         FC_STATUS_QUEUE_EMPTY    一条没取到（CQ 为空）。
 *         FC_STATUS_INVALID_ARGUMENT max_count == 0（release 拒绝）。
 */
fc_status_e fc_poll_completions(fc_cmd_status_s *completions, WORD max_count, WORD *count)
{
    WORD      n          = 0U;
    fc_ctl_s *controller = &g_fc_controller;

    ASSERT(completions != NULL);
    ASSERT(count != NULL);
    ASSERT_REJECT(max_count > 0U, FC_STATUS_INVALID_ARGUMENT, "poll completions: max_count=0");

    while ((n < max_count) && (fc_completion_queue_pop(&controller->cq, &completions[n]) == FC_STATUS_OK))
    {
        n++;
    }
    *count = n;

    return (n != 0U) ? FC_STATUS_OK : FC_STATUS_QUEUE_EMPTY;
}

/* ══════════════════════════════════════════════════════════
 * 断言终端路径的 FC 现场补充
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 断言终端路径的 FC 现场补充：把各资源池占用与队列长度追加到日志。
 *
 * @note 覆盖 assertion.c 中的同名弱钩子（强符号），由 assertion_terminate 在
 * flush 之前调用。故障路径只做标量快照：**不得**调用会断言/扫描池的函数
 * （fc_get_resource_stats / fc_debug_state_get）——二次断言会覆盖本次现场并递归；
 * fc_pool_init 之前池指针还是 NULL，扫描也会崩。
 * ⚠ 本函数在 release 也必须存在：ASSERT_REJECT 在 release 仍会走 assertion_failf →
 * assertion_terminate → 本钩子；不要把它并入 #ifndef NDEBUG 的调试块。
 */
void assertion_platform_context(void)
{
    LOGE(LOG_TAG_RES "cmd=%hu/%u desc=%hu/%u buf=%hu/%u sq=%hu hsq=%hu cq=%hu\r\n",
         g_fc_controller.command_used,
         FC_COMMAND_POOL_DEPTH,
         g_fc_controller.descriptor_used,
         FC_DESCRIPTOR_POOL_DEPTH,
         g_fc_controller.buffer_used,
         FC_BUFFER_POOL_DEPTH,
         g_fc_controller.sq.count,
         g_fc_controller.hsq.count,
         g_fc_controller.cq.count);
}

/* ══════════════════════════════════════════════════════════
 * 8. 提交与诊断
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 提交一条命令
 *
 * 配置错误一律 ASSERT；唯一合法失败是队列满。
 *
 * @param cmd_id 命令 ID（ALLOCATED）
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL
 */
fc_status_e fc_submit(fc_cmd_id_t cmd_id)
{
    /* ── 校验段：提交是唯一入队边界 —— debug ASSERT + release 拒绝（判据 9）。 ── */
    ASSERT_REJECT(cmd_id < FC_COMMAND_POOL_DEPTH,
                  FC_STATUS_INVALID_ARGUMENT,
                  "submit: cmd=%hu out of range",
                  cmd_id);
    ASSERT_REJECT(g_fc_controller.command_state[cmd_id] == FC_COMMAND_STATE_ALLOCATED,
                  FC_STATUS_INVALID_ARGUMENT,
                  "submit: cmd=%hu not ALLOCATED",
                  cmd_id);

    fc_cmd_s *cmd = &g_fc_controller.command_pool[cmd_id];

    fc_command_validate(cmd);
    ASSERT_REJECT(fc_pool_chain_validate(&g_fc_controller, cmd_id) != 0U,
                  FC_STATUS_INVALID_ARGUMENT,
                  "submit: chain invalid cmd=%hu",
                  cmd_id);

    return fc_sched_submit(&g_fc_controller, cmd_id);
}

/**
 * @brief 批量提交（全进或全不进）
 *
 * cache program 批次依赖此语义；链契约要求全批同优先级。
 *
 * @param cmds 命令数组
 * @param n    条数
 * @return FC_STATUS_OK；FC_STATUS_QUEUE_FULL；FC_STATUS_INVALID_ARGUMENT 链契约违反
 */
fc_status_e fc_submit_batch(const fc_cmd_id_t cmd_ids[], DWORD n)
{
    ASSERT_REJECT((cmd_ids != NULL) && (n > 0U),
                  FC_STATUS_INVALID_ARGUMENT,
                  "batch: cmd_ids/n invalid");

    WORD  sq_need        = 0U;
    WORD  hsq_need       = 0U;
    BOOL  has_chain      = false;
    BOOL  mixed_queue    = false;
    DWORD first_priority = 0U;
    BYTE  first_ch       = 0U;
    BYTE  first_ce       = 0U;
    BYTE  first_lun      = 0U;

    for (DWORD i = 0U; i < n; i++)
    {
        const fc_cmd_id_t cmd_id = cmd_ids[i];

        // 批内重复：同一 ID 出现两次会双双通过校验；提交阶段会把同一 ID
        // 入队两次（第二次状态 setter 拒绝但 push 已发生）。
        for (DWORD j = 0U; j < i; j++)
        {
            ASSERT_REJECT(cmd_ids[j] != cmd_id,
                          FC_STATUS_INVALID_ARGUMENT,
                          "batch: duplicate command at index %lu",
                          i);
        }

        ASSERT_REJECT(cmd_id < FC_COMMAND_POOL_DEPTH,
                      FC_STATUS_INVALID_ARGUMENT,
                      "batch: cmd=%hu out of range",
                      cmd_id);
        ASSERT_REJECT(g_fc_controller.command_state[cmd_id] == FC_COMMAND_STATE_ALLOCATED,
                      FC_STATUS_INVALID_ARGUMENT,
                      "batch: cmd=%hu not ALLOCATED",
                      cmd_id);

        fc_cmd_s *cmd = &g_fc_controller.command_pool[cmd_id];

        fc_command_validate(cmd);
        ASSERT_REJECT(fc_pool_chain_validate(&g_fc_controller, cmd_id) != 0U,
                      FC_STATUS_INVALID_ARGUMENT,
                      "batch: chain invalid cmd=%hu",
                      cmd_id);

        // 一条链只能落在一个 target：链成员跨 ch/ce/lun 会被 dispatcher 按链头
        // 路由，其余成员静默打到别的实例。release 也必须拒绝（H14）。
        if (i == 0U)
        {
            first_ch       = (BYTE)cmd->ch;
            first_ce       = (BYTE)cmd->ce;
            first_lun      = (BYTE)cmd->lun;
            first_priority = cmd->high_priority;
        }
        else
        {
            ASSERT_REJECT((cmd->ch == first_ch) && (cmd->ce == first_ce) && (cmd->lun == first_lun),
                          FC_STATUS_INVALID_ARGUMENT,
                          "batch: mixed target (ch/ce/lun) at index %lu",
                          i);

            if (cmd->high_priority != first_priority)
            {
                mixed_queue = true;
            }
        }

        has_chain = (has_chain == true) || (cmd->config.chain == FC_CHAIN_MORE);

        if (cmd->high_priority != 0U)
        {
            hsq_need++;
        }
        else
        {
            sq_need++;
        }
    }

    // 链契约：一批里的 chain 必须全批同优先级；**末条允许 MORE**（流式窗口）。
    // · 混优先级 ⇒ 链被拆成 SQ/HSQ 两段，fc_poll_chain_length 只沿本队列 peek，
    //   跳过的成员被当成不存在 ⇒ 序列静默错乱（cache read 输出错页 / 写错序列）；
    // · 末条 MORE 是合法中间窗口：FTL 的序列状态保证在 run_end 发 END；FC 的
    //   chain_owner 跨窗口保留，下一窗口继续。
    // 负例：cmds[0].chain=MORE 且 cmds[1].high_priority=1 ⇒ release 拒绝。
    ASSERT_REJECT(!((has_chain == true) && (mixed_queue == true)), FC_STATUS_INVALID_ARGUMENT, "mixed-priority chain batch");

    if (((WORD)(g_fc_controller.sq.count + sq_need) > FC_SQ_DEPTH) || ((WORD)(g_fc_controller.hsq.count + hsq_need) > FC_HSQ_DEPTH))
    {
        return FC_STATUS_QUEUE_FULL;
    }

    for (DWORD i = 0U; i < n; i++)
    {
        const fc_status_e st = fc_sched_submit(&g_fc_controller, cmd_ids[i]);

        // 预检通过后入队不可能失败；失败即队列不变量被破坏：
        // debug 停机；release 记 sticky fatal，不再继续半批。
        ASSERT(st == FC_STATUS_OK);
        if (st != FC_STATUS_OK)
        {
            g_fc_controller.fatal = true;
            return FC_STATUS_IO_ERROR;
        }
    }

    return FC_STATUS_OK;
}

/**
 * @brief 汇总队列/三池/调度器调试状态
 *
 * @param out 输出
 */
void fc_debug_state_get(fc_debug_state_s *out)
{
    ASSERT(out != NULL);

    memset(out, 0, sizeof(*out));
    out->sq_used     = g_fc_controller.sq.count;
    out->hsq_used    = g_fc_controller.hsq.count;
    /* 诊断采样：只报告 (ch,ce,lun)=(0,0,0) 的 LUN，不是全维遍历。 */
    out->chain_owner = (BYTE)g_fc_controller.scheduler.chain_owner[0][0][0];
    out->lun_used    = fc_nand_dispatch_lun_used(0U, 0U, 0U);
    out->lun_phase   = fc_nand_dispatch_lun_phase(0U, 0U, 0U);
    fc_fill_counters_read(&out->fill_calls, &out->fill_pops);
    out->sched_sq_count = (g_fc_controller.scheduler.sq != NULL) ? g_fc_controller.scheduler.sq->count : 0U;
    out->sched_sq_bound = ((g_fc_controller.scheduler.sq == &g_fc_controller.sq) && (g_fc_controller.scheduler.hsq == &g_fc_controller.hsq)) ? 1U : 0U;

    const fc_cmd_id_t sq_id = fc_submission_queue_peek(&g_fc_controller.sq, 0U);

    if (sq_id != FC_CMD_ID_INVALID)
    {
        const fc_cmd_s *cmd = fc_command_id_to_command(sq_id);

        out->sq_head_id    = sq_id;
        out->sq_head_state = (BYTE)g_fc_controller.command_state[sq_id];
        out->sq_head_chain = (WORD)cmd->config.chain;
        out->sq_head_op    = (BYTE)cmd->config.operation;
    }

    const fc_cmd_id_t hsq_id = fc_submission_queue_peek(&g_fc_controller.hsq, 0U);

    if (hsq_id != FC_CMD_ID_INVALID)
    {
        const fc_cmd_s *cmd = fc_command_id_to_command(hsq_id);

        out->hsq_head_id    = hsq_id;
        out->hsq_head_chain = (WORD)cmd->config.chain;
        out->hsq_head_op    = (BYTE)cmd->config.operation;
    }
}

/**
 * @brief 汇总队列与三池占用
 *
 * @param stats 输出
 *
 * @note debug 档会扫描全池并 ASSERT 计数一致；**不得用于断言/故障路径**——
 *       计数不一致正是断言常见原因，二次断言会让终端递归（见 assertion_platform_context）。
 *       函数本体 debug/release 都要编译（FTL/CLI 调用）；只有下面的全池校验扫描是
 *       #ifndef NDEBUG。
 */
void fc_get_resource_stats(fc_resource_stats_s *stats)
{
    // stats 为出参前置条件（调用者恒传有效指针）
    ASSERT(stats != NULL);

    stats->sq_used         = g_fc_controller.sq.count;
    stats->hsq_used        = g_fc_controller.hsq.count;
    stats->cq_used         = g_fc_controller.cq.count;
    stats->command_used    = g_fc_controller.command_used;
    stats->descriptor_used = g_fc_controller.descriptor_used;
    stats->buffer_used     = g_fc_controller.buffer_used;
    stats->command_high    = g_fc_controller.command_high;
    stats->descriptor_high = g_fc_controller.descriptor_high;
    stats->buffer_high     = g_fc_controller.buffer_high;

#ifndef NDEBUG
    /* debug 档做一次全池扫描，证明 O(1) 计数器与真值一致（C7 负例：
     * 任一闸门漏增减，这里立刻炸）。release 档只读计数器。 */
    WORD command_scan    = 0U;
    WORD descriptor_scan = 0U;
    WORD buffer_scan     = 0U;

    for (WORD index = 0U; index < FC_COMMAND_POOL_DEPTH; index++)
    {
        if (g_fc_controller.command_state[index] != FC_COMMAND_STATE_FREE)
        {
            command_scan++;
        }
    }
    for (WORD index = 0U; index < FC_DESCRIPTOR_POOL_DEPTH; index++)
    {
        if (g_fc_controller.descriptor_owner[index] != FC_CMD_ID_INVALID)
        {
            descriptor_scan++;
        }
    }
    for (WORD index = 0U; index < FC_BUFFER_POOL_DEPTH; index++)
    {
        if (g_fc_controller.buffer_state[index] != FC_BUFFER_STATE_FREE)
        {
            buffer_scan++;
        }
    }
    ASSERT(command_scan == stats->command_used);
    ASSERT(descriptor_scan == stats->descriptor_used);
    ASSERT(buffer_scan == stats->buffer_used);
#endif
}

/**
 * @brief 打印 FC 容量与拓扑（开机一次）
 *
 * 与断言路径的 [RES] 现场行互补：此处打容量，那里打占用。
 */
void fc_boot_report(void)
{
    const char *data_path = "PIO"; /* FMC NAND 无 DMA 请求线，数据面固定 PIO */

    LOGI("[FC  ] cmd=%u desc=%u buf=%u sq=%u hsq=%u cq=%u\r\n", FC_COMMAND_POOL_DEPTH, FC_DESCRIPTOR_POOL_DEPTH, FC_BUFFER_POOL_DEPTH, FC_SQ_DEPTH, FC_HSQ_DEPTH, FC_CQ_DEPTH);
    LOGI("[FC  ] topology ch=%u ce=%u lun=%d plane=%d raid=%s\r\n", CH_NUM, CE_NUM, LUN_NUM, PLANE_NUM, (CONFIG_FC_RAID != 0U) ? "on" : "off");
    LOGI("[FC  ] data path=%s ecc=%s random=%s\r\n", data_path, fc_ecc_algo_name(), fc_random_algo_name());
}
