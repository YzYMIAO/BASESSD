/*
 * cli.c — UART 调试命令行（应用层）
 *
 * 职责：行编辑/回显、分词、命令表，以及 wr/rd/tr/fl/info/test 等命令。
 * 这是唯一直接驱动 controller/FTL 全链路的调试前端：wr/rd/tr/fl/info 经
 * ssd_controller_submit(&g_ctrl, ...) 模拟 NVMe host IO。
 *
 * 与传输层（src/platform/usart.c）的分工：本模块只调 uart_rx_read 取字节、
 * uart_log_write 输出、uart_get_stats 读诊断计数，不碰环形队列/寄存器。
 */

#include "controller/cli.h"

#include "common/common.h"
#include "common/log.h"
#include "controller/ssd_request.h"
#include "ftl/bbt.h"
#include "ftl/ftl.h"
#include "usart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FC_NAND_TEST_ENABLE)
#    include "ut_main.h"
#endif

#define USART_CLI_LINE_CAPACITY 128U
#define UART_CLI_MAX_ARGUMENTS  10U
#define UART_ARRAY_SIZE(array)  (sizeof(array) / sizeof((array)[0]))

/*
 * CLI 打印 lba / FTL_LBA_MAX 一律用 %lu，不用 %llu：
 * 本工具链的 newlib-nano（-specs=nano.specs，newlib 3.3.0）不带 long long
 * 格式化支持，%llu 会把 "lu" 当字面量原样打出来（`lba_max=%llu` → `lba_max=lu`），
 * 而 GCC 的 -Wformat 按标准语义检查，编译期不会报。这些值都受
 * uart_cli_lba_range_ok() 约束在 FTL_LBA_MAX 以内，装得下 32 位才安全。
 */
_Static_assert(FTL_LBA_MAX <= 0xFFFFFFFFUL, "FTL_LBA_MAX 超出 32 位，CLI 的 %lu 打印会截断");

static char     g_cli_line[USART_CLI_LINE_CAPACITY];
static uint16_t g_cli_length;
static uint8_t  g_cli_discarding;

/*
 * 命令表把“命令字符串 -> 参数约束 -> 处理函数”的关系集中管理。
 * argc 包含命令名自身，例如 "log level debug" 被切分后 argc 为 3。
 */
typedef int (*uart_cli_handler_t)(int argc, char *argv[]);

typedef struct
{
    const char        *name;
    const char        *usage;
    const char        *description;
    uint8_t            min_argc;
    uint8_t            max_argc;
    uart_cli_handler_t handler;
} uart_cli_command_t;

static int uart_cli_help(int argc, char *argv[]);
static int uart_cli_status(int argc, char *argv[]);
static int uart_cli_version(int argc, char *argv[]);
static int uart_cli_log(int argc, char *argv[]);
static int uart_cli_wr(int argc, char *argv[]);
static int uart_cli_rd(int argc, char *argv[]);
static int uart_cli_tr(int argc, char *argv[]);
static int uart_cli_fl(int argc, char *argv[]);
static int uart_cli_info(int argc, char *argv[]);
#if defined(FC_NAND_TEST_ENABLE)
static int uart_cli_test(int argc, char *argv[]);
#endif

static void uart_write_prompt(void)
{
    static const char prompt[] = "ssd> ";

    (void)uart_log_write(prompt, sizeof(prompt) - 1U);
}

static void uart_print_status(void)
{
    uart_stats_t stats;

    uart_get_stats(&stats);
    (void)LOGI("uart: rx_ovf=%lu tx_drop=%lu tx_ctx=%lu flush_to=%lu dma_err=%lu tx_fault=%lu ore=%lu fe=%lu ne=%lu pe=%lu\r\n",
               (unsigned long)stats.rx_overflow,
               (unsigned long)stats.tx_dropped,
               (unsigned long)stats.tx_context_errors,
               (unsigned long)stats.tx_flush_timeouts,
               (unsigned long)stats.tx_dma_errors,
               (unsigned long)stats.tx_transport_errors,
               (unsigned long)stats.overrun_errors,
               (unsigned long)stats.framing_errors,
               (unsigned long)stats.noise_errors,
               (unsigned long)stats.parity_errors);
}

