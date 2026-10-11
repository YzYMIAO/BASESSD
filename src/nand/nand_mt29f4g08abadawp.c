/*
 * nand_mt29f4g08abadawp.c — Micron MT29F4G08ABADAWP NAND 驱动
 *
 * 职责：
 *   - 芯片初始化（FMC 硬件 + DMA + RESET + READ_ID 校验）
 *   - FMC 时序换算（ONFI ns → PCC 寄存器周期数）
 *   - ONFI 操作表（g_nand_operation_map，定义于 onfi_1_0.c）的执行入口：
 *       nand_fc_execute/start/step → nand_engine → UOP → FMC 后端
 *
 * 芯片：Micron MT29F4G08ABADAWP，512MB SLC，tRC=50ns（Mode 0）/ 20ns（Mode 4）。
 * 物理几何见 nand_mt29f4g08abadawp.h；FC/FTL 逻辑几何见 nand_config.h。
 *
 * 时序来源：本文件两张静态表（slow/fast，MT29 数据手册参数）
 *   → nand_fmc_timing_t（nand_fmc.h，29 字段，顺序耦合）
 *   → UOP handler 层（nand_fmc.c）与 PCC 换算（nand_fmc_timing_to_pcc）。
 */

#include "nand_mt29f4g08abadawp.h"

#include "common/assertion.h"
#include "common/log.h"
#include "delay.h"
#include "dma.h"
#include "nand_config.h"
#include "nand_dispatch.h"
#include "nand_fmc.h"

#include <stddef.h>

/* 全局 HAL NAND 句柄：生命周期从 nand_init 起，供 FMC 初始化/时序更新复用 */
static NAND_HandleTypeDef g_nand_handle;

/* --------------------------------------------------------------------
 * g_nand_timing_slow — 低速时序（NAND 默认 Mode 0，tRC=50ns）
 *   29 个值按 nand_fmc_timing_t 字段顺序初始化（顺序耦合，错一位全错）。
 *   数值来源：MT29F4G08 数据手册时序参数表（Mode 0 值）。
 *   按组划分：
 *     地址锁存组  : tADL / tALH / tALS / tCS / tCH / tCLH / tCLS
 *     数据读写组  : tDH / tDS / tWC / tWH / tWP / tAR / tIR / tRC / tREA / tREH /
 *                   tRP / tRLOH / tRHOH / tRHW / tRHZ / tRR / tWHR / tWB
 *     就绪/复电组 : tCEA / tCHZ / tCLR / tCOH
 *   （Mode 0 典型值；本表被 nand_init 低速分支的初始 config 引用）
 * -------------------------------------------------------------------- */
