/*
 * mt29f___g08a.c — MT29FxxxG08A 族 NAND 驱动
 *
 * 职责：
 *   - 器件硬件初始化（FMC/GPIO/R-B# + FMC 后端实例）
 *   - 器件时序表与 ONFI ns → PCC 寄存器周期换算
 *   - 器件描述符：几何/时序/ID + hw_init/set_timing
 *
 * ONFI 命令序列在 NAND（src/nand/onfi_1_0.c），UOP 引擎在 FC
 * （src/fc/fc_nand_engine.c）；本文件只提供 NAND 器件/PHY 事实。
 *
 * 芯片：当前封装为 Micron MT29F4G08ABADAWP，512MB SLC，
 *       tRC=50ns（Mode 0）/ 20ns（Mode 5）。
 * 器件事实见 nand/devices/mt29f___g08a.h；通用布局见 nand/nand_config.h。
 *
 * 时序来源：本文件两张静态表（slow/fast，MT29 数据手册参数）
 *   → nand_fmc_timing_s（nand_protocol.h，29 字段，顺序耦合）
 *   → UOP handler 层（nand_fmc.c）与 PCC 换算（nand_fmc_timing_to_pcc）。
 */

#include "assertion.h"
#include "log.h"
#include "nand_config.h"
#include "nand_device.h"
#include "nand_fmc.h"

#include <string.h>

/* 全局 HAL NAND 句柄：生命周期从 fc_nand_init 起，供 FMC 初始化/时序更新复用 */
static NAND_HandleTypeDef g_nand_handle;

/* 当前实际生效的 ONFI timing mode（0 = 仍为低速）。[STAT] 诊断用。 */
static BYTE s_timing_mode[CH_NUM][CE_NUM];

/* 低速 bring-up 的 FMC PCC 值（FMC 周期）：对 Mode0 数据手册最小值留保守余量。
 * 不是从 AC 表推导——上电时先保证任何时序边缘都能读 ID/参数页。 */
#define MT29F_LOW_PCC_SETUP (4U)
#define MT29F_LOW_PCC_WAIT  (6U)
#define MT29F_LOW_PCC_HOLD  (2U)
#define MT29F_LOW_PCC_HIZ   (4U)

#if (CONFIG_NAND_TIMING_MODE != 0U)

/* --------------------------------------------------------------------
 * g_nand_timing_mode0 — MT29F4G08 3.3V 时序（数据手册 Table 29/31）
 *   29 个值按字段名初始化（designated initializer），字段重排不会错位。
 *   本表固定使用 Table 29/31 的 3.3V AC 参数；CONFIG_NAND_TIMING_MODE 只改变
 *   SET_FEATURES 的模式值（4 或 5），用于在**相同 FMC 时序**下对比两种模式。
 *   注意：Mode 4 的独立 AC 表在数据手册 Table 32（1.8V），本板 3.3V 实验不采用它。
 *   按组划分：
 *     地址锁存组  : tADL / tALH / tALS / tCS / tCH / tCLH / tCLS
 *     数据读写组  : tDH / tDS / tWC / tWH / tWP / tAR / tIR / tRC / tREA / tREH /
 *                   tRP / tRLOH / tRHOH / tRHW / tRHZ / tRR / tWHR / tWB
 *     就绪/复电组 : tCEA / tCHZ / tCLR / tCOH
 *   （Mode 0 典型值；本表供 mt29f_set_timing 的 PCC 换算使用）
 * -------------------------------------------------------------------- */