/*
 * UART CLI 驱动 FTL（C 档，设计文档 §13.4 D10：Host 命令来源 = UART CLI）。
 *
 * 固定 pattern 写 buffer：16KB，落 SRAM2（.sram2_data 为 NOLOAD 段，上电不清零
 * → cli_init 主动预填）。前 8KB = 0x55555555，后 8KB = 0xAAAAAAAA。
 *
 * ⚠ CW 级映射（IU = 1 CW = 512B）：`wr lba n` / `rd lba n` 的 n 单位是**扇区**
 *   （每个 512B），缓冲步进 = FTL_IU_BYTES。故 **n 上限 = 16KB / 512B = 32**
 *   （旧页级模型下 n 计 2048B 页，上限 8）。超出由 controller 的 buffer_size
 *   校验拒绝（写爆 g_wr_buf 是真实隐患）。
 */
static uint8_t g_wr_buf[FTL_READ_USER_SIZE * CW_NUM * PLANE_NUM] /* 16KB → SRAM2 */
    __attribute__((section(".sram2_data")));

/* n（扇区数）上限：g_wr_buf 容量 ÷ 映射单元字节数 */
#define UART_CLI_MAX_SECTORS ((uint32_t)(sizeof(g_wr_buf) / FTL_IU_BYTES))

/* wr/rd 两段式：handler 只提交请求到 controller（异步），结果由
 * cli_service_poll 每轮末尾检查 ssd_controller_is_complete 打印（D11
 * 非阻塞 pending 表语义；主循环的 ftl_service 完成请求）。 */
typedef struct
{
    uint8_t       busy;    /* 1=有未决 CLI 请求 */
    ssd_request_t req;     /* 未决请求（controller 拆分子请求跟踪其 complete_flag） */
    uint8_t       compare; /* rd: 1=compare 模式，0=hex dump */
    uint64_t      lba;     /* 本次请求起始 LBA（结果打印用） */
    uint32_t      nblocks; /* 本次请求块数 */
} uart_cli_pending_t;

static uart_cli_pending_t g_cli_pending;

/* 预填 g_wr_buf pattern（.sram2_data NOLOAD，必须主动初始化） */
static void uart_cli_pattern_init(void)
{
    uint32_t  i;
    uint32_t  words_per_half = (uint32_t)((FTL_READ_USER_SIZE * CW_NUM) / sizeof(uint32_t)); /* 8KB/4 = 2048 */
    uint32_t *lo             = (uint32_t *)(void *)g_wr_buf;
    uint32_t *hi             = (uint32_t *)(void *)(g_wr_buf + FTL_READ_USER_SIZE * CW_NUM);

    for (i = 0U; i < words_per_half; i++)
    {
        lo[i] = 0x55555555U;
        hi[i] = 0xAAAAAAAAU;
    }
}

/* 从 g_wr_buf 对应区取期望 pattern 值（compare 模式用；前 8KB/后 8KB 两段） */
static uint32_t uart_cli_expect_word(uint32_t byte_offset)
{
    return (byte_offset < (uint32_t)(FTL_READ_USER_SIZE * CW_NUM))
               ? 0x55555555U
               : 0xAAAAAAAAU;
}

/* 提交前的 LBA 范围检查（lba+n ≤ FTL_LBA_MAX） */
static int uart_cli_lba_range_ok(uint64_t lba, uint32_t n)
{
    uint64_t end = lba + (uint64_t)n;

    return (lba < FTL_LBA_MAX) && (end <= FTL_LBA_MAX);
}

/* 解析十进制/十六进制数。成功且全部消费返回 0；失败/残留返回 -1。 */
static int uart_cli_parse_uint(const char *text, uint64_t *out)
{
    char    *end = NULL;
    uint64_t value;

    if ((text == NULL) || (text[0] == '\0'))
    {
        return -1;
    }
    value = strtoull(text, &end, 0);
    if ((end == NULL) || (*end != '\0'))
    {
        return -1; /* 有非法字符残留 */
    }
    if (out != NULL)
    {
        *out = value;
    }
    return 0;
}

