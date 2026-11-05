/*
 * mt29f___g08a.c — MT29FxxxG08A 族 NAND 驱动
 *
 * 职责：
 *   - 器件硬件初始化（FMC/GPIO/R-B# + FMC 后端实例）
 *   - 器件时序表与 ONFI ns → PCC 寄存器周期换算
 *   - 器件描述符：几何/时序/ID + hw_init/set_timing
 *
 * ONFI 命令序列与 UOP 引擎在 FC（src/fc/fc_onfi_1_0.c / fc_nand_engine.c）；
 * 本文件不再执行命令，只提供 NAND 器件/PHY 事实。
 *
 * 芯片：当前封装为 Micron MT29F4G08ABADAWP，512MB SLC，
 *       tRC=50ns（Mode 0）/ 20ns（Mode 5）。
 * 器件事实见 nand/devices/mt29f___g08a.h；通用布局见 nand/nand_config.h。
 *
 * 时序来源：本文件两张静态表（slow/fast，MT29 数据手册参数）
 *   → nand_fmc_timing_t（nand_protocol.h，29 字段，顺序耦合）
 *   → UOP handler 层（nand_fmc.c）与 PCC 换算（nand_fmc_timing_to_pcc）。
 */

#include "assertion.h"
#include "log.h"
#include "nand_config.h"
#include "nand_device.h"
#include "nand_fmc.h"

#include <stddef.h>
#include <string.h>

/* 全局 HAL NAND 句柄：生命周期从 fc_nand_init 起，供 FMC 初始化/时序更新复用 */
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
 *   （Mode 0 典型值；本表被 mt29f_hw_init 低速分支的初始 config 引用）
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
static void nand_fmc_timing_to_pcc(const nand_fmc_timing_t *onfi, FMC_NAND_PCC_TimingTypeDef *pcc, DWORD fmc_hz)
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

/* ══════════════════════════════════════════════════════════════
 * 上电拓扑扫描：只产出“板级物理事实”（哪些 target 存在、logical_units、
 * 逐 LUN present）。紧凑 die 编号与 LUN→die 表在 FTL 层（ftl_config.h / ftl.c）。
 * ══════════════════════════════════════════════════════════════ */
nand_target_info_t g_nand_targets[CH_MAX][CE_MAX];

/* 板级物理通路表：每一项表示 (ch,ce) 有一条可寻址的 NAND target。
 * 当前板：FMC Bank3 + 单个 NCE，只有 (0,0)。多 target 板卡在此扩展，
 * 并在 nand_select_target() 里实现对应的 FMC bank / CE# GPIO 切换。
 * ⚠ 这张表是**板级事实**，不是 NAND 器件属性。 */
typedef struct
{
    BYTE ch;
    BYTE ce;
} nand_board_target_t;

static const nand_board_target_t s_board_targets[] = {
    {0U, 0U},
};

/**
 * @brief 板级 target 选择（当前只校验通路表）
 *
 * @param ch 目标 ch
 * @param ce 目标 ce
 * @return OK / INVALID_ARGUMENT 越界 / NOT_READY 无通路
 */
