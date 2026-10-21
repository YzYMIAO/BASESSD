/*
 * mt29f___g08a.c — MT29FxxxG08A 族 NAND 驱动
 *
 * 职责：
 *   - 芯片初始化（FMC 硬件 + DMA + RESET + READ_ID 校验）
 *   - FMC 时序换算（ONFI ns → PCC 寄存器周期数）
 *   - ONFI 操作表（g_nand_operation_map，定义于 onfi_1_0.c）的执行入口：
 *       nand_fc_execute/start/step → nand_engine → UOP → FMC 后端
 *
 * 芯片：当前封装为 Micron MT29F4G08ABADAWP，512MB SLC，
 *       tRC=50ns（Mode 0）/ 20ns（Mode 5）。
 * 器件事实见 nand/devices/mt29f___g08a.h；通用布局见 nand/nand_config.h。
 *
 * 时序来源：本文件两张静态表（slow/fast，MT29 数据手册参数）
 *   → nand_fmc_timing_t（nand_fmc.h，29 字段，顺序耦合）
 *   → UOP handler 层（nand_fmc.c）与 PCC 换算（nand_fmc_timing_to_pcc）。
 */

#include "assertion.h"
#include "delay.h"
#include "dma.h"
#include "log.h"
#include "nand_config.h"
#include "nand_debug.h"
#include "nand_dispatch.h"
#include "nand_fc_interface.h"
#include "nand_fmc.h"

#include <stddef.h>
#include <string.h>

/* 全局 HAL NAND 句柄：生命周期从 nand_init 起，供 FMC 初始化/时序更新复用 */
static NAND_HandleTypeDef g_nand_handle;

/* 当前实际生效的 ONFI timing mode（0 = 仍为低速）。[STAT] 诊断用。 */
static BYTE s_timing_mode;

/* --------------------------------------------------------------------
 * g_nand_timing_high — MT29F4G08 3.3V 时序（数据手册 Table 29/31）
 *   29 个值按 nand_fmc_timing_t 字段顺序初始化（顺序耦合，错一位全错）。
 *   本表固定使用 Table 29/31 的 3.3V AC 参数；CONFIG_NAND_TIMING_MODE 只改变
 *   SET_FEATURES 的模式值（4 或 5），用于在**相同 FMC 时序**下对比两种模式。
 *   注意：Mode 4 的独立 AC 表在数据手册 Table 32（1.8V），本板 3.3V 实验不采用它。
 *   按组划分：
 *     地址锁存组  : tADL / tALH / tALS / tCS / tCH / tCLH / tCLS
 *     数据读写组  : tDH / tDS / tWC / tWH / tWP / tAR / tIR / tRC / tREA / tREH /
 *                   tRP / tRLOH / tRHOH / tRHW / tRHZ / tRR / tWHR / tWB
 *     就绪/复电组 : tCEA / tCHZ / tCLR / tCOH
 *   （Mode 0 典型值；本表被 nand_init 低速分支的初始 config 引用）
 * -------------------------------------------------------------------- */
static const nand_fmc_timing_t g_nand_timing_high = {
    70U,  /* tADL   地址→数据加载延时 */
    5U,   /* tALH   ALE 保持 */
    10U,  /* tALS   ALE 建立 */
    5U,   /* tCH    CE 保持 */
    5U,   /* tCLH   CLE 保持 */
    10U,  /* tCLS   CLE 建立 */
    15U,  /* tCS    CE 建立 */
    5U,   /* tDH    数据保持 */
    7U,   /* tDS    数据建立 */
    20U,  /* tWC    写周期 */
    7U,   /* tWH    WE 高电平宽度 */
    10U,  /* tWP    WE 低电平宽度 */
    10U,  /* tAR    ALE→RE 延时 */
    25U,  /* tCEA   CE→输出有效（max） */
    50U,  /* tCHZ   CE→输出高阻（max） */
    10U,  /* tCLR   CLE→RE 延时 */
    15U,  /* tCOH   CE 高→输出保持 */
    0U,   /* tIR    命令→数据输出间隔（Mode0 为 0） */
    20U,  /* tRC    读周期 */
    16U,  /* tREA   RE→输出有效（max） */
    7U,   /* tREH   RE 高电平宽度 */
    15U,  /* tRHOH  RE 高→输出保持 */
    100U, /* tRHW   RE→WE 延时 */
    100U, /* tRHZ   RE→输出高阻（max） */
    5U,   /* tRLOH  RE 低→输出保持 */
    10U,  /* tRP    RE 低电平宽度 */
    20U,  /* tRR    读命令→读命令间隔 */
    100U, /* tWB    WE 高→busy（max） */
    60U,  /* tWHR   状态→RE 高延时 */
};

/*
 * 根据 ONFI 时序参数 nand_fmc_timing_t 和 FMC 时钟频率，
 * 计算 FMC PCC 硬件寄存器值。
 *
 * FMC kernel clock 由调用者传入（当前 200 MHz → tHCLK = 5 ns）。
 * 所有 ONFI 时序量纲为 ns，向上取整到整周期数（保守偏慢）。
 *
 * 当前实现基于 MT29F4G08ABADAWP 异步时序协商规则：
 *   SetupTime     = max(tCLS, tALS, tCS)           / tHCLK
 *   WaitSetupTime = max(tRP, tWP)                  / tHCLK
 *   HoldSetupTime = max(tCH, tALH, tCLH, tDH)      / tHCLK
 *   HiZSetupTime  = tRHZ / tHCLK
 */