static int uart_cli_wr(int argc, char *argv[])
{
    uint64_t lba;
    uint64_t n_parsed;
    uint32_t n;
    char    *end = NULL;

    (void)argc;

    if (g_cli_pending.busy != 0U)
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }

    lba = strtoull(argv[1], &end, 0);
    if ((end == NULL) || (*end != '\0') || (uart_cli_parse_uint(argv[2], &n_parsed) != 0) ||
        (n_parsed == 0U) || (n_parsed > 0xFFFFFFFFU))
    {
        (void)uart_log_write("error: invalid lba/n\r\n", 22U);
        return FUNC_STATUS_ERROR;
    }
    n = (uint32_t)n_parsed;
    if (n > UART_CLI_MAX_SECTORS)
    {
        (void)uart_log_write("error: n exceeds g_wr_buf (max 32 sectors)\r\n", 44U);
        return FUNC_STATUS_ERROR;
    }
    if (!uart_cli_lba_range_ok(lba, n))
    {
        (void)uart_log_write("error: lba range exceeds lba_max\r\n", 34U);
        return FUNC_STATUS_ERROR;
    }

    g_cli_pending.busy    = 1U;
    g_cli_pending.compare = 0U;
    g_cli_pending.lba     = lba;
    g_cli_pending.nblocks = n;

    memset(&g_cli_pending.req, 0, sizeof(g_cli_pending.req));
    g_cli_pending.req.opcode      = SSD_REQUEST_WRITE;
    g_cli_pending.req.start_lba   = lba;
    g_cli_pending.req.block_count = n;
    g_cli_pending.req.buffer      = (void *)g_wr_buf;
    g_cli_pending.req.buffer_size = sizeof(g_wr_buf); /* 真实缓冲容量（非 n×2KB）——
                                                       * controller 据此拒绝会写爆 g_wr_buf 的 n */

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)LOGI("wr submit ok lba=%lu n=%lu, waiting...\r\n", (unsigned long)lba, (unsigned long)n);
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_rd(int argc, char *argv[])
{
    uint64_t lba;
    uint64_t n_parsed;
    uint32_t n;
    char    *end = NULL;

    if (g_cli_pending.busy != 0U)
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }

    lba = strtoull(argv[1], &end, 0);
    if ((end == NULL) || (*end != '\0') || (uart_cli_parse_uint(argv[2], &n_parsed) != 0) ||
        (n_parsed == 0U) || (n_parsed > 0xFFFFFFFFU))
    {
        (void)uart_log_write("error: invalid lba/n\r\n", 22U);
        return FUNC_STATUS_ERROR;
    }
    n = (uint32_t)n_parsed;
    if (n > UART_CLI_MAX_SECTORS)
    {
        (void)uart_log_write("error: n exceeds g_wr_buf (max 32 sectors)\r\n", 44U);
        return FUNC_STATUS_ERROR;
    }
    if (!uart_cli_lba_range_ok(lba, n))
    {
        (void)uart_log_write("error: lba range exceeds lba_max\r\n", 34U);
        return FUNC_STATUS_ERROR;
    }

    g_cli_pending.busy    = 1U;
    g_cli_pending.compare = (argc >= 4) && (strcmp(argv[3], "c") == 0) ? 1U : 0U;
    g_cli_pending.lba     = lba;
    g_cli_pending.nblocks = n;

    memset(&g_cli_pending.req, 0, sizeof(g_cli_pending.req));
    g_cli_pending.req.opcode      = SSD_REQUEST_READ;
    g_cli_pending.req.start_lba   = lba;
    g_cli_pending.req.block_count = n;
    g_cli_pending.req.buffer      = (void *)g_wr_buf; /* 读数据覆盖 pattern；compare 模式直接与 pattern 比对 */
    g_cli_pending.req.buffer_size = sizeof(g_wr_buf); /* 真实缓冲容量（非 n×2KB），见 uart_cli_wr */

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)LOGI("rd submit ok lba=%lu n=%lu%s, waiting...\r\n", (unsigned long)lba, (unsigned long)n, g_cli_pending.compare ? " c" : "");
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_tr(int argc, char *argv[])
{
    uint64_t lba;

    (void)argc;

    if (g_cli_pending.busy != 0U)
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }
    if (uart_cli_parse_uint(argv[1], &lba) != 0)
    {
        (void)uart_log_write("error: invalid lba\r\n", 19U);
        return FUNC_STATUS_ERROR;
    }
    if (!uart_cli_lba_range_ok(lba, 1U))
    {
        (void)uart_log_write("error: lba range exceeds lba_max\r\n", 34U);
        return FUNC_STATUS_ERROR;
    }

    g_cli_pending.busy    = 1U;
    g_cli_pending.compare = 0U;
    g_cli_pending.lba     = lba;
    g_cli_pending.nblocks = 1U;

    memset(&g_cli_pending.req, 0, sizeof(g_cli_pending.req));
    g_cli_pending.req.opcode      = SSD_REQUEST_TRIM;
    g_cli_pending.req.start_lba   = lba;
    g_cli_pending.req.block_count = 1U;
    g_cli_pending.req.buffer      = (void *)g_wr_buf;

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)uart_log_write("tr submit ok\r\n", 14U);
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_fl(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (g_cli_pending.busy != 0U)
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }

    g_cli_pending.busy    = 1U;
    g_cli_pending.compare = 0U;
    g_cli_pending.lba     = 0U;
    g_cli_pending.nblocks = 1U;

    memset(&g_cli_pending.req, 0, sizeof(g_cli_pending.req));
    g_cli_pending.req.opcode      = SSD_REQUEST_FLUSH;
    g_cli_pending.req.start_lba   = 0U;
    g_cli_pending.req.block_count = 1U;
    g_cli_pending.req.buffer      = (void *)g_wr_buf;

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)uart_log_write("fl submit ok\r\n", 14U);
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_info(int argc, char *argv[])
{
    static const char *const state_names[] = {"RESET", "INITIALIZING", "READY", "FAILED"};
    uint8_t                  ch;
    uint8_t                  ce;
    uint8_t                  lun;

    (void)argc;
    (void)argv;

    if (g_ctrl.state <= SSD_CONTROLLER_FAILED)
    {
        (void)LOGI("controller state=%s lba_max=%lu\r\n", state_names[g_ctrl.state], (unsigned long)FTL_LBA_MAX);
    }

    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                const ftl_context_t *ftl = g_ctrl.ftl[ch][ce][lun];

                if (ftl == NULL)
                {
                    continue;
                }
                (void)LOGI("ftl[%u][%u][%u] free=%u op=%u spare=%u bad=%u closed=%u thrash=%u miss=%u%%\r\n",
                           (unsigned)ch,
                           (unsigned)ce,
                           (unsigned)lun,
                           (unsigned)ftl->alloc.free_count,
                           (unsigned)ftl->alloc.op_count,
                           (unsigned)ftl->alloc.spare_count,
                           (unsigned)ftl->alloc.bad_count,
                           (unsigned)ftl->alloc.closed_sb_count,
                           (unsigned)ftl->cache.thrashing,
                           (unsigned)ftl->cache.miss_pct);
            }
        }
    }
    return FUNC_STATUS_SUCCESS;
}