static const nand_fmc_timing_s g_nand_timing_mode0 = {
    .t_adl_ns     = 70U,  /* 地址→数据加载延时 */
    .t_alh_ns     = 5U,   /* ALE 保持 */
    .t_als_ns     = 10U,  /* ALE 建立 */
    .t_ch_ns      = 5U,   /* CE 保持 */
    .t_clh_ns     = 5U,   /* CLE 保持 */
    .t_cls_ns     = 10U,  /* CLE 建立 */
    .t_cs_ns      = 15U,  /* CE 建立 */
    .t_dh_ns      = 5U,   /* 数据保持 */
    .t_ds_ns      = 7U,   /* 数据建立 */
    .t_wc_ns      = 20U,  /* 写周期 */
    .t_wh_ns      = 7U,   /* WE 高电平宽度 */
    .t_wp_ns      = 10U,  /* WE 低电平宽度 */
    .t_ar_ns      = 10U,  /* ALE→RE 延时 */
    .t_cea_max_ns = 25U,  /* CE→输出有效（max） */
    .t_chz_max_ns = 50U,  /* CE→输出高阻（max） */
    .t_clr_ns     = 10U,  /* CLE→RE 延时 */
    .t_coh_ns     = 15U,  /* CE 高→输出保持 */
    .t_ir_ns      = 0U,   /* 命令→数据输出间隔（Mode0 为 0） */
    .t_rc_ns      = 20U,  /* 读周期 */
    .t_rea_max_ns = 16U,  /* RE→输出有效（max） */
    .t_reh_ns     = 7U,   /* RE 高电平宽度 */
    .t_rhoh_ns    = 15U,  /* RE 高→输出保持 */
    .t_rhw_ns     = 100U, /* RE→WE 延时 */
    .t_rhz_max_ns = 100U, /* RE→输出高阻（max） */
    .t_rloh_ns    = 5U,   /* RE 低→输出保持 */
    .t_rp_ns      = 10U,  /* RE 低电平宽度 */
    .t_rr_ns      = 20U,  /* 读命令→读命令间隔 */
    .t_wb_max_ns  = 100U, /* WE 高→busy（max） */
    .t_whr_ns     = 60U,  /* 状态→RE 高延时 */
};
#endif /* CONFIG_NAND_TIMING_MODE */

/*
 * 根据 ONFI 时序参数 nand_fmc_timing_s 和 FMC 时钟频率，
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
 * 按实际 fmc_hz 换算（不能硬编码 5ns/4545ps）。Mode 4 表实测 HOLD/HiZ 各短 1 周期
 * 会导致命令/地址采样失败（RISK-NAND-03），故使用 Mode 0 的 3.3V AC 参数作为基准。
 */
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_s *onfi, FMC_NAND_PCC_TimingTypeDef *pcc, DWORD fmc_hz)
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

/**
 * @brief 装配 FMC Bank3 三窗口配置（器件/板级窗口地址）
 *
 * data/command/address 指向同一 FMC Bank3 的三个 A16/A17 别名窗口；
 * 具体 GPIO/FMC 初始化由 board 层的 HAL_NAND_MspInit 完成。
 *
 * @param cfg 输出配置
 */
static void mt29f_build_fmc_config(nand_fmc_cfg_s *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->data    = (volatile BYTE *)NAND_FMC_BASE_ADDR;
    cfg->command = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_CMD_OFFSET);
    cfg->address = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_ADDR_OFFSET);
    cfg->rb_port = NAND_RB_GPIO_PORT;
    cfg->rb_pin  = NAND_RB_GPIO_PIN;
}