#if (CONFIG_NAND_TIMING_MODE != 0U)
/* FMC 周期由调用者传入实际 fmc_hz 计算（1e12 / fmc_hz ps，向下取整）。
 * 当前 sys_port.c 配 PLL2R=200MHz（D1 域 FMC 上限 200MHz）⇒ tHCLK = 5000 ps。 */
/* 首次 Mode 5 上板留 1 个 FMC 周期的余量；稳定后可收紧到 0。 */
#    define NAND_PCC_MARGIN_CYCLES (1U)
/* 板级时序余量（单位：FMC 周期）：Setup/Wait 在通用 margin 之上再各 +1。
 * 2× 曾用于 bring-up 确认“失败来自余量”；当前值 FCUT 55/55 通过，
 * 200MHz 下各相相对数据手册最小值 ≥ +5ns。再减 1 周期即 0ns 余量，不再收紧。 */
#    define NAND_PCC_SETUP_EXTRA          (1U)
#    define NAND_PCC_WAIT_EXTRA           (1U)
#    define NS_TO_PCC_CYCLES(ns, tclk_ps) ((((ns)*1000U) + (tclk_ps)-1U) / (tclk_ps))

/*
 * ST 精确式（tHCLK = 1e12 / fmc_hz ps，当前 5000 ps）：
 *   (SET+1)  * tHCLK >= max(tCS,tCLS,tALS,tCLR,tAR) - tWP
 *   (WAIT+1) * tHCLK >= max(tWP,tRP)
 *   HOLD     * tHCLK >= max(tCH,tCLH,tALH)
 *   HiZ      * tHCLK >= max(tCS,tCLS,tALS) + (tWP - tDS)
 * 旧实现硬编码 5ns/4545ps，FMC 时钟一变就失真；现按实际 fmc_hz 算。
 * 旧实现（Mode 4 表）实测 HOLD/HiZ 各短 1 周期，命令/地址采样失败（RISK-NAND-03）。
 */
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_t    *onfi,
                                   FMC_NAND_PCC_TimingTypeDef *pcc,
                                   DWORD                       fmc_hz)
{
    ASSERT(fmc_hz != 0U);

    const DWORD tclk_ps = (DWORD)(1000000000000ULL / (QWORD)fmc_hz);

    ASSERT(tclk_ps != 0U);
    /* max(tCS, tCLS, tALS, tCLR, tAR) → SetupTime：命令/地址建立窗口。 */
    {
        DWORD max_ns = onfi->t_cs_ns;

        if (onfi->t_cls_ns > max_ns)
        {
            max_ns = onfi->t_cls_ns;
        }
        if (onfi->t_als_ns > max_ns)
        {
            max_ns = onfi->t_als_ns;
        }
        if (onfi->t_clr_ns > max_ns)
        {
            max_ns = onfi->t_clr_ns;
        }
        if (onfi->t_ar_ns > max_ns)
        {
            max_ns = onfi->t_ar_ns;
        }

        const DWORD setup_ns = (max_ns > onfi->t_wp_ns) ? (max_ns - onfi->t_wp_ns) : 0U;
        DWORD       cycles   = NS_TO_PCC_CYCLES(setup_ns, tclk_ps);

        if (cycles > 0U)
        {
            cycles--; /* 精确式的 (SET+1) 项 */
        }
        DWORD setup = cycles + NAND_PCC_MARGIN_CYCLES;

        /* CE# setup 下限：FMC MEMSET 的 setup 相必须至少覆盖 tCS。
         * ST 精确式减去了 tWP，单看该式会把 Setup 压到 1 个周期（200MHz 下 5ns），
         * 而 3.3V Mode 5 的 tCS=15ns；地址/擦除命令在这种 Setup 下会失败（实测
         * op=18/16 status=0xFF，而纯命令的 status_query 能过）。 */
        const DWORD setup_min = NS_TO_PCC_CYCLES(onfi->t_cs_ns, tclk_ps);

        if (setup < setup_min)
        {
            setup = setup_min;
        }
        pcc->SetupTime = setup + NAND_PCC_SETUP_EXTRA;
    }

    /* max(tWP, tRP) → WaitSetupTime：RE/WE 低电平宽度。 */
    {
        DWORD max_ns = onfi->t_wp_ns;

        if (onfi->t_rp_ns > max_ns)
        {
            max_ns = onfi->t_rp_ns;
        }

        DWORD cycles = NS_TO_PCC_CYCLES(max_ns, tclk_ps);

        if (cycles > 0U)
        {
            cycles--; /* 精确式的 (WAIT+1) 项 */
        }
        pcc->WaitSetupTime = cycles + NAND_PCC_MARGIN_CYCLES + NAND_PCC_WAIT_EXTRA;
    }

    /* max(tCH, tCLH, tALH, tDH) → HoldSetupTime：控制/数据保持。 */
    {
        DWORD max_ns = onfi->t_ch_ns;

        if (onfi->t_clh_ns > max_ns)
        {
            max_ns = onfi->t_clh_ns;
        }
        if (onfi->t_alh_ns > max_ns)
        {
            max_ns = onfi->t_alh_ns;
        }
        if (onfi->t_dh_ns > max_ns)
        {
            max_ns = onfi->t_dh_ns;
        }
        pcc->HoldSetupTime = NS_TO_PCC_CYCLES(max_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;
    }

    /* HiZSetupTime：写方向总线高阻窗口（与读方向 tRHZ 无关）。 */
    {
        DWORD       max_ns = onfi->t_cs_ns;
        const DWORD wp_ds  = (onfi->t_wp_ns > onfi->t_ds_ns) ? (onfi->t_wp_ns - onfi->t_ds_ns) : 0;

        if (onfi->t_cls_ns > max_ns)
        {
            max_ns = onfi->t_cls_ns;
        }
        if (onfi->t_als_ns > max_ns)
        {
            max_ns = onfi->t_als_ns;
        }
        pcc->HiZSetupTime = NS_TO_PCC_CYCLES(max_ns + wp_ds, tclk_ps) + NAND_PCC_MARGIN_CYCLES;
    }
}
#endif /* CONFIG_NAND_TIMING_MODE */

/* --------------------------------------------------------------------
 * nand_fc_execute
 *   功能：同步执行单个 ONFI 操作（阻塞直至完成）。
 *   参数：operation —— OP_*（< OP_COUNT）；request —— 列/行/数据；
 *         completion —— 回填执行状态 + NAND 状态字节。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：经 g_nand_operation_map[operation] 取 UOP 序列，
 *         手工展开 request（row 只拷前 2 个 plane，NAND_PLANE_MAX=4 深度）
 *         后调用 nand_engine_execute 阻塞执行。
 *         契约：仅初始化阶段（armed==0）可调，见下 ASSERT。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_execute(BYTE operation, const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    const nand_operation_map_t *map;
    nand_command_request_t      command_request;
    nand_command_completion_t   command_completion;
    BYTE                        status;

    /* 入参前置条件由调用者保证（本文件内 typed 包装 / nand_init） */
    ASSERT(operation < OP_COUNT);
    ASSERT(request != NULL);
    ASSERT(completion != NULL);
    /* 同步入口契约：仅 nand_init 阶段（armed==0）可调。放行闩锁置位后
     * 必须走 nand_fc_start/step 异步路径——此处阻塞式执行会卡住整个调度器。 */
    ASSERT(nand_dispatch_is_armed() == 0U);

    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    memset(&command_request, 0, sizeof(command_request));
    command_request.operation   = operation;
    command_request.column      = request->column;
    command_request.buffer      = request->data;
    command_request.data_length = request->data_length;
    /* 行地址按 NAND_PLANE_MAX 整体搬运，不写死 plane 下标（H39）。 */
    for (BYTE plane = 0U; plane < NAND_PLANE_MAX; plane++)
    {
        command_request.row[plane] = request->row[plane];
    }
    status                       = nand_engine_execute(nand_fmc_backend_get(), map->uops, &map->timing, &command_request, &command_completion);
    completion->execution_status = status;
    completion->nand_status      = command_completion.nand_status;
    return status;
}