/*
 * bbt [--erase] — 逐块探测并打印坏块图。
 *
 * 默认**只读**：读每块 Page 0 的 spare 首字节（列 2048），非 0xFF ⇒ 出厂坏块。
 * 不写介质，可反复调用。
 *
 * ⚠ `--erase` 才是**破坏性**的：每个实例的每个块都会被擦除（含元数据区）⇒ 抹掉
 * FTL 元数据与用户数据。只在专门的坏块体检时用。
 */
static int uart_cli_bbt(int argc, char *argv[])
{
    uint8_t do_erase = 0U;
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;

    /* 选项**严格**校验：不认识就报错返回，绝不静默降级为只读。
     * 静默忽略曾两次把 `-erase` / `--rease`（拼错）当成合法擦除执行完、实际一条
     * 擦除命令都没发，看图的人只会以为"擦了但没生效"。 */
    if (argc >= 2)
    {
        if (strcmp(argv[1], "--erase") != 0)
        {
            LOGW("bbt: unknown option '%s' (only --erase)\r\n", argv[1]);
            return FUNC_STATUS_ERROR;
        }
        do_erase = 1U;
        LOGW("bbt --erase: erasing EVERY block of every instance (incl. metadata). Do not reset until done.\r\n");
    }

    for (ch = 0U; ch < CH_NUM; ch++)
    {
        for (ce = 0U; ce < CE_NUM; ce++)
        {
            for (lun = 0U; lun < LUN_NUM; lun++)
            {
                ftl_context_t *ftl = g_ctrl.ftl[ch][ce][lun];

                if (ftl == NULL)
                {
                    continue;
                }
                /* 擦除前先关掉本实例的后台写：擦除要跑十几秒，且擦完介质已空而 RAM 状态
                 * 陈旧（CP_TIME_INTERVAL_MS=10s 的定时 checkpoint 会把旧状态写回去，
                 * 用户就永远看不到"擦干净"）。置位后 ftl_service 跳过 Phase 3，无清除路径。 */
                if (do_erase != 0U)
                {
                    ftl->bg_suspend = 1U;
                }
                bbt_print_bad_map(&ftl->alloc, do_erase);
            }
        }
    }

    if (do_erase != 0U)
    {
        LOGW("bbt --erase: done. POWER-CYCLE NOW — the running FTL rewrites metadata if left idle.\r\n");
    }

    return FUNC_STATUS_SUCCESS;
}