/**
 * @brief 器件硬件初始化：HAL/FMC/GPIO + FMC 后端实例
 *
 * 只碰硬件，不做 ONFI 命令/拓扑扫描；后者由 FC 的 fc_nand_init 编排。
 *
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e mt29f_hw_init(void)
{
    FMC_NAND_PCC_TimingTypeDef common_timing;
    FMC_NAND_PCC_TimingTypeDef attribute_timing;

    memset(&common_timing, 0, sizeof(common_timing));
    memset(&attribute_timing, 0, sizeof(attribute_timing));

    memset(s_timing_mode, 0, sizeof(s_timing_mode)); /* 上电/重新初始化一律从低速开始 */

    g_nand_handle.Instance             = FMC_NAND_DEVICE;
    g_nand_handle.Init.NandBank        = FMC_NAND_BANK3;
    g_nand_handle.Init.Waitfeature     = FMC_NAND_PCC_WAIT_FEATURE_DISABLE;
    g_nand_handle.Init.MemoryDataWidth = FMC_NAND_PCC_MEM_BUS_WIDTH_8;
    g_nand_handle.Init.EccComputation  = FMC_NAND_ECC_DISABLE;
    g_nand_handle.Init.ECCPageSize     = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
    g_nand_handle.Init.TCLRSetupTime   = 8U;
    g_nand_handle.Init.TARSetupTime    = 8U;

    /* 低速保守 PCC：Setup/Wait/Hold/HiZ 均有数据手册最小值的余量。 */
    common_timing.SetupTime     = MT29F_LOW_PCC_SETUP;
    common_timing.WaitSetupTime = MT29F_LOW_PCC_WAIT;
    common_timing.HoldSetupTime = MT29F_LOW_PCC_HOLD;
    common_timing.HiZSetupTime  = MT29F_LOW_PCC_HIZ;
    attribute_timing            = common_timing;

    if (HAL_NAND_Init(&g_nand_handle, &common_timing, &attribute_timing) != HAL_OK)
    {
        LOGE(LOG_TAG_NAND "HAL_NAND_Init failed\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }

    nand_fmc_cfg_s fmc_config;

    mt29f_build_fmc_config(&fmc_config);

    if (nand_fmc_init_target(0U, 0U, &fmc_config) != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "FMC backend configure failed\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }
    return NAND_RESULT_OK;
}

/**
 * @brief SET_FEATURES 成功后按 mode/fmc_hz 重配 FMC 时序与后端
 *
 * @param mode   已生效的 ONFI timing mode
 * @param fmc_hz FMC 内核时钟（Hz）
 * @return NAND_RESULT_OK / 错误码
 */
static nand_result_e mt29f_set_timing(BYTE ch, BYTE ce, BYTE mode, DWORD fmc_hz)
{
#if (CONFIG_NAND_TIMING_MODE != 0U)
    FMC_NAND_PCC_TimingTypeDef fast_pcc;

    ASSERT(fmc_hz != 0U);
    ASSERT((ch < CH_NUM) && (ce < CE_NUM));
    memset(&fast_pcc, 0, sizeof(fast_pcc));
    nand_fmc_timing_to_pcc(&g_nand_timing_mode0, &fast_pcc, fmc_hz);

    const DWORD tclk_ps = (DWORD)(1000000000000ULL / (QWORD)fmc_hz);
    const DWORD tclr    = NS_TO_PCC_CYCLES(g_nand_timing_mode0.t_clr_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;
    const DWORD tar     = NS_TO_PCC_CYCLES(g_nand_timing_mode0.t_ar_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;

    LOGI(LOG_TAG_NAND "Mode %u PCC SET=%lu WAIT=%lu HOLD=%lu HiZ=%lu TCLR=%lu TAR=%lu (fmc=%luHz tHCLK=%lups)\r\n", mode, fast_pcc.SetupTime, fast_pcc.WaitSetupTime, fast_pcc.HoldSetupTime, fast_pcc.HiZSetupTime, tclr, tar, fmc_hz, tclk_ps);

    g_nand_handle.Init.TCLRSetupTime = tclr;
    g_nand_handle.Init.TARSetupTime  = tar;
    if (HAL_NAND_Init(&g_nand_handle, &fast_pcc, &fast_pcc) != HAL_OK)
    {
        LOGE(LOG_TAG_NAND "HAL_NAND_Init (Mode %u) failed\r\n", mode);
        return NAND_RESULT_OPERATION_FAILED;
    }

    nand_fmc_cfg_s fast_fmc;

    mt29f_build_fmc_config(&fast_fmc);

    if (nand_fmc_init_target(ch, ce, &fast_fmc) != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "FMC backend configure (Mode %u) failed\r\n", mode);
        return NAND_RESULT_OPERATION_FAILED;
    }

    s_timing_mode[ch][ce] = mode;
    LOGI(LOG_TAG_NAND "FMC timing updated for Mode %u\r\n", mode);
    return NAND_RESULT_OK;
#else
    (void)ch;
    (void)ce;
    (void)mode;
    (void)fmc_hz;
    return NAND_RESULT_OK;
#endif /* CONFIG_NAND_TIMING_MODE */
}

static BYTE mt29f_get_timing_mode(BYTE ch, BYTE ce);

static const nand_device_desc_s g_mt29f_desc = {
    .name            = "MT29F4G08ABADAWP",
    .expected_id     = NAND_CHIP_ID,
    .hw_init         = mt29f_hw_init,
    .set_timing      = mt29f_set_timing,
    .get_timing_mode = mt29f_get_timing_mode,
};

const nand_device_desc_s *nand_device_get(void)
{
    return &g_mt29f_desc;
}

/**
 * @brief 当前 NAND 时序模式（器件内部实现，经 nand_device.h 的通用接口暴露）
 *
 * @return 模式号（0 = 低速）
 */
static BYTE mt29f_get_timing_mode(BYTE ch, BYTE ce)
{
    ASSERT((ch < CH_NUM) && (ce < CE_NUM));
    return s_timing_mode[ch][ce];
}