/* --------------------------------------------------------------------
 * nand_fc_start
 *   功能：发起异步 ONFI 事务（非阻塞，仅推进一步引擎）。
 *   参数：transaction —— 事务状态（含 backend/engine）；operation —— OP_*；
 *         request —— 命令数据。
 *   返回：NAND_RESULT_OK（start 内已完成，active 已清）/
 *         NOT_READY（有 busy 阶段，需 nand_fc_step 推进）/ 错误码。
 *   实现要点：
 *     - 先清 DMA 残留完成标志，防 DATA_IN/OUT 误判。
 *     - engine.backend 由调用者（nand_dispatch_task_load）绑定，恒非空（前置条件断言）。
 *     - nand_engine_start 末尾即 step 一次：无 busy 阶段命令直接完成。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_start(nand_fc_transaction_t *transaction, BYTE operation, const nand_fc_request_t *request)
{
    const nand_operation_map_t *map;
    BYTE                        result;

    /* 入参前置条件由调用者保证（nand_dispatch 由已校验的 task 发起） */
    ASSERT(transaction != NULL);
    ASSERT(operation < OP_COUNT);
    ASSERT(request != NULL);

    map = &g_nand_operation_map[operation];
    if (map->uops == NULL)
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    /* 清零 DMA 完成标志，防止上一命令的残留标志导致 DATA_IN/DATA_OUT 误判 */
    nand_dma_clear_flags();

    ENG_LOGD("nand_fc_start: op=%u col=%u row=0x%06X data=%p len=%u\r\n",
             (unsigned)operation,
             (unsigned)request->column,
             (unsigned)request->row[0],
             (const void *)request->data,
             (unsigned)request->data_length);

    memset(&transaction->request, 0, sizeof(transaction->request));
    transaction->request.operation   = operation;
    transaction->request.column      = request->column;
    transaction->request.buffer      = request->data;
    transaction->request.data_length = request->data_length;
    /* 行地址按 NAND_PLANE_MAX 整体搬运，不写死 plane 下标（H39）。 */
    for (BYTE plane = 0U; plane < NAND_PLANE_MAX; plane++)
    {
        transaction->request.row[plane] = request->row[plane];
    }
    memset(&transaction->completion, 0, sizeof(transaction->completion));
    /* engine.backend 由 nand_dispatch_task_load 随事务绑定（全工程唯一写点，恒非空）；
     * 未绑定属调用者违约，按断言纪律转 ASSERT 而非静默回退 */
    ASSERT(transaction->engine.backend != NULL);
    result = nand_engine_start(&transaction->engine, map->uops, &map->timing);
    if (result != NAND_RESULT_NOT_READY)
    {
        /* 命令完成（OK：无 busy 阶段，start 内即跑完）或出错，两种都不该留在 active。
         * 引擎在 END 分支已清 active，但入参校验 / 非法 UOP 码 / handler 缺失这三条
         * 错误路径不清，这里统一兜底（OK 时重复清一次无副作用）。 */
        transaction->engine.active = 0U;
        LOGE_IF(result != NAND_RESULT_OK, "[NAND] start failed, op=%u result=0x%02X status=0x%02X\r\n", operation, result, transaction->completion.nand_status);
    }
    return result;
}