/* rd 完成：compare 模式与 g_wr_buf pattern 比对，报 PASS / 首个不一致偏移 */
static void uart_cli_rd_report(void)
{
    uint32_t total_bytes = g_cli_pending.nblocks * FTL_IU_BYTES; /* n 计扇区（512B），非页 */
    uint32_t offset;

    if (g_cli_pending.compare == 0U)
    {
        /* hex dump 前 64B 防刷屏 */
        (void)LOGI("rd ok n=%lu, first 64 bytes:\r\n", (unsigned long)g_cli_pending.nblocks);
        for (offset = 0U; (offset < 64U) && (offset < total_bytes); offset += 16U)
        {
            (void)LOGI("%08lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                       (unsigned long)offset,
                       (unsigned)g_wr_buf[offset],
                       (unsigned)g_wr_buf[offset + 1U],
                       (unsigned)g_wr_buf[offset + 2U],
                       (unsigned)g_wr_buf[offset + 3U],
                       (unsigned)g_wr_buf[offset + 4U],
                       (unsigned)g_wr_buf[offset + 5U],
                       (unsigned)g_wr_buf[offset + 6U],
                       (unsigned)g_wr_buf[offset + 7U],
                       (unsigned)g_wr_buf[offset + 8U],
                       (unsigned)g_wr_buf[offset + 9U],
                       (unsigned)g_wr_buf[offset + 10U],
                       (unsigned)g_wr_buf[offset + 11U],
                       (unsigned)g_wr_buf[offset + 12U],
                       (unsigned)g_wr_buf[offset + 13U],
                       (unsigned)g_wr_buf[offset + 14U],
                       (unsigned)g_wr_buf[offset + 15U]);
        }
        return;
    }

    /* compare：按 4B 粒度与期望 pattern 比对（plane0=0x55555555 / plane1=0xAAAAAAAA） */
    for (offset = 0U; offset + sizeof(uint32_t) <= total_bytes; offset += sizeof(uint32_t))
    {
        uint32_t expect = uart_cli_expect_word(offset);
        uint32_t actual;

        memcpy(&actual, &g_wr_buf[offset], sizeof(actual));
        if (actual != expect)
        {
            (void)LOGE("rd FAIL at byte %lu: got %08lx expect %08lx\r\n", (unsigned long)offset, (unsigned long)actual, (unsigned long)expect);
            return;
        }
    }
    (void)uart_log_write("rd PASS\r\n", 8U);
}

/* cli_service_poll 每轮末尾调：检查未决 CLI 请求是否被主循环 ftl_service 完成 */
static void uart_cli_pending_poll(void)
{
    if (g_cli_pending.busy == 0U)
    {
        return;
    }
    if (ssd_controller_is_complete(&g_cli_pending.req) == 0U)
    {
        return;
    }

    switch (g_cli_pending.req.opcode)
    {
        case SSD_REQUEST_WRITE:
            (void)LOGI("wr done lba=%lu n=%lu status=%d\r\n", (unsigned long)g_cli_pending.lba, (unsigned long)g_cli_pending.nblocks, (int)g_cli_pending.req.completion_status);
            break;
        case SSD_REQUEST_READ:
            uart_cli_rd_report();
            break;
        case SSD_REQUEST_TRIM:
            (void)LOGI("tr done lba=%lu status=%d\r\n", (unsigned long)g_cli_pending.lba, (int)g_cli_pending.req.completion_status);
            break;
        case SSD_REQUEST_FLUSH:
            (void)LOGI("fl done status=%d\r\n", (int)g_cli_pending.req.completion_status);
            break;
        default:
            break;
    }
    g_cli_pending.busy = 0U;
}