nand_result_e nand_select_target(BYTE ch, BYTE ce)
{
    if ((ch >= CH_MAX) || (ce >= CE_MAX))
    {
        return NAND_RESULT_INVALID_ARGUMENT;
    }

    for (size_t i = 0U; i < (sizeof(s_board_targets) / sizeof(s_board_targets[0])); i++)
    {
        if ((s_board_targets[i].ch == ch) && (s_board_targets[i].ce == ce))
        {
            return NAND_RESULT_OK;
        }
    }

    return NAND_RESULT_NOT_READY;
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

    s_timing_mode = 0U; /* 上电/重新初始化一律从低速开始 */

    g_nand_handle.Instance             = FMC_NAND_DEVICE;
    g_nand_handle.Init.NandBank        = FMC_NAND_BANK3;
    g_nand_handle.Init.Waitfeature     = FMC_NAND_PCC_WAIT_FEATURE_DISABLE;
    g_nand_handle.Init.MemoryDataWidth = FMC_NAND_PCC_MEM_BUS_WIDTH_8;
    g_nand_handle.Init.EccComputation  = FMC_NAND_ECC_DISABLE;
    g_nand_handle.Init.ECCPageSize     = FMC_NAND_ECC_PAGE_SIZE_512BYTE;
    g_nand_handle.Init.TCLRSetupTime   = 8U;
    g_nand_handle.Init.TARSetupTime    = 8U;

    /* 低速保守 PCC：Setup/Wait/Hold/HiZ 均有数据手册最小值的余量。 */
    common_timing.SetupTime     = 4U;
    common_timing.WaitSetupTime = 6U;
    common_timing.HoldSetupTime = 2U;
    common_timing.HiZSetupTime  = 4U;
    attribute_timing            = common_timing;

    if (HAL_NAND_Init(&g_nand_handle, &common_timing, &attribute_timing) != HAL_OK)
    {
        LOGE(LOG_TAG_NAND "HAL_NAND_Init failed\r\n");
        return NAND_RESULT_OPERATION_FAILED;
    }
    LOGI(LOG_TAG_NAND "FMC ready, R/B#=%u\r\n", (unsigned int)HAL_GPIO_ReadPin(NAND_RB_GPIO_PORT, NAND_RB_GPIO_PIN));

    nand_fmc_config_t fmc_config;

    memset(&fmc_config, 0, sizeof(fmc_config));
    fmc_config.data    = (volatile BYTE *)NAND_FMC_BASE_ADDR;
    fmc_config.command = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_CMD_OFFSET);
    fmc_config.address = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_ADDR_OFFSET);
    fmc_config.timing  = g_nand_timing_high;

    if (nand_fmc_init(&fmc_config) != NAND_RESULT_OK)
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
static nand_result_e mt29f_set_timing(BYTE mode, DWORD fmc_hz)
{
#if (CONFIG_NAND_TIMING_MODE != 0U)
    FMC_NAND_PCC_TimingTypeDef fast_pcc;

    ASSERT(fmc_hz != 0U);
    memset(&fast_pcc, 0, sizeof(fast_pcc));
    nand_fmc_timing_to_pcc(&g_nand_timing_high, &fast_pcc, fmc_hz);

    const DWORD tclk_ps = (DWORD)(1000000000000ULL / (QWORD)fmc_hz);
    const DWORD tclr    = NS_TO_PCC_CYCLES(g_nand_timing_high.t_clr_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;
    const DWORD tar     = NS_TO_PCC_CYCLES(g_nand_timing_high.t_ar_ns, tclk_ps) + NAND_PCC_MARGIN_CYCLES;

    LOGI(LOG_TAG_NAND "Mode %u PCC SET=%lu WAIT=%lu HOLD=%lu HiZ=%lu TCLR=%lu TAR=%lu (fmc=%luHz tHCLK=%lups)\r\n", mode, fast_pcc.SetupTime, fast_pcc.WaitSetupTime, fast_pcc.HoldSetupTime, fast_pcc.HiZSetupTime, tclr, tar, fmc_hz, tclk_ps);

    g_nand_handle.Init.TCLRSetupTime = tclr;
    g_nand_handle.Init.TARSetupTime  = tar;
    if (HAL_NAND_Init(&g_nand_handle, &fast_pcc, &fast_pcc) != HAL_OK)
    {
        LOGE(LOG_TAG_NAND "HAL_NAND_Init (Mode %u) failed\r\n", mode);
        return NAND_RESULT_OPERATION_FAILED;
    }

    nand_fmc_config_t fast_fmc;

    memset(&fast_fmc, 0, sizeof(fast_fmc));
    fast_fmc.data    = (volatile BYTE *)NAND_FMC_BASE_ADDR;
    fast_fmc.command = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_CMD_OFFSET);
    fast_fmc.address = (volatile BYTE *)(NAND_FMC_BASE_ADDR | NAND_FMC_ADDR_OFFSET);
    fast_fmc.timing  = g_nand_timing_high;

    if (nand_fmc_init(&fast_fmc) != NAND_RESULT_OK)
    {
        LOGE(LOG_TAG_NAND "FMC backend configure (Mode %u) failed\r\n", mode);
        return NAND_RESULT_OPERATION_FAILED;
    }

    s_timing_mode = mode;
    LOGI(LOG_TAG_NAND "FMC timing updated for Mode %u\r\n", mode);
    return NAND_RESULT_OK;
#else
    (void)mode;
    (void)fmc_hz;
    return NAND_RESULT_OK;
#endif /* CONFIG_NAND_TIMING_MODE */
}

static const nand_device_desc_t g_mt29f_desc = {
    .name        = "MT29F4G08ABADAWP",
    .expected_id = NAND_CHIP_ID,
    .hw_init     = mt29f_hw_init,
    .set_timing  = mt29f_set_timing,
};

const nand_device_desc_t *nand_device_get(void)
{
    return &g_mt29f_desc;
}

/**
 * @brief 当前 NAND 时序模式
 *
 * @return 模式号（0 = 低速）
 */
BYTE nand_get_timing_mode(void)
{
    return s_timing_mode;
}

/**
 * @brief HAL 平台 MSP 回调：时钟 + GPIO 引脚复用
 *
 * 由 HAL_NAND_Init 内部调用；R/B# 上拉输入，NCE/数据总线 AF12。
 *
 * @param hnand HAL 句柄（未使用）
 */
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