/* --------------------------------------------------------------------
 * nand_fc_step
 *   功能：推进异步事务一个引擎步（配合 nand_fc_start 使用）。
 *   参数：transaction —— 发起中的事务；completion —— 完成时回填。
 *   返回：NAND_RESULT_NOT_READY（未完成，继续 poll）/ 完成结果。
 *   实现要点：engine step 直到非 NOT_READY 才算完成，完成时回填
 *         completion 并清 active。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion)
{
    BYTE result;

    /* 调用者只在事务 active 时推进（nand_dispatch.c 已先判 active） */
    ASSERT(transaction != NULL);
    ASSERT(completion != NULL);
    ASSERT(transaction->engine.active != 0U);

    result = nand_engine_step(&transaction->engine);
    if (result != NAND_RESULT_NOT_READY)
    {
        completion->execution_status = result;
        completion->nand_status      = transaction->completion.nand_status;
        ENG_LOGD("complete, op=%u result=0x%02X status=0x%02X\r\n", transaction->request.operation, result, completion->nand_status);
        transaction->engine.active = 0U;
    }
    return result;
}

/* --------------------------------------------------------------------
 * nand_fc_reset
 *   功能：NAND 复位（同步）。
 *   返回：NAND_RESULT_OK / 错误码。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_reset(nand_fc_completion_t *completion)
{
    nand_fc_request_t request;
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.data        = NULL;
    request.data_length = 0U;

    return nand_fc_execute(OP_RESET, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_fc_read_id
 *   功能：读取芯片 ID（5 字节，同步）。
 *   参数：id —— 输出缓冲（≥5 字节）；completion —— 状态。
 *   返回：NAND_RESULT_OK / 错误码。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_read_id(BYTE id[5], nand_fc_completion_t *completion)
{
    ASSERT(id != NULL);

    nand_fc_request_t request;
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.data        = id;
    request.data_length = 5U;

    return nand_fc_execute(OP_READ_ID, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_fc_read_parameter_page
 *   功能：读 ONFI 参数页（ECh，256 字节，同步）。
 *   参数：page —— 输出缓冲（≥ NAND_ONFI_PARAM_PAGE_SIZE 字节）；completion —— 状态。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：column / row 恒 0——ECh 无地址语义（uop 序列只有 COLUMN_0 = 0）。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_read_parameter_page(BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_fc_completion_t *completion)
{
    ASSERT(page != NULL);

    nand_fc_request_t request;
    memset(&request, 0, sizeof(request));
    request.column      = 0U;
    request.data        = page;
    request.data_length = NAND_ONFI_PARAM_PAGE_SIZE;

    return nand_fc_execute(OP_READ_PARAMETER_PAGE, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_fc_set_features
 *   功能：写 Feature 寄存器（SET_FEATURES，同步）。
 *   参数：feature_address —— 寄存器地址；feature_data —— 4 字节值。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：column 复用为 feature 地址，data 长度固定 4。
 * -------------------------------------------------------------------- */