/*
 * 第一版只注册只读诊断和日志等级控制。危险操作应按产品生命周期、认证状态和权限
 * 单独注册，不能把 raw NAND 或任意内存访问默认暴露在量产 UART 上。
 */
static const uart_cli_command_t g_uart_cli_commands[] = {
    {   "help",                                                                                                           "help",                                            "show command list", 1U,  1U,    uart_cli_help},
    { "status",                                                                                                         "status",                           "show UART error and queue counters", 1U,  1U,  uart_cli_status},
    {"version",                                                                                                        "version",                                "show firmware console version", 1U,  1U, uart_cli_version},
    {    "log",                                                                              "log level <error|warn|info|debug>",                                        "set runtime log level", 3U,  3U,     uart_cli_log},
 /* UART CLI 驱动 FTL（C 档，D10）：wr/rd/tr/fl/info 模拟 NVMe host IO */
    {     "wr",                                                                                                   "wr <lba> <n>",                            "write n blocks with fixed pattern", 3U,  3U,      uart_cli_wr},
    {     "rd",                                                                                               "rd <lba> <n> [c]",                            "read n blocks (c=compare pattern)", 3U,  4U,      uart_cli_rd},
    {     "tr",                                                                                                       "tr <lba>",                                                 "trim one lba", 2U,  2U,      uart_cli_tr},
    {     "fl",                                                                                                             "fl",                            "flush dirty L2P + journal to NAND", 1U,  1U,      uart_cli_fl},
    {   "info",                                                                                                           "info",                             "show controller + FTL statistics", 1U,  1U,    uart_cli_info},
    {    "bbt",                                                                                                  "bbt [--erase]", "print factory bad block map (--erase: also erase each block)", 1U,  2U,     uart_cli_bbt},
#if defined(FC_NAND_TEST_ENABLE)
  /* TODO: Level 3 完成后将 uart_cli_nand 替换为 ut_run_all */
  /* { "nand", ... }, */
    {   "test", "test [--seed N] [--iterations N] [--single] [--list] [--list-params] [--log-visible info|debug] [test_name...]",                                    "run munit unit test suite", 1U, 10U,    uart_cli_test},
#endif
};