static const nand_fmc_timing_t g_nand_timing_slow = {
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

/* --------------------------------------------------------------------
 * g_nand_timing_fast — 高速时序（ONFI Mode 4，tRC=20ns）
 *   29 个值同样按字段顺序初始化；仅 CONFIG_NAND_TIMING_MODE_4 编译。
 *   数值来源：MT29F4G08 数据手册 Mode 4 时序参数表。
 *   本表被 nand_init 高速分支（nand_fmc_timing_to_pcc + fast_fmc config）引用。
 * -------------------------------------------------------------------- */
#if (CONFIG_NAND_TIMING_MODE_4 != 0U)
static const nand_fmc_timing_t g_nand_timing_fast = {
    20U, /* tADL */
    5U,  /* tALH */
    5U,  /* tALS */
    5U,  /* tCH */
    5U,  /* tCLH */
    5U,  /* tCLS */
    10U, /* tCS */
    5U,  /* tDH */
    5U,  /* tDS */
    10U, /* tWC */
    5U,  /* tWH */
    5U,  /* tWP */
    10U, /* tAR */
    20U, /* tCEA（max） */
    30U, /* tCHZ（max） */
    10U, /* tCLR */
    10U, /* tCOH */
    0U,  /* tIR */
    10U, /* tRC */
    12U, /* tREA（max） */
    5U,  /* tREH */
    5U,  /* tRHOH */
    50U, /* tRHW */
    50U, /* tRHZ（max） */
    5U,  /* tRLOH */
    5U,  /* tRP */
    20U, /* tRR */
    50U, /* tWB（max） */
    30U, /* tWHR */
};
#endif /* CONFIG_NAND_TIMING_MODE_4 */

/*
 * 根据 ONFI 时序参数 nand_fmc_timing_t 和 FMC 时钟频率，
 * 计算 FMC PCC 硬件寄存器值。
 *
 * FMC kernel clock = 220 MHz → tHCLK ≈ 4.55 ns。
 * 所有 ONFI 时序量纲为 ns，向上取整到整周期数（保守偏慢）。
 *
 * 当前实现基于 MT29F4G08ABADAWP 异步时序协商规则：
 *   SetupTime     = max(tCLS, tALS, tCS)           / tHCLK
 *   WaitSetupTime = max(tRP, tWP)                  / tHCLK
 *   HoldSetupTime = max(tCH, tALH, tCLH, tDH)      / tHCLK
 *   HiZSetupTime  = tRHZ / tHCLK
 */
#if (CONFIG_NAND_TIMING_MODE_4 != 0U)
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_t    *onfi,
                                   FMC_NAND_PCC_TimingTypeDef *pcc)
{
    /* FMC kernel clock period in ns, fixed at compile time for STM32H743
     * 220MHz 周期 4.55ns，保守取整到 5ns（偏慢，安全方向） */
    const uint32_t hclk_ns = 5U;

#    define CEIL_DIV(a, b) (((a) + (b)-1U) / (b)) /* 向上取整除法（本函数内局部宏，末尾 undef） */

    /* max(tCLS, tALS, tCS) → SetupTime：命令/地址建立窗口取三者最大 */
    {
        uint32_t max_ns = onfi->t_cls_ns;
        if (onfi->t_als_ns > max_ns)
        {
            max_ns = onfi->t_als_ns;
        }
        if (onfi->t_cs_ns > max_ns)
        {
            max_ns = onfi->t_cs_ns;
        }
        pcc->SetupTime = (uint8_t)CEIL_DIV(max_ns, hclk_ns);
    }

    /* max(tRP, tWP) → WaitSetupTime：读/写脉冲低电平宽度取最大 */
    {
        uint32_t max_ns = onfi->t_rp_ns;
        if (onfi->t_wp_ns > max_ns)
        {
            max_ns = onfi->t_wp_ns;
        }
        pcc->WaitSetupTime = (uint8_t)CEIL_DIV(max_ns, hclk_ns);
    }

    /* max(tCH, tALH, tCLH, tDH) → HoldSetupTime：各保持时间取最大 */
    {
        uint32_t max_ns = onfi->t_ch_ns;
        if (onfi->t_alh_ns > max_ns)
        {
            max_ns = onfi->t_alh_ns;
        }
        if (onfi->t_clh_ns > max_ns)
        {
            max_ns = onfi->t_clh_ns;
        }
        if (onfi->t_dh_ns > max_ns)
        {
            max_ns = onfi->t_dh_ns;
        }
        pcc->HoldSetupTime = (uint8_t)CEIL_DIV(max_ns, hclk_ns);
    }

    /* HiZSetupTime：写方向总线高阻窗口。ST 官方式：
     *     HiZ × tHCLK ≥ max(tCS, tCLS, tALS) + (tWP − tDS)
     * ⚠ 与读方向的 tRHZ 无关（早期误用 tRHZ，已更正）。 */
    {
        uint32_t max_ns = onfi->t_cs_ns;
        uint32_t wp_ds  = (onfi->t_wp_ns > onfi->t_ds_ns) ? ((uint32_t)onfi->t_wp_ns - (uint32_t)onfi->t_ds_ns) : 0U;

        if (onfi->t_cls_ns > max_ns)
        {
            max_ns = onfi->t_cls_ns;
        }
        if (onfi->t_als_ns > max_ns)
        {
            max_ns = onfi->t_als_ns;
        }
        pcc->HiZSetupTime = (uint8_t)CEIL_DIV(max_ns + wp_ds, hclk_ns);
    }

#    undef CEIL_DIV
}
#endif /* CONFIG_NAND_TIMING_MODE_4 */

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
nand_result_t nand_fc_execute(uint8_t operation, const nand_fc_request_t *request, nand_fc_completion_t *completion)
{
    const nand_operation_map_t *map;
    nand_command_request_t      command_request;
    nand_command_completion_t   command_completion;
    uint8_t                     status;

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

    command_request = (nand_command_request_t){
        operation, request->column, {request->row[0], request->row[1]},
          request->data, request->data_length
    };
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
nand_result_t nand_fc_start(nand_fc_transaction_t *transaction, uint8_t operation, const nand_fc_request_t *request)
{
    const nand_operation_map_t *map;
    uint8_t                     result;

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

    LOGD("nand_fc_start: op=%u col=%u row=0x%06X data=%p len=%u\r\n",
         (unsigned)operation,
         (unsigned)request->column,
         (unsigned)request->row[0],
         (const void *)request->data,
         (unsigned)request->data_length);

    transaction->request = (nand_command_request_t){
        operation, request->column, {request->row[0], request->row[1]},
          request->data, request->data_length
    };
    transaction->completion = (nand_command_completion_t){0};
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
        LOGE_IF(result != NAND_RESULT_OK, "nand: start failed, op=%u result=0x%02X status=0x%02X\r\n", operation, result, transaction->completion.nand_status);
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
nand_result_t nand_fc_step(nand_fc_transaction_t *transaction, nand_fc_completion_t *completion)
{
    uint8_t result;

    /* 调用者只在事务 active 时推进（nand_dispatch.c 已先判 active） */
    ASSERT(transaction != NULL);
    ASSERT(completion != NULL);
    ASSERT(transaction->engine.active != 0U);

    result = nand_engine_step(&transaction->engine);
    if (result != NAND_RESULT_NOT_READY)
    {
        completion->execution_status = result;
        completion->nand_status      = transaction->completion.nand_status;
        LOGD("nand: complete, op=%u result=0x%02X status=0x%02X\r\n", transaction->request.operation, result, completion->nand_status);
        transaction->engine.active = 0U;
    }
    return result;
}

/* --------------------------------------------------------------------
 * nand_fc_reset
 *   功能：NAND 复位（同步）。
 *   返回：NAND_RESULT_OK / 错误码。
 * -------------------------------------------------------------------- */
nand_result_t nand_fc_reset(nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {.column = 0U, .row = {0U}, .data = NULL, .data_length = 0U};

    return nand_fc_execute(OP_RESET, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_fc_read_id
 *   功能：读取芯片 ID（5 字节，同步）。
 *   参数：id —— 输出缓冲（≥5 字节）；completion —— 状态。
 *   返回：NAND_RESULT_OK / 错误码。
 * -------------------------------------------------------------------- */
nand_result_t nand_fc_read_id(uint8_t id[5], nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {.column = 0U, .row = {0U}, .data = id, .data_length = 5U};

    ASSERT(id != NULL);
    return nand_fc_execute(OP_READ_ID, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_fc_set_features
 *   功能：写 Feature 寄存器（SET_FEATURES，同步）。
 *   参数：feature_address —— 寄存器地址；feature_data —— 4 字节值。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：column 复用为 feature 地址，data 长度固定 4。
 * -------------------------------------------------------------------- */
nand_result_t nand_fc_set_features(uint8_t feature_address, const uint8_t feature_data[4], nand_fc_completion_t *completion)
{
    const nand_fc_request_t request = {.column = feature_address, .row = {0U}, .data = (uint8_t *)feature_data, .data_length = 4U};

    ASSERT(feature_data != NULL);
    return nand_fc_execute(OP_SET_FEATURES, &request, completion);
}

/* --------------------------------------------------------------------
 * nand_init
 *   功能：NAND 芯片全初始化（FMC 硬件 + MSP + DMA +
 *         RESET + READ_ID 校验 + 可选 Mode 4 切换）。
 *   参数：无。
 *   返回：NAND_RESULT_OK / 错误码。
 *   实现要点：
 *     - FMC 用 Bank3、8 位总线、ECC 关闭；初始时序为 power-on
 *       默认 Mode 0 的保守寄存器周期数（4/6/2/3）。
 *     - RESET/READ_ID 在低速时序下完成；ID 校验跳过厂商字节 id[0]。
 *     - CONFIG_NAND_TIMING_MODE_4 时：SET_FEATURES(01h,4) 切 Mode 4，
 *       重新算 PCC 周期数并重配 FMC + 后端时序。
 *     - DMA 初始化失败仅回退 PIO，非致命。
 *   步骤编号（与函数体内 Step 标记一一对应）：
 *     Step 1 FMC 句柄 + PCC 时序（纯软件填结构体）
 *     Step 2 HAL_NAND_Init → MspInit（时钟/GPIO/NCE）★首次碰硬件
 *     Step 3 per-[ch][ce][lun] FMC 后端实例
 *     Step 4 DMA（本配置 no-op，失败仅回退 PIO）
 *     Step 5 RESET（FFh）★启动命令绕过 dispatch，直连 engine
 *     Step 6 READ_ID + 厂商 ID 校验
 *     Step 7 Mode 4 切换（CONFIG_NAND_TIMING_MODE_4=0 时整段编掉）
 *   调用链：main.c:320 → nand_init → {HAL_NAND_Init,
 *          nand_fmc_init, nand_fc_reset, nand_fc_read_id}
 *   流程图见 docs/design/fc_nand_design.md §22（图 16）。
 * -------------------------------------------------------------------- */
uint8_t nand_init(void)
{
    FMC_NAND_PCC_TimingTypeDef common_timing    = {0};
    FMC_NAND_PCC_TimingTypeDef attribute_timing = {0};
    nand_fc_completion_t       completion;
    uint8_t                    id[5];
    uint8_t                    result;

    /* ── Step 1: FMC 句柄 + PCC 时序（纯软件，尚未碰硬件）──
     * Bank3 / 8 位总线 / ECC 关闭；时序先给 power-on 默认 Mode 0 的保守值。 */
    g_nand_handle.Instance             = FMC_NAND_DEVICE;
    g_nand_handle.Init.NandBank        = FMC_NAND_BANK3;
    g_nand_handle.Init.Waitfeature     = FMC_NAND_PCC_WAIT_FEATURE_DISABLE;
    g_nand_handle.Init.MemoryDataWidth = FMC_NAND_PCC_MEM_BUS_WIDTH_8;
    g_nand_handle.Init.EccComputation  = FMC_NAND_ECC_DISABLE;
    g_nand_handle.Init.ECCPageSize     = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
    /* Start with the board-vendor's conservative timings before switching NAND to Mode 4. */
    g_nand_handle.Init.TCLRSetupTime   = 8U;
    g_nand_handle.Init.TARSetupTime    = 8U;
    /*
     * TCLR/TAR 单位是 FMC 内核时钟周期（fmc_ker_ck，见 sys.c PLL2 = 220 MHz，
     * T ≈ 4.55 ns），不是 ns。取值下限由数据手册最小建立时间决定：
     *     周期数 = ceil(t_min_ns / T_fmc)
     * MT29F4G08ABADAWP：tCLR = 10 ns、tAR = 10 ns（见 g_nand_timing_slow）
     *     → 理论下限 = ceil(10 / 4.55) = 3 周期
     * 这里填 8 是板厂给的保守值（≈ 36 ns，约 3.6× 余量），并非按公式推得；
     * Mode 4 分支再收紧到 4（≈ 18 ns，仍 ≥ 10 ns 下限）。
     */

    /*
     * FMC PCC 时序 → PMEM(通用空间)/PATT(属性空间) 寄存器，单位同为
     * fmc_ker_ck 周期（220 MHz，≈ 4.55 ns）。四字段 = NAND 访问三相 + 写向 HiZ：
     *     SetupTime     = max(tCLS, tALS, tCS)          命令/地址建立窗口
     *     WaitSetupTime = max(tRP, tWP)                 RE/WE 低电平脉宽
     *     HoldSetupTime = max(tCH, tALH, tCLH, tDH)     控制/数据保持
     *     HiZSetupTime  = max(tCS,tCLS,tALS)+(tWP−tDS)  写方向总线高阻窗口
     *                    （⚠ 与读方向的 tRHZ 无关；§19.4 早期误映射已更正）
     * 初值多为核心板厂商的保守值，按 ST 官方精确式核算（slow 表）：
     *     SET ≥ 1、WAIT ≥ 2、HOLD ≥ 2 → 本值 4/6/2 均有富余；
     *     HiZ 需 ≥ ceil(18/4.545) = 4 → 原厂商值 3 差 1 周期（疑似按 ~166 MHz
     *     时钟算得），已上调为 4。HiZ 属写方向，需上板写压力测试复核。
     * Mode 4 分支改由 nand_fmc_timing_to_pcc() 按同一公式重算。
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
        LOGE("nand: HAL_NAND_Init failed\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }
    LOGI("nand: FMC ready, R/B#=%u\r\n",
         (unsigned int)HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT,
                                        MT29F4G08ABADAWP_RB_GPIO_PIN));

    {
        /* ── Step 3: FMC 后端实例（单 bank：全 LUN 共享窗口/时序） ──
         * 数据/命令/地址窗口 = FMC_BASE | 0 / A16 / A17。
         * FMC 时序寄存器是 bank 级，同 bank 各 LUN 共享窗口与时序，
         * 故一个后端实例即够——无需按 [CH][CE][LUN] 循环分化。 */
        const nand_fmc_config_t fmc_config = {
            .data    = (volatile uint8_t *)MT29F4G08ABADAWP_FMC_BASE_ADDR,
            .command = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_CMD_OFFSET),
            .address = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_ADDR_OFFSET),
            .timing  = g_nand_timing_slow,
        };

        if (nand_fmc_init(&fmc_config) != NAND_RESULT_OK)
        {
            LOGE("nand: FMC backend configure failed\r\n");
            return NAND_RESULT_OPERATION_FAILED;
        }

        /* ── Step 4: DMA（CONFIG_NAND_DMA=0 时为 no-op；失败仅回退 PIO）── */
        if (nand_dma_init() != 0U)
        {
            LOGE("nand: DMA init failed, falling back to PIO\r\n");
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
        LOGE("nand: reset failed, result=0x%02X status=0x%02X R/B#=%u\r\n",
             (unsigned int)result,
             (unsigned int)completion.nand_status,
             (unsigned int)HAL_GPIO_ReadPin(MT29F4G08ABADAWP_RB_GPIO_PORT,
                                            MT29F4G08ABADAWP_RB_GPIO_PIN));
        return result;
    }
    LOGI("nand: reset complete, status=0x%02X\r\n", (unsigned int)completion.nand_status);
    /* ── Step 6: READ_ID + 厂商 ID 校验 ── */
    result = nand_fc_read_id(id, &completion);
    if (result != NAND_RESULT_OK)
    {
        LOGE("nand: read ID failed, result=0x%02X status=0x%02X\r\n",
             (unsigned int)result,
             (unsigned int)completion.nand_status);
        return result;
    }
    LOGI("nand: ID=%02X %02X %02X %02X %02X\r\n", (unsigned int)id[0], (unsigned int)id[1], (unsigned int)id[2], (unsigned int)id[3], (unsigned int)id[4]);
    /* 拼 id[1..4]（跳过厂商字节 id[0]=0x2C）与宏值比较，见 nand_mt29f4g08abadawp.h */
    if ((((uint32_t)id[1] << 24U) | ((uint32_t)id[2] << 16U) | ((uint32_t)id[3] << 8U) | id[4]) != MT29F4G08ABADAWP_ID)
    {
        LOGE("nand: unexpected ID\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }

#if CONFIG_NAND_TIMING_MODE_4
    /* ── Step 7: 高速模式：切 NAND 到 Mode 4 + 更新 FMC 硬件时序 ── */
    {
        const uint8_t              mode_4[4] = {4U, 0U, 0U, 0U};
        FMC_NAND_PCC_TimingTypeDef fast_pcc  = {0};

        /* 0x01 = ONFI 时序模式 Feature 寄存器地址，值 4 = Mode 4 */
        result = nand_fc_set_features(0x01U, mode_4, &completion);
        if (result != NAND_RESULT_OK)
        {
            LOGE("nand: set feature 01h (Mode 4) failed, result=0x%02X status=0x%02X\r\n",
                 (unsigned int)result,
                 (unsigned int)completion.nand_status);
            return result;
        }
        LOGI("nand: switched NAND to ONFI Mode 4\r\n");

        /* 根据 Mode 4 时序重新计算 FMC PCC 硬件参数 */
        nand_fmc_timing_to_pcc(&g_nand_timing_fast, &fast_pcc);
        g_nand_handle.Init.TCLRSetupTime = 4U; /* Mode 4 下 CLE→RE 延时收紧 */
        g_nand_handle.Init.TARSetupTime  = 4U; /* Mode 4 下 ALE→RE 延时收紧 */
        if (HAL_NAND_Init(&g_nand_handle, &fast_pcc, &fast_pcc) != HAL_OK)
        {
            LOGE("nand: HAL_NAND_Init (Mode 4) failed\r\n");
            return NAND_RESULT_OPERATION_FAILED;
        }
        LOGI("nand: FMC timing updated for Mode 4\r\n");

        /* 更新 FMC 后端时序，供 UOP handler 使用。
         * 单 bank：同一实例直接覆盖（无需按 [CH][CE][LUN] 循环）。 */
        {
            nand_fmc_config_t fast_fmc = {
                .data    = (volatile uint8_t *)MT29F4G08ABADAWP_FMC_BASE_ADDR,
                .command = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_CMD_OFFSET),
                .address = (volatile uint8_t *)(MT29F4G08ABADAWP_FMC_BASE_ADDR | MT29F4G08ABADAWP_FMC_ADDR_OFFSET),
                .timing  = g_nand_timing_fast,
            };

            if (nand_fmc_init(&fast_fmc) != NAND_RESULT_OK)
            {
                LOGE("nand: FMC backend configure (Mode 4) failed\r\n");
                return NAND_RESULT_OPERATION_FAILED;
            }
        }
    }
#else
    /* ── Step 7（编掉）：低速模式：跳过 Mode 4 切换，NAND 和 FMC 都运行在默认慢速时序 ── */
    LOGI("nand: keeping NAND at default timing (CONFIG_NAND_TIMING_MODE_4=0)\r\n");
#endif /* CONFIG_NAND_TIMING_MODE_4 */

    LOGI("nand: init complete\r\n");
    return NAND_RESULT_OK;
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
    GPIO_InitTypeDef gpio = {0};

    (void)hnand;
    __HAL_RCC_FMC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* R/B#：上拉输入（busy 时为低） */
    gpio.Pin   = MT29F4G08ABADAWP_RB_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_HIGH;
    HAL_GPIO_Init(MT29F4G08ABADAWP_RB_GPIO_PORT, &gpio);

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