nand_result_e nand_fc_set_features(BYTE feature_address, const BYTE feature_data[4], nand_fc_completion_t *completion)
{
    ASSERT(feature_data != NULL);

    nand_fc_request_t request;
    memset(&request, 0, sizeof(request));
    request.column      = feature_address;
    request.data        = (BYTE *)feature_data;
    request.data_length = 4U;

    return nand_fc_execute(OP_SET_FEATURES, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_onfi_param_print
 *   功能：把解码后的 ONFI 参数页打三行开机日志（设备信息）。
 *   参数：info —— 已解码的参数页。
 *   返回：无。
 *   实现要点：每行必须 < 192 字符（src/common/log.c 的 message[192] 上限，超长会被
 *         截断并打 "...[truncated]"）。时序字段只打原值、不打单位后缀——量纲见手册
 *         Table 11，仓库内未查证。address_cycles 同时打原值与解码后的
 *         C/R 周期数（0x23 = C2/R3），供 nand_init 的一致性检查对读。
 * -------------------------------------------------------------------- */
static void nand_onfi_param_print(const nand_onfi_param_info_t *info)
{
    /* 器件总容量（MiB）= 页主区 × 页/块 × 块/单元 × 逻辑单元。
     * 本器件 = 2^29 B；>4 GiB 器件在 32 位 unsigned long 下会溢出（本工程只跑 MT29F4G08）。 */
    unsigned long capacity_mib = info->data_bytes_per_page * info->pages_per_block * info->blocks_per_unit *
                                 info->logical_units / (1024UL * 1024UL);

    LOGI("[NAND] ONFI sig=%c%c%c%c rev=0x%04X model=%s\r\n",
         (int)info->signature[0],
         (int)info->signature[1],
         (int)info->signature[2],
         (int)info->signature[3],
         (unsigned int)info->revision,
         info->model);
    LOGI("[NAND] ONFI %luMiB page=%lu+%u partial=%lu+%u block=%lup blocks=%lu lun=%u addrcyc=0x%02X(C%u/R%u) bitcell=%u\r\n",
         capacity_mib,
         info->data_bytes_per_page,
         (unsigned int)info->spare_bytes_per_page,
         info->data_bytes_per_partial_page,
         (unsigned int)info->spare_bytes_per_partial_page,
         info->pages_per_block,
         info->blocks_per_unit,
         (unsigned int)info->logical_units,
         (unsigned int)info->address_cycles,
         (unsigned int)info->column_cycles,
         (unsigned int)info->row_cycles,
         (unsigned int)info->bits_per_cell);
    LOGI("[NAND] ONFI ecc=%ubit prog/pg=%u tprog=%u terase=%u tr=%u tccs=%u timingmodes=0x%04X\r\n",
         (unsigned int)info->ecc_bits,
         (unsigned int)info->programs_per_page,
         (unsigned int)info->tprog_max,
         (unsigned int)info->tbers_max,
         (unsigned int)info->tr_max,
         (unsigned int)info->tccs_min,
         (unsigned int)info->timing_mode_support);
}

/* --------------------------------------------------------------------
 * nand_init
 *   功能：NAND 芯片低速全初始化（FMC 硬件 + MSP + DMA +
 *         RESET + READ_ID + ONFI 参数页校验）。
 *   参数：无。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：
 *     - FMC 用 Bank3、8 位总线、ECC 关闭；初始时序为 power-on
 *       默认 Mode 0 的保守寄存器周期数（4/6/2/3）。
 *     - RESET/READ_ID 在低速时序下完成；ID 校验跳过厂商字节 id[0]。
 *     - 本函数**不切高速模式**：启动全程保持低速，保证初始化时序余量；
 *       切换由 nand_enable_high_speed() 在 FTL boot/恢复完成后单独执行。
 *     - DMA 初始化失败仅回退 PIO，非致命。
 *   步骤编号（与函数体内 Step 标记一一对应）：
 *     Step 1 FMC 句柄 + PCC 时序（纯软件填结构体）
 *     Step 2 HAL_NAND_Init → MspInit（时钟/GPIO/NCE）★首次碰硬件
 *     Step 3 per-[ch][ce][lun] FMC 后端实例
 *     Step 4 DMA（本配置 no-op，失败仅回退 PIO）
 *     Step 5 RESET（FFh）★启动命令绕过 dispatch，直连 engine
 *     Step 6 READ_ID + 厂商 ID 校验
 *     Step 6b 读 ONFI 参数页（Table 11）+ 打印设备信息（非致命，失败仅 LOGW）
 *   高速模式切换不在这里：见 nand_enable_high_speed()。
 *   调用链：main.c → nand_init → {HAL_NAND_Init,
 *          nand_fmc_init, nand_fc_reset, nand_fc_read_id,
 *          nand_fc_read_parameter_page}；
 *          随后 main.c 在 ssd_ftl_boot_all() 成功后调 nand_enable_high_speed()。
 *   流程图见 docs/design/fc_nand_design.md §22（图 16）。
 * -------------------------------------------------------------------- */
BYTE nand_init(void)
{
    FMC_NAND_PCC_TimingTypeDef common_timing;
    FMC_NAND_PCC_TimingTypeDef attribute_timing;
    nand_fc_completion_t       completion;
    BYTE                       id[5];
    BYTE                       result;

    memset(&common_timing, 0, sizeof(common_timing));
    memset(&attribute_timing, 0, sizeof(attribute_timing));

    /* ── Step 1: FMC 句柄 + PCC 时序（纯软件，尚未碰硬件）──
     * Bank3 / 8 位总线 / ECC 关闭；时序先给 power-on 默认 Mode 0 的保守值。 */
    g_nand_handle.Instance             = FMC_NAND_DEVICE;
    g_nand_handle.Init.NandBank        = FMC_NAND_BANK3;
    g_nand_handle.Init.Waitfeature     = FMC_NAND_PCC_WAIT_FEATURE_DISABLE;
    g_nand_handle.Init.MemoryDataWidth = FMC_NAND_PCC_MEM_BUS_WIDTH_8;
    g_nand_handle.Init.EccComputation  = FMC_NAND_ECC_DISABLE;
    g_nand_handle.Init.ECCPageSize     = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
    /* Start with the board-vendor's conservative timings before switching to the high-speed mode. */
    g_nand_handle.Init.TCLRSetupTime   = 8U;
    g_nand_handle.Init.TARSetupTime    = 8U;
    /*
     * TCLR/TAR 单位是 FMC 内核时钟周期（fmc_ker_ck，当前 PLL2 = 200 MHz，
     * T = 5 ns），不是 ns。取值下限由数据手册最小建立时间决定：
     *     周期数 = ceil(t_min_ns / T_fmc)
     * MT29F4G08ABADAWP：tCLR = 10 ns、tAR = 10 ns（见 g_nand_timing_high）
     *     → 理论下限 = ceil(10 / 4.55) = 3 周期
     * 这里填 8 是板厂给的保守值（≈ 36 ns，约 3.6× 余量），并非按公式推得；
     * Mode 5 分支再收紧到 4（≈ 18 ns，仍 ≥ 10 ns 下限）。
     */

    /*
     * FMC PCC 时序 → PMEM(通用空间)/PATT(属性空间) 寄存器，单位同为
     * fmc_ker_ck 周期（200 MHz，5 ns）。四字段 = NAND 访问三相 + 写向 HiZ：
     *     SetupTime     = max(tCLS, tALS, tCS)          命令/地址建立窗口
     *     WaitSetupTime = max(tRP, tWP)                 RE/WE 低电平脉宽
     *     HoldSetupTime = max(tCH, tALH, tCLH, tDH)     控制/数据保持
     *     HiZSetupTime  = max(tCS,tCLS,tALS)+(tWP−tDS)  写方向总线高阻窗口
     *                    （⚠ 与读方向的 tRHZ 无关；§19.4 早期误映射已更正）
     * 初值多为核心板厂商的保守值，按 ST 官方精确式核算（slow 表）：
     *     SET ≥ 1、WAIT ≥ 2、HOLD ≥ 2 → 本值 4/6/2 均有富余；
     *     HiZ 需 ≥ ceil(18/4.545) = 4 → 原厂商值 3 差 1 周期（疑似按 ~166 MHz
     *     时钟算得），已上调为 4。HiZ 属写方向，需上板写压力测试复核。
     * Mode 5 分支改由 nand_fmc_timing_to_pcc() 按同一公式重算。
     */
    common_timing.SetupTime     = 4U;
    common_timing.WaitSetupTime = 6U;
    common_timing.HoldSetupTime = 2U;
    common_timing.HiZSetupTime  = 4U;
    attribute_timing            = common_timing;

    /* ── Step 2: 首次碰硬件。HAL_NAND_Init 内部回调 HAL_NAND_MspInit
     *    （使能 FMC/GPIO 时钟 + 配 PD6=R/B#、PG9=NCE、数据总线 AF12）。── */
    if (HAL_NAND_Init(&g_nand_handle, &common_timing, &attribute_timing) != HAL_OK)
    {
        LOGE("[NAND] HAL_NAND_Init failed\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }
    LOGI("[NAND] FMC ready, R/B#=%u\r\n", (unsigned int)HAL_GPIO_ReadPin(NAND_RB_GPIO_PORT, NAND_RB_GPIO_PIN));

    {
        /* ── Step 3: FMC 后端实例（单 bank：全 LUN 共享窗口/时序） ──
         * 数据/命令/地址窗口 = FMC_BASE | 0 / A16 / A17。
         * FMC 时序寄存器是 bank 级，同 bank 各 LUN 共享窗口与时序，
         * 故一个后端实例即够——无需按 [CH][CE][LUN] 循环分化。 */
        nand_fmc_config_t fmc_config;
        memset(&fmc_config, 0, sizeof(fmc_config));
        fmc_config.data    = (volatile BYTE *)NAND_FMC_BASE_ADDR;
        fmc_config.command = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_CMD_OFFSET);
        fmc_config.address = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_ADDR_OFFSET);
        fmc_config.timing  = g_nand_timing_high;

        if (nand_fmc_init(&fmc_config) != NAND_RESULT_OK)
        {
            LOGE("[NAND] FMC backend configure failed\r\n");
            return NAND_RESULT_OPERATION_FAILED;
        }

        /* ── Step 4: DMA（CONFIG_NAND_DMA=0 时为 no-op；失败仅回退 PIO）── */
        if (nand_dma_init() != 0U)
        {
            LOGE("[NAND] DMA init failed, falling back to PIO\r\n");
            /* DMA 失败不是致命错误，回退到 CPU 逐字节搬运 */
        }
    }

    /* ── Step 5: RESET（FFh）──
     * Datasheet power-on rule: allow R/B# to become valid before the first FFh command.
     * ★ 启动命令经 nand_fc_execute 直连 nand_engine，绕过 fc / nand_dispatch
     *   命令队列——此刻 g_nand_dispatch_armed 仍为 0，nand_dispatch_submit 会拒收。 */
    delay_ms(1U);
    result = nand_fc_reset(&completion);
    if (result != NAND_RESULT_OK)
    {
        LOGE("[NAND] reset failed, result=0x%02X status=0x%02X R/B#=%u\r\n",
             (unsigned int)result,
             (unsigned int)completion.nand_status,
             (unsigned int)HAL_GPIO_ReadPin(NAND_RB_GPIO_PORT,
                                            NAND_RB_GPIO_PIN));
        return result;
    }
    LOGI("[NAND] reset complete, status=0x%02X\r\n", (unsigned int)completion.nand_status);
    /* ── Step 6: READ_ID + 厂商 ID 校验 ── */
    result = nand_fc_read_id(id, &completion);
    if (result != NAND_RESULT_OK)
    {
        LOGE("[NAND] read ID failed, result=0x%02X status=0x%02X\r\n", (unsigned int)result, (unsigned int)completion.nand_status);
        return result;
    }
    LOGI("[NAND] ID=%02X %02X %02X %02X %02X\r\n", (unsigned int)id[0], (unsigned int)id[1], (unsigned int)id[2], (unsigned int)id[3], (unsigned int)id[4]);
    /* 拼 id[1..4]（跳过厂商字节 id[0]=0x2C）与宏值比较，见 nand/devices/mt29f___g08a.h */
    if ((((DWORD)id[1] << 24U) | ((DWORD)id[2] << 16U) | ((DWORD)id[3] << 8U) | id[4]) != NAND_CHIP_ID)
    {
        LOGE("[NAND] unexpected ID\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }

    /* ── Step 6b: 读 ONFI 参数页并打印设备信息（手册 Table 11，256 字节）──
     * 同步路径（此刻 armed==0，契约允许）。
     * 读失败 / 签名不符 = 非致命（ID 已匹配 ⇒ 器件是好的，参数页读打嗝不该 brick）。
     * 但**解码成功后的地址周期数不符是致命的**：本仓 UOP 表固定发
     * NAND_COLUMN_ADDR_CYCLES 列 + NAND_ROW_ADDR_CYCLES 行，器件报告的周期数不同
     * ⇒ 每条命令都发错地址（静默写坏介质）⇒ release 档也必须拒绝启动。 */
    {
        BYTE                   page[NAND_ONFI_PARAM_PAGE_SIZE];
        nand_onfi_param_info_t info;

        result = nand_fc_read_parameter_page(page, &completion);
        if (result != NAND_RESULT_OK)
        {
            LOGW("[NAND] read parameter page failed, result=0x%02X status=0x%02X\r\n",
                 (unsigned int)result,
                 (unsigned int)completion.nand_status);
        }
        else if (nand_onfi_parameter_page_decode(page, &info) == 0U)
        {
            LOGW("[NAND] parameter page signature mismatch (got %02X %02X %02X %02X)\r\n",
                 (unsigned int)page[0],
                 (unsigned int)page[1],
                 (unsigned int)page[2],
                 (unsigned int)page[3]);
        }
        else
        {
            nand_onfi_param_print(&info);
            if ((info.column_cycles != NAND_COLUMN_ADDR_CYCLES) || (info.row_cycles != NAND_ROW_ADDR_CYCLES))
            {
                LOGE("[NAND] addr cycle mismatch: device C%u/R%u, driver C%u/R%u\r\n",
                     (unsigned int)info.column_cycles,
                     (unsigned int)info.row_cycles,
                     (unsigned int)NAND_COLUMN_ADDR_CYCLES,
                     (unsigned int)NAND_ROW_ADDR_CYCLES);
                return NAND_RESULT_INVALID_ARGUMENT;
            }
        }
    }

    LOGI("[NAND] init complete (low speed)\r\n");
    return NAND_RESULT_OK;
}

/* --------------------------------------------------------------------
 * nand_enable_high_speed
 *   功能：初始化全部完成后，把 NAND 切到 CONFIG_NAND_TIMING_MODE 并同步更新 FMC 时序。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：
 *     - 上电初始化（RESET/READ_ID/ONFI/FTL 元数据恢复）全程保持低速 Mode 0，
 *       保证启动路径的时序余量；本函数只在 boot 完成后调用一次。
 *     - armed 已置位，不能再走阻塞式 nand_fc_execute：用 nand_fc_start/step
 *       直接推进一次 SET_FEATURES 事务（此时无其它命令在飞）。
 *     - SET_FEATURES 成功后才改 FMC 时序；失败则保持低速，不影响功能。
 * -------------------------------------------------------------------- */
BYTE nand_enable_high_speed(DWORD fmc_hz)
{
#if (CONFIG_NAND_TIMING_MODE != 0U)
    const BYTE                 mode[4] = {(BYTE)CONFIG_NAND_TIMING_MODE, 0U, 0U, 0U};
    nand_fc_request_t          request;
    nand_fc_completion_t       completion;
    nand_fc_transaction_t      transaction;
    FMC_NAND_PCC_TimingTypeDef fast_pcc;
    BYTE                       result;

    if (fmc_hz == 0U)
    {
        /* 时钟树读取失败：按当前 sys_port.c 的 200 MHz 兜底，保证不出 0 周期 */
        LOGE("[NAND] FMC clock unknown, fallback to 200MHz\r\n");
        fmc_hz = 200000000U;
    }

    memset(&request, 0, sizeof(request));
    request.column      = 0x01U; /* ONFI timing mode feature address */
    request.data        = (BYTE *)mode;
    request.data_length = 4U;

    memset(&completion, 0, sizeof(completion));
    memset(&transaction, 0, sizeof(transaction));
    transaction.engine.backend    = nand_fmc_backend_get();
    transaction.engine.request    = &transaction.request;
    transaction.engine.completion = &transaction.completion;

    result = nand_fc_start(&transaction, OP_SET_FEATURES, &request);
    while (result == NAND_RESULT_NOT_READY)
    {
        result = nand_fc_step(&transaction, &completion);
    }
    if (result != NAND_RESULT_OK)
    {
        LOGE("[NAND] set feature 01h (Mode %u) failed, result=0x%02X status=0x%02X\r\n",
             (unsigned)CONFIG_NAND_TIMING_MODE,
             (unsigned)result,
             (unsigned)transaction.completion.nand_status);
        return result;
    }
    LOGI("[NAND] switched NAND to ONFI Mode %u\r\n", (unsigned)CONFIG_NAND_TIMING_MODE);

    const DWORD tclk_ps = (DWORD)(1000000000000ULL / (QWORD)fmc_hz);

    memset(&fast_pcc, 0, sizeof(fast_pcc));
    nand_fmc_timing_to_pcc(&g_nand_timing_high, &fast_pcc, fmc_hz);

    const DWORD tclr = NS_TO_PCC_CYCLES(g_nand_timing_high.t_clr_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;
    const DWORD tar  = NS_TO_PCC_CYCLES(g_nand_timing_high.t_ar_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;

    LOGI("[NAND] Mode %u PCC SET=%lu WAIT=%lu HOLD=%lu HiZ=%lu TCLR=%lu TAR=%lu (fmc=%luHz tHCLK=%lups)\r\n",
         (unsigned)CONFIG_NAND_TIMING_MODE,
         fast_pcc.SetupTime,
         fast_pcc.WaitSetupTime,
         fast_pcc.HoldSetupTime,
         fast_pcc.HiZSetupTime,
         tclr,
         tar,
         fmc_hz,
         tclk_ps);

    g_nand_handle.Init.TCLRSetupTime = tclr;
    g_nand_handle.Init.TARSetupTime  = tar;
    if (HAL_NAND_Init(&g_nand_handle, &fast_pcc, &fast_pcc) != HAL_OK)
    {
        LOGE("[NAND] HAL_NAND_Init (Mode %u) failed\r\n", (unsigned)CONFIG_NAND_TIMING_MODE);
        return NAND_RESULT_OPERATION_FAILED;
    }
    LOGI("[NAND] FMC timing updated for Mode %u\r\n", (unsigned)CONFIG_NAND_TIMING_MODE);

    {
        nand_fmc_config_t fast_fmc;

        memset(&fast_fmc, 0, sizeof(fast_fmc));
        fast_fmc.data    = (volatile BYTE *)NAND_FMC_BASE_ADDR;
        fast_fmc.command = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_CMD_OFFSET);
        fast_fmc.address = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_ADDR_OFFSET);
        fast_fmc.timing  = g_nand_timing_high;

        if (nand_fmc_init(&fast_fmc) != NAND_RESULT_OK)
        {
            LOGE("[NAND] FMC backend configure (Mode %u) failed\r\n", (unsigned)CONFIG_NAND_TIMING_MODE);
            return NAND_RESULT_OPERATION_FAILED;
        }
    }

    s_timing_mode = (BYTE)CONFIG_NAND_TIMING_MODE;
    return NAND_RESULT_OK;
#else
    LOGI("[NAND] high-speed mode disabled (CONFIG_NAND_TIMING_MODE=0)\r\n");
    return NAND_RESULT_OK;
#endif /* CONFIG_NAND_TIMING_MODE */
}

BYTE nand_get_timing_mode(void)
{
    return s_timing_mode;
}

/* --------------------------------------------------------------------
 * HAL_NAND_MspInit
 *   功能：HAL 平台的 MSP 回调（时钟 + GPIO 引脚复用配置）。
 *   参数：hnand —— HAL 句柄（本函数不使用）。
 *   返回：无。
 *   实现要点：由 HAL_NAND_Init 内部调用；使能 FMC/GPIOD/GPIOE/GPIOG
 *         时钟；引脚映射：
 *         GPIOD PIN6 = R/B#（上拉输入）
 *         GPIOG PIN9  = FMC NCE（片选，AF12）
 *         GPIOD 8/11/12/14/15 + GPIOE 7/8/9/10 = D0~D7 数据总线（AF12）
 *         （NWE/NOE/ALE/CLE 的 GPIO 由 HAL 句柄字段经 FMC 复用在
 *          本板设计下归入上述脚位范围）
 * -------------------------------------------------------------------- */
void HAL_NAND_MspInit(NAND_HandleTypeDef *hnand)
{
    GPIO_InitTypeDef gpio;

    memset(&gpio, 0, sizeof(gpio));
    (void)hnand;
    __HAL_RCC_FMC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* R/B#：上拉输入（busy 时为低） */
    gpio.Pin   = NAND_RB_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(NAND_RB_GPIO_PORT, &gpio);

    /* GPIOG PIN9：FMC NCE（片选，AF12） */
    gpio.Pin       = GPIO_PIN_9;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_HIGH;
    gpio.Alternate = GPIO_AF12_FMC;
    HAL_GPIO_Init(GPIOG, &gpio);

    /* GPIOD：数据总线低 8 位相关脚（AF12，由上一段 gpio 配置延续） */
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio);

    /* GPIOE：数据总线高 8 位相关脚（AF12） */
    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    HAL_GPIO_Init(GPIOE, &gpio);
}