static int uart_cli_help(int argc, char *argv[])
{
    uint32_t index;

    (void)argc;
    (void)argv;
    for (index = 0U; index < UART_ARRAY_SIZE(g_uart_cli_commands); index++)
    {
        const uart_cli_command_t *command = &g_uart_cli_commands[index];

        (void)LOGI("%-38s %s\r\n", command->usage, command->description);
    }
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_status(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    uart_print_status();
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_version(int argc, char *argv[])
{
    static const char version[] = "soft_ssd debug console\r\n";

    (void)argc;
    (void)argv;
    (void)uart_log_write(version, sizeof(version) - 1U);
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_log(int argc, char *argv[])
{
    static const char updated[] = "log level updated\r\n";

    (void)argc;
    if (strcmp(argv[1], "level") != 0)
    {
        return -1;
    }

    if (strcmp(argv[2], "error") == 0)
    {
        log_set_level(LOG_ERROR);
    }
    else if (strcmp(argv[2], "warn") == 0)
    {
        log_set_level(LOG_WARNING);
    }
    else if (strcmp(argv[2], "info") == 0)
    {
        log_set_level(LOG_INFO);
    }
    else if (strcmp(argv[2], "debug") == 0)
    {
        log_set_level(LOG_DEBUG);
    }
    else
    {
        return -1;
    }

    (void)uart_log_write(updated, sizeof(updated) - 1U);
    return FUNC_STATUS_SUCCESS;
}

#if defined(FC_NAND_TEST_ENABLE)
/* TODO: uart_cli_nand 和 uart_cli_parse_u32 待迁移用 munit test 替换 */
#    if 0
static int uart_cli_parse_u32(const char *text, uint32_t *value)
{
    ...
}
#    endif
#endif

#if defined(FC_NAND_TEST_ENABLE)
static int uart_cli_test(int argc, char *argv[])
{
    return ut_run_all(argc, argv);
}
#endif

/*
 * 仅支持空格和 Tab 分隔，不支持引号、转义或二进制参数。该约束让调试 CLI 保持小且可预测；
 * 产测或固件下载应使用独立的二进制帧协议，而不是继续扩展这个文本解析器。
 */
static int uart_cli_tokenize(char *line, char *argv[], uint32_t max_arguments)
{
    uint32_t argc   = 0U;
    char    *cursor = line;

    while (*cursor != '\0')
    {
        while ((*cursor == ' ') || (*cursor == '\t'))
        {
            cursor++;
        }
        if (*cursor == '\0')
        {
            break;
        }
        if (argc == max_arguments)
        {
            return -1;
        }

        argv[argc++] = cursor;
        while ((*cursor != '\0') && (*cursor != ' ') && (*cursor != '\t'))
        {
            cursor++;
        }
        if (*cursor != '\0')
        {
            *cursor++ = '\0';
        }
    }

    return (int)argc;
}

static void uart_execute_command(char *line)
{
    static const char unknown[] = "error: unknown command\r\n";
    char             *argv[UART_CLI_MAX_ARGUMENTS];
    int               argc;
    uint32_t          index;

    argc = uart_cli_tokenize(line, argv, UART_CLI_MAX_ARGUMENTS);
    if (argc == 0)
    {
        return;
    }
    if (argc < 0)
    {
        (void)uart_log_write("error: too many arguments\r\n", 27U);
        return;
    }

    for (index = 0U; index < UART_ARRAY_SIZE(g_uart_cli_commands); index++)
    {
        const uart_cli_command_t *command = &g_uart_cli_commands[index];

        if (strcmp(argv[0], command->name) != 0)
        {
            continue;
        }

        if (((uint32_t)argc < command->min_argc) || ((uint32_t)argc > command->max_argc) ||
            (command->handler(argc, argv) != 0))
        {
            (void)LOGE("usage: %s\r\n", command->usage);
        }
        return;
    }

    (void)uart_log_write(unknown, sizeof(unknown) - 1U);
}

void cli_init(void)
{
    /* g_wr_buf 落 NOLOAD SRAM2 段，上电不清零 → 主动预填 pattern */
    uart_cli_pattern_init();
}

void cli_service_poll(void)
{
    uint8_t byte;

    while (uart_rx_read(&byte) != 0U)
    {
        if ((byte == '\r') || (byte == '\n'))
        {
            /* CRLF 被视为一次提交；紧跟 CR 的 LF 不产生一个空命令。 */
            if ((byte == '\n') && (g_cli_length == 0U) && !g_cli_discarding)
            {
                continue;
            }

            (void)uart_log_write("\r\n", 2U);
            if (g_cli_discarding)
            {
                static const char too_long[] = "error: command too long\r\n";
                (void)uart_log_write(too_long, sizeof(too_long) - 1U);
            }
            else
            {
                g_cli_line[g_cli_length] = '\0';
                uart_execute_command(g_cli_line);
            }
            g_cli_length     = 0U;
            g_cli_discarding = 0U;
            uart_write_prompt();
        }
        else if ((byte == '\b') || (byte == 0x7FU))
        {
            if (g_cli_length != 0U)
            {
                g_cli_length--;
                (void)uart_log_write("\b \b", 3U);
            }
        }
        else if ((byte >= 0x20U) && (byte <= 0x7EU))
        {
            if (g_cli_length < (USART_CLI_LINE_CAPACITY - 1U))
            {
                g_cli_line[g_cli_length++] = (char)byte;
                (void)uart_log_write((const char *)&byte, 1U);
            }
            else
            {
                /*
                 * 后续字节一直丢到换行为止。不能简单截断后立即执行，否则一个过长命令的
                 * 前缀可能恰好构成合法且不应执行的命令。
                 */
                g_cli_discarding = 1U;
            }
        }
    }

    /* 两段式完成轮询：wr/rd/tr/fl 已提交到 controller，主循环 ftl_service 完成后在此报告 */
    uart_cli_pending_poll();
}
