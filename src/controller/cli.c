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

#include "cli.h"

#include "bbt.h"
#include "common.h"
#include "crash.h"
#include "ftl.h"
#include "ftl_fc_interface.h" /* ftl_sync_read_page / FTL_PAGE_CFG_DATA_ANY（rdpaa 诊断）*/
#include "gc.h"               /* gc_read_page_with_lba（rdpaa 打印页内 META）*/
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h"               /* md_pool_print（pool 诊断） */
#include "power_failure_recovery.h" /* pfr_media_format（boot 失败后的恢复格式化） */
#include "ssd_request.h"
#include "stack_guard.h"
#include "usart.h"
#include "usb_msc.h"
#include "wbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FC_NAND_TEST_ENABLE)
#    include "ut_main.h"
#endif

/* 平台 syscalls.c：堆与栈共用 DTCM，`stack` 需要单独报告堆用量。 */
extern DWORD syscalls_heap_used(void);

#define USART_CLI_LINE_CAPACITY 128U
/* 单条命令行最多几个 token。`test` 子命令要能容下
 * `--seed/--iterations/--single/--list/--list-params/--log-visible/--log-project`
 * 加多组 `--param name value` 再过套件名，10 不够（两组 --param 就顶满）。 */
#define UART_CLI_MAX_ARGUMENTS 24U
#define UART_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/*
 * QWORD（host LBA）一律用 %llu；DWORD 用 %lu。完整 newlib 支持标准长度修饰符。
 * ssd_controller_t.host_lba_max 仍是 DWORD（32-bit LBA × 512B = 2 TiB 上限），
 * 故仍用 _Static_assert 钉住 FTL_LBA_MAX 装得进 32 位。
 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= 0xFFFFFFFFUL, "FTL_LBA_MAX 超出 32 位，host_lba_max 装不下");

static char g_cli_line[USART_CLI_LINE_CAPACITY];
static WORD g_cli_length;
static BYTE g_cli_discarding;

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
    BYTE               min_argc;
    BYTE               max_argc;
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
static int uart_cli_wp(int argc, char *argv[]); /* 页打包写缓冲合并诊断（§七 方案 A） */
static int uart_cli_info(int argc, char *argv[]);
static int uart_cli_map(int argc, char *argv[]);   /* L2P 映射 + 目标块 BST 条目（只读诊断） */
static int uart_cli_addr(int argc, char *argv[]);  /* PBA/PAA/gblock 地址字段转换（只读诊断） */
static int uart_cli_rdpaa(int argc, char *argv[]); /* 按 PAA 读一个 NAND 页（诊断） */
static int uart_cli_crash(int argc, char *argv[]); /* .noinit 崩溃记录查看/清除 */
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
    (void)LOGI("uart: rx_ovf=%lu tx_drop=%lu tx_ctx=%lu flush_to=%lu dma_err=%lu tx_fault=%lu ore=%lu fe=%lu ne=%lu pe=%lu\r\n", stats.rx_overflow, stats.tx_dropped, stats.tx_context_errors, stats.tx_flush_timeouts, stats.tx_dma_errors, stats.tx_transport_errors, stats.overrun_errors, stats.framing_errors, stats.noise_errors, stats.parity_errors);
}

/*
 * UART CLI 驱动 FTL（C 档，设计文档 §13.4 D10：Host 命令来源 = UART CLI）。
 *
 * 固定 pattern 写 buffer：16KB，落 **AXI_RAM .bss**（启动清零；cli_init 仍主动预填
 * 0x55/0xAA）。阶段 C 起 SRAM1+SRAM2 合并给 FTL tables（2×123,896B），
 * 16KB CLI 缓冲放不进 SRAM12 的 14KB 余量，故迁回 AXI（见 main.c 内存断言）。
 *
 * ⚠ CW 级映射（IU = 1 CW = 512B）：`wr lba n` / `rd lba n` 的 n 单位是**扇区**
 *   （每个 512B），缓冲步进 = FTL_IU_BYTES。故 **n 上限 = 16KB / 512B = 32**
 *   （旧页级模型下 n 计 2048B 页，上限 8）。超出由 controller 的 buffer_size
 *   校验拒绝（写爆 g_wr_buf 是真实隐患）。
 */
static BYTE g_wr_buf[FTL_READ_USER_SIZE * CW_NUM * PLANE_NUM]; /* 16KB → AXI .bss */

/* n（扇区数）上限：g_wr_buf 容量 ÷ 映射单元字节数 */
#define UART_CLI_MAX_SECTORS ((sizeof(g_wr_buf) / FTL_IU_BYTES))

/* ── wp 诊断缓冲（§七 方案 A 合并可观测性）──
 *
 * 一个 pair 窗口 = FTL_LBA_PER_PAIR（= 8）个 CW = 8 扇区。wp 一次提交 k 个组、
 * 每组 n 个扇区（k*n ≤ 8），各组的源数据取自本缓冲的**不相交切片**（组 i 用
 * 扇区段 [i*n, i*n+n)）。切片内容按组号填不同字节，便于 rd 回读时肉眼分辨
 * 「哪一段是哪个组写的」——若打包把 CW 错位，回读会看到串段的字节。 */
static BYTE g_wp_buf[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 × 512B = 4KB */
/* 每组一个完成标志/状态（wp 不等待，仅作在飞期间的写回目标，避免悬垂指针）。 */
static volatile BYTE         g_wp_done[FTL_LBA_PER_PAIR];
static volatile ssd_status_e g_wp_status[FTL_LBA_PER_PAIR];

/* wr/rd 两段式：handler 只提交请求到 controller（异步），结果由
 * cli_service_poll 每轮末尾检查 ssd_controller_is_complete 打印（D11
 * 非阻塞 pending 表语义；主循环的 ftl_service 完成请求）。 */
typedef struct
{
    BYTE          busy;    /* 1=有未决 CLI 请求 */
    ssd_request_t req;     /* 未决请求（controller 拆分子请求跟踪其 complete_flag） */
    BYTE          compare; /* rd: 1=compare 模式，0=hex dump */
    QWORD         lba;     /* 本次请求起始 LBA（结果打印用） */
    DWORD         nblocks; /* 本次请求块数 */
} uart_cli_pending_t;

static uart_cli_pending_t g_cli_pending;

bool cli_host_busy(void)
{
    return g_cli_pending.busy != 0U;
}

/* 预填 g_wr_buf pattern（.sram2_data NOLOAD，必须主动初始化） */
static void uart_cli_pattern_init(void)
{
    DWORD  i;
    DWORD  words_per_half = ((FTL_READ_USER_SIZE * CW_NUM) / sizeof(DWORD)); /* 8KB/4 = 2048 */
    DWORD *lo             = (DWORD *)(void *)g_wr_buf;
    DWORD *hi             = (DWORD *)(void *)(g_wr_buf + FTL_READ_USER_SIZE * CW_NUM);

    for (i = 0U; i < words_per_half; i++)
    {
        lo[i] = 0x55555555U;
        hi[i] = 0xAAAAAAAAU;
    }
}

/* 从 g_wr_buf 对应区取期望 pattern 值（compare 模式用；前 8KB/后 8KB 两段） */
static DWORD uart_cli_expect_word(DWORD byte_offset)
{
    return (byte_offset < (FTL_READ_USER_SIZE * CW_NUM)) ? 0x55555555U : 0xAAAAAAAAU;
}

/* CLI 诊断命令（map / pair 窗口）按 LBA 取所属 FTL 实例。
 * 复用 l2p_cache.h 的分片公式；8-LBA pair 窗口不跨 region，故按窗口基址取即可。 */
static ftl_ctx_t *uart_cli_ftl_for_lba(QWORD lba)
{
    return g_ctrl.ftl[lba_2_die(lba)];
}

/* 解析十进制/十六进制数。成功且全部消费返回 true；失败/残留返回 false。 */
static bool uart_cli_parse_uint(const char *text, QWORD *out)
{
    char *end = NULL;
    QWORD value;

    if ((text == NULL) || (text[0] == '\0'))
    {
        return false;
    }
    value = strtoull(text, &end, 0);
    if ((end == NULL) || (*end != '\0'))
    {
        return false; /* 有非法字符残留 */
    }
    if (out != NULL)
    {
        *out = value;
    }
    return true;
}

/* 地址字段是 32-bit：先按 QWORD 解析，再显式拒绝超出 DWORD 的输入。 */
static bool uart_cli_parse_dword(const char *text, DWORD *out)
{
    QWORD value;

    if (!uart_cli_parse_uint(text, &value) || (value > UINT32_MAX))
    {
        return false;
    }
    *out = (DWORD)value;
    return true;
}

static int uart_cli_wr(int argc, char *argv[])
{
    QWORD lba;
    QWORD n_parsed;
    DWORD n;
    char *end = NULL;

    (void)argc;

    if ((g_cli_pending.busy != 0U) || usb_msc_host_busy())
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }

    lba = strtoull(argv[1], &end, 0);
    if ((end == NULL) || (*end != '\0') || (!uart_cli_parse_uint(argv[2], &n_parsed)) || (n_parsed == 0U) || (n_parsed > 0xFFFFFFFFU))
    {
        (void)uart_log_write("error: invalid lba/n\r\n", 22U);
        return FUNC_STATUS_ERROR;
    }
    n = n_parsed;
    if (n > UART_CLI_MAX_SECTORS)
    {
        (void)uart_log_write("error: n exceeds g_wr_buf (max 32 sectors)\r\n", 44U);
        return FUNC_STATUS_ERROR;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, n))
    {
        (void)uart_log_write("error: lba range exceeds lba_max\r\n", 34U);
        return FUNC_STATUS_ERROR;
    }

    /* ⚠ `rd` 把读到的数据写进 g_wr_buf（同一块缓冲），会覆盖 pattern。
     * 若只在 init 里预填一次，`wr` 紧跟在 `rd` 之后时会把**读回的旧数据**当写源，
     * 表现为「写成功但内容不对」。每次 wr 重新预填，保证命令语义是「写固定 pattern」。 */
    uart_cli_pattern_init();

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

    (void)LOGI("wr submit ok lba=%llu n=%lu, waiting...\r\n", lba, n);
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_rd(int argc, char *argv[])
{
    QWORD lba;
    QWORD n_parsed;
    DWORD n;
    char *end = NULL;

    if ((g_cli_pending.busy != 0U) || usb_msc_host_busy())
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }

    lba = strtoull(argv[1], &end, 0);
    if ((end == NULL) || (*end != '\0') || (!uart_cli_parse_uint(argv[2], &n_parsed)) || (n_parsed == 0U) || (n_parsed > 0xFFFFFFFFU))
    {
        (void)uart_log_write("error: invalid lba/n\r\n", 22U);
        return FUNC_STATUS_ERROR;
    }
    n = n_parsed;
    if (n > UART_CLI_MAX_SECTORS)
    {
        (void)uart_log_write("error: n exceeds g_wr_buf (max 32 sectors)\r\n", 44U);
        return FUNC_STATUS_ERROR;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, n))
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
    /* 读数据覆盖 pattern；`wr` 每次会重新预填（见 uart_cli_wr），所以 rd→wr 相邻也安全。 */
    g_cli_pending.req.buffer      = (void *)g_wr_buf;
    g_cli_pending.req.buffer_size = sizeof(g_wr_buf); /* 真实缓冲容量（非 n×2KB），见 uart_cli_wr */

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)LOGI("rd submit ok lba=%llu n=%lu%s, waiting...\r\n", lba, n, g_cli_pending.compare ? " c" : "");
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_tr(int argc, char *argv[])
{
    QWORD lba;

    (void)argc;

    if ((g_cli_pending.busy != 0U) || usb_msc_host_busy())
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }
    if (!uart_cli_parse_uint(argv[1], &lba))
    {
        (void)uart_log_write("error: invalid lba\r\n", 19U);
        return FUNC_STATUS_ERROR;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, 1U))
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

/*
 * wp <lba> <n> <k> — 页打包写缓冲合并诊断
 *
 * 一次提交 k 个「同一 pair 窗口内、各 n 个连续 LBA」的小写，**直接调 ftl_submit_write
 * 绕过 g_cli_pending.busy 闸门**——该闸门让连续 `wr` 串行，而串行下 idle flush 立即触发，
 * 两个连续 wr 永不合并。k 个组同时在飞 ⇒ 窗口填满时由写缓冲合并成 **1 次编程**。
 *
 * 约束：lba 必须 8 对齐（= 窗口基址）；n ≥ 1；k ≥ 2；k*n ≤ FTL_LBA_PER_PAIR（= 8）。
 * 各组源数据取自 g_wp_buf 的不相交切片，组 i 填字节 0xA0+i（错位时回读会串段）。
 *
 * 判据：`info` 的 total_writes 相对本次只 +1（对比逐组编程的 +k）——**主判据**；
 *       wb_handle(FLUSH) 的 `[FTL] wbuf flush` 行是辅助证据，且是 LOGD ⇒ **仅 debug 档可见**。
 *       随后 `rd <lba> <k*n>` 回读，第 (i*n+j) 段字节应为 0xA0+i。
 */
static int uart_cli_wp(int argc, char *argv[])
{
    QWORD      lba;
    QWORD      n_parsed;
    QWORD      k_parsed;
    DWORD      n;
    DWORD      k;
    DWORD      i;
    DWORD      j;
    ftl_ctx_t *ftl;

    (void)argc;

    /* wp 只绕过 CLI 自己的 busy 闸门（要在飞多组），仍须让路给 USB host 请求。 */
    if (usb_msc_host_busy())
    {
        (void)uart_log_write("error: busy, USB host request\r\n", 31U);
        return FUNC_STATUS_ERROR;
    }

    if ((!uart_cli_parse_uint(argv[1], &lba)) || (!uart_cli_parse_uint(argv[2], &n_parsed)) || (!uart_cli_parse_uint(argv[3], &k_parsed)) || (n_parsed == 0U) || (k_parsed < 2U) || (n_parsed > 0xFFFFFFFFU) || (k_parsed > 0xFFFFFFFFU))
    {
        (void)uart_log_write("error: invalid lba/n/k (need n>=1, k>=2)\r\n", 41U);
        return FUNC_STATUS_ERROR;
    }
    n = n_parsed;
    k = k_parsed;
    if ((QWORD)n * (QWORD)k > FTL_LBA_PER_PAIR)
    {
        (void)uart_log_write("error: n*k must be <= 8 (one pair window)\r\n", 42U);
        return FUNC_STATUS_ERROR;
    }
    if ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U)
    {
        (void)uart_log_write("error: lba must be 8-aligned (pair window base)\r\n", 47U);
        return FUNC_STATUS_ERROR;
    }
    const DWORD sectors = n * k;

    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, sectors))
    {
        (void)uart_log_write("error: lba range exceeds lba_max\r\n", 34U);
        return FUNC_STATUS_ERROR;
    }

    /* 按窗口基址路由到所属实例（与 controller/FTL 的分片规则一致）。 */
    ftl = uart_cli_ftl_for_lba(lba);
    if (ftl == NULL)
    {
        (void)uart_log_write("error: ftl instance not ready\r\n", 31U);
        return FUNC_STATUS_ERROR;
    }

    /* 组 i 的 n 个扇区填字节 0xA0+i（各切片不相交） */
    for (i = 0U; i < k; i++)
    {
        BYTE *slice = &g_wp_buf[(i * n) * FTL_IU_BYTES];

        for (j = 0U; j < (n * FTL_IU_BYTES); j++)
        {
            slice[j] = (0xA0U + i);
        }
        g_wp_done[i]   = 0U;
        g_wp_status[i] = SSD_STATUS_OK;
    }

    /* 逐组提交（不查 busy）：k 个组同时在飞 ⇒ 窗口填满时合并成 1 次编程。
     * 各组 complete_flag/status_out 指向静态数组，完成前始终有效。 */
    for (i = 0U; i < k; i++)
    {
        ftl_io_req_t req;

        memset(&req, 0, sizeof(req));
        req.lba           = lba + ((QWORD)i * (QWORD)n);
        req.buffer        = &g_wp_buf[(i * n) * FTL_IU_BYTES];
        req.count         = n;
        req.complete_flag = &g_wp_done[i];
        req.status_out    = &g_wp_status[i];

        if (ftl_submit_write(ftl, &req) != FTL_OK)
        {
            (void)LOGE("wp: group %lu submit failed\r\n", i);
            return FUNC_STATUS_ERROR;
        }
    }

    (void)LOGI("wp submit ok lba=%llu n=%lu k=%lu (expect 1 program on flush)\r\n", lba, n, k);
    return FUNC_STATUS_SUCCESS;
}

static int uart_cli_fl(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if ((g_cli_pending.busy != 0U) || usb_msc_host_busy())
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
    BYTE                     ch;
    BYTE                     ce;
    BYTE                     lun;

    (void)argc;
    (void)argv;

    if (g_ctrl.state <= SSD_CONTROLLER_FAILED)
    {
        (void)LOGI("controller state=%s lba_max=%lu\r\n", state_names[g_ctrl.state], ssd_controller_host_lba_max(&g_ctrl));
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ch  = die_2_ch(die);
        ce  = die_2_ce(die);
        lun = die_2_lun(die);

        const ftl_ctx_t *ftl = g_ctrl.ftl[die];

        if (ftl == NULL)
        {
            continue;
        }
        (void)LOGI("ftl[%hhu][%hhu][%hhu] mode=%hhu free=%hu op=%hu/%hu spare=%hu/%hu bad=%hu closed=%hu live=%hu thrash=%d miss=%hhu%% cp=%lu cpa=%d jrn=%lu wr=%lu\r\n", ch, ce, lun, nand_get_timing_mode(), ftl->alloc.free_count, alc_op_get_count(&ftl->alloc, 0U), alc_op_get_count(&ftl->alloc, 1U), alc_spare_get_count(&ftl->alloc, 0U), alc_spare_get_count(&ftl->alloc, 1U), ftl->alloc.bad_count, ftl->alloc.closed_sb_count, md_live_block_count_get(ftl->metadata_dir), ftl->cache->thrash.thrashing, ftl->cache->thrash.miss_pct, ftl->cp_handle->cp_state.sequence, ftl->cp_handle->active, ftl->journal->handle.state.sequence_counter, ftl->stats.total_writes);

        /* 计数类诊断（不逐事件打日志，靠这里/`[STAT]` 查）。 */
        (void)LOGI("diag[%hhu][%hhu][%hhu] dng=%lu rrab=%lu mig=%u/%u rec=%lu mv=%lu nop=%lu nov=%lu erased=%hu streak=%hhu gcm=%d ng=%lu fpmin=%hu\r\n", ch, ce, lun, ftl->stats.dangling_total, ftl->read_recovery_handle.already_bad_reads, ((ftl->mig_handle->job.active != 0U) ? 1U : 0U), ((ftl->mig_handle->state.failure.paused != 0U) ? 1U : 0U), ftl->mig_handle->state.counters.reclaimed, ftl->mig_handle->state.counters.moved, ftl->mig_handle->state.counters.noop, ftl->mig_handle->state.counters.novictim, ftl->mig_handle->job.erased, ftl->mig_handle->state.failure.read_fail_streak, ftl->mig_handle->state.benefit.last_mode, ftl->mig_handle->state.counters.nogain, alc_free_min_plane_count(&ftl->alloc));
        /* 顺序读预取 / cache read 观测（只读计数，不逐事件打日志）。
         * 拆两行：单行最坏值会超过 log_vprintf 的 192 B 缓冲。 */
        (void)LOGI("l2p[%hhu][%hhu][%hhu] seq=%lu/%lu pf=%lu/%lu/%lu/%lu/%lu cr=%lu/%lu/%lu/%lu/%lu\r\n", ch, ce, lun, ftl->cache_stream.prefetch_run, ftl->cache_stream.prefetch_run_max, ftl->cache->prefetch.calls, ftl->cache->prefetch.started, ftl->cache->prefetch.skipped, ftl->cache->prefetch.resident, ftl->cache->prefetch.hits, ftl->cache_stream.read_batches, ftl->cache_stream.read_pairs, ftl->cache_stream.read_starts, ftl->cache_stream.read_declines, ftl->cache_stream.read_fallbacks);
        (void)LOGI("l2p[%hhu][%hhu][%hhu] cw=%lu/%lu/%lu/%lu sw=%lu/%lu pc=%lu jf=%lu mb=%lu/%lu/%lu d4=%lu wd=%lu\r\n", ch, ce, lun, ftl->cache_stream.wb_batches, ftl->cache_stream.wb_pages, ftl->cache_stream.wb_declines, ftl->cache_stream.wb_aborts, ftl->cache_stream.wb_starts, ftl->cache_stream.wb_page_ends, ftl->cache_stream.wb_pad_closes, jrn_page_writes(ftl->journal), ftl->mig_handle->state.counters.batch_jobs, ftl->mig_handle->state.counters.batch_pages, ftl->mig_handle->state.counters.batch_aborts, ftl->cache_stream.seq_breaks, ftl->cache->stats.wal_defer_count);

        /* entry 级 CMT：读/写命中率 + dirty 写回压力。 */
        (void)LOGI("l2p[%hhu][%hhu][%hhu] cmt hit=%lu/%lu wr=%lu/%lu dirty=%lu wb=%lu ev=%lu/%lu defer=%lu\r\n", ch, ce, lun, ftl->cache->stats.cmt_hits, ftl->cache->stats.cmt_misses, ftl->cache->stats.cmt_wr_hits, ftl->cache->stats.cmt_wr_misses, ftl->cache->stats.cmt_dirty, ftl->cache->stats.cmt_writebacks, ftl->cache->stats.cmt_evict_clean, ftl->cache->stats.cmt_evict_dirty, ftl->cache->stats.cmt_wb_defer);
        (void)LOGI("l2p[%hhu][%hhu][%hhu] line hint=%lu/%lu\r\n", ch, ce, lun, ftl->cache->stats.hint_hits, ftl->cache->stats.hint_misses);

        /* 元数据整理 job 的现场（心跳默认档不打，靠这里观测）。
         * 只在有 job 在跑 / 有待回收块时多打一行，空闲时不刷屏。 */
        const md_dir_cache_t *md  = ftl->metadata_dir;
        const md_handle_t    *job = &md->handle;

        if ((job->phase != MD_JOB_IDLE) || (md->reclaim.count != 0U))
        {
            (void)LOGI("md[%hhu][%hhu][%hhu] phase=%d moved=%hu stall_armed=%d pending=%hu victim=%hu\r\n", ch, ce, lun, job->phase, job->moved, (int)job->submit_deadline.armed, md->reclaim.count, job->victim);
        }
    }

    usb_msc_io_stats_print(); /* RISK-NAND-04：WRITE(10) 分块直方图 */

    return FUNC_STATUS_SUCCESS;
}

/*
 * map <lba> — 只读诊断：打印该 LBA 的 L2P 映射与**目标块**的 BST 条目。
 *
 * 起因：`md_block_is_metadata` 缺 plane 掩码（v0.69 修）时，被日志块掩住的用户块
 * 进不了 Free Pool、也被 full metadata rebuild 跳过 ⇒ `valid_count` 恒 0，而 L2P 仍可能指向它
 * ⇒ 此后**每次**覆盖写该 LBA 都撞 `bst.h` 的 `b->valid_count > 0U` 断言。
 * 本命令把「L2P 指向谁」与「BST 怎么看那个块」摆在一起，用于在**不复现崩溃**的前提下
 * 判定现场是否已经悬空（末行 VERDICT）。
 *
 * ⚠ 判据：L2P 指向某块 ⇒ 至少该 CW 有效 ⇒ `valid_count ≥ 1` 是**不变量**；
 *   `valid_count == 0` 即悬空，与 v0.69 的 `[FTL] dangling` 守卫同一判据。
 *   `state` 用于一次归因：BAD=坏块本不可读 / FREE=会被复用**丢数据** /
 *   LOG=判据过宽 / NONE=从未认领。
 *
 * 按 LBA 路由到所属实例（与 rd/wr 经 controller 路由到的实例一致）。
 */
static int uart_cli_map(int argc, char *argv[])
{
    ftl_ctx_t         *ftl;
    const bst_block_t *b;
    DWORD              pba;
    QWORD              lba;
    WORD               g;

    (void)argc;

    if (!uart_cli_parse_uint(argv[1], &lba))
    {
        (void)uart_log_write("error: invalid lba\r\n", 19U);
        return FUNC_STATUS_ERROR;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, 1U))
    {
        (void)uart_log_write("error: lba range exceeds lba_max\r\n", 34U);
        return FUNC_STATUS_ERROR;
    }

    ftl = uart_cli_ftl_for_lba(lba);
    if (ftl == NULL)
    {
        (void)uart_log_write("error: ftl instance not ready\r\n", 31U);
        return FUNC_STATUS_ERROR;
    }

    /* ⚠ CLI 上下文（不在完成派发里）：region 未驻留时自旋泵等加载完成。
     * 自旋失败（超预算）时 pba 保持 INVALID，输出按"未映射"处理——CLI 只读不写，
     * 误报一次"未映射"无副作用。 */
    if (l2p_read_sync(ftl->cache, (DWORD)lba, &pba) != FTL_OK)
    {
        pba = NAND_PBA_INVALID;
    }
    if (pba == NAND_PBA_INVALID)
    {
        (void)LOGI("map lba=%llu -> INVALID (未映射)\r\n", lba);
        return FUNC_STATUS_SUCCESS;
    }

    g = ftl_pba_gblock(pba);
    b = &ftl->alloc.blocks[g];
    (void)LOGI("map lba=%llu pba=%08lx paa=%08lx ch=%hhu ce=%hhu lun=%hhu plane=%hhu blk=%hu pg=%hhu cw=%hhu g=%hu\r\n", lba, pba, pba_to_paa(pba), pba_to_ch(pba), pba_to_ce(pba), pba_to_lun(pba), pba_to_plane(pba), pba_to_block(pba), pba_to_page(pba), pba_to_cw(pba), g);
    (void)LOGI("    block g=%u state=%s valid=%u sb=%u ec=%lu\r\n", g, ftl_block_state_name(ftl_block_state(b)), b->valid_count, b->sb_id, b->erase_count);

    if (b->valid_count == 0U)
    {
        (void)LOGE("    VERDICT: 悬空 —— 覆盖写该 LBA 会撞 bst.h valid_count>0 断言\r\n");
    }
    else
    {
        (void)LOGI("    VERDICT: ok —— 覆盖写安全\r\n");
    }
    return FUNC_STATUS_SUCCESS;
}

/*
 * addr — PBA/PAA/gblock 的只读地址转换。
 *
 *   addr pba <pba>                  PBA → PAA + ch/ce/lun/plane/block/page/cw + g
 *   addr paa <paa> [ch ce]          PAA → PBA + 展开字段（默认单 chip ch=ce=0）
 *   addr g <g> [ch ce lun]          实例内全局块号 → plane/block + BST 摘要
 *
 * LBA → 上述字段的入口是 `map <lba>`；这里不再提供 `addr lba`，避免两套入口（C9）。
 * 所有字段展开都复用 PBA/PAA getter 与 ftl_gblock helper，不手写位运算。
 */
static int uart_cli_addr(int argc, char *argv[])
{
    DWORD value;

    if (argc < 3)
    {
        (void)LOGE("usage: addr pba <pba> | paa <paa> [ch ce] | g <g> [ch ce lun]\r\n");
        return FUNC_STATUS_ERROR;
    }

    if (strcmp(argv[1], "pba") == 0)
    {
        if ((argc != 3) || (!uart_cli_parse_dword(argv[2], &value)) || (!pba_is_valid(value)))
        {
            (void)LOGE("addr: invalid pba\r\n");
            return FUNC_STATUS_ERROR;
        }

        (void)LOGI("addr pba=0x%08lx paa=0x%08lx ch=%hhu ce=%hhu lun=%hhu g=%hu plane=%hhu blk=%hu pg=%hhu cw=%hhu\r\n", value, pba_to_paa(value), pba_to_ch(value), pba_to_ce(value), pba_to_lun(value), ftl_pba_gblock(value), pba_to_plane(value), pba_to_block(value), pba_to_page(value), pba_to_cw(value));
        return FUNC_STATUS_SUCCESS;
    }

    if (strcmp(argv[1], "paa") == 0)
    {
        DWORD ch = 0U;
        DWORD ce = 0U;

        if ((argc != 3) && (argc != 5))
        {
            (void)LOGE("usage: addr paa <paa> [ch ce]\r\n");
            return FUNC_STATUS_ERROR;
        }
        if ((!uart_cli_parse_dword(argv[2], &value)) || (paa_is_valid_paa(value) == 0U))
        {
            (void)LOGE("addr: invalid paa\r\n");
            return FUNC_STATUS_ERROR;
        }
        if (argc == 5)
        {
            if ((!uart_cli_parse_dword(argv[3], &ch)) || (!uart_cli_parse_dword(argv[4], &ce)) || (ch >= CH_NUM) || (ce >= CE_NUM))
            {
                (void)LOGE("addr: invalid ch/ce\r\n");
                return FUNC_STATUS_ERROR;
            }
        }

        const DWORD pba = paa_to_pba(ch, ce, value);

        (void)LOGI("addr paa=0x%08lx pba=0x%08lx ch=%hhu ce=%hhu lun=%hhu g=%hu plane=%hhu blk=%hu pg=%hhu cw=%hhu\r\n", value, pba, pba_to_ch(pba), pba_to_ce(pba), pba_to_lun(pba), ftl_pba_gblock(pba), pba_to_plane(pba), pba_to_block(pba), pba_to_page(pba), pba_to_cw(pba));
        return FUNC_STATUS_SUCCESS;
    }

    if (strcmp(argv[1], "g") == 0)
    {
        DWORD ch  = 0U;
        DWORD ce  = 0U;
        DWORD lun = 0U;

        if ((argc != 3) && (argc != 6))
        {
            (void)LOGE("usage: addr g <g> [ch ce lun]\r\n");
            return FUNC_STATUS_ERROR;
        }
        if ((!uart_cli_parse_dword(argv[2], &value)) || (value >= FTL_BLOCK_TOTAL))
        {
            (void)LOGE("addr: invalid gblock\r\n");
            return FUNC_STATUS_ERROR;
        }
        if (argc == 6)
        {
            if ((!uart_cli_parse_dword(argv[3], &ch)) || (!uart_cli_parse_dword(argv[4], &ce)) || (!uart_cli_parse_dword(argv[5], &lun)) || (ch >= CH_MAX) || (ce >= CE_MAX) || (lun >= LUN_MAX))
            {
                (void)LOGE("addr: invalid ch/ce/lun\r\n");
                return FUNC_STATUS_ERROR;
            }
        }

        const BYTE         die = chcelun_2_die((BYTE)ch, (BYTE)ce, (BYTE)lun);
        const ftl_ctx_t   *ftl = ((die == FTL_DIE_INVALID) || (die >= DIE_MAX)) ? NULL : g_ctrl.ftl[die];
        const bst_block_t *b;

        if (ftl == NULL)
        {
            (void)LOGE("addr: ftl instance not ready\r\n");
            return FUNC_STATUS_ERROR;
        }
        b = &ftl->alloc.blocks[value];
        (void)LOGI("addr g=%lu plane=%hhu blk=%hu state=%s valid=%hu sb=%hu ec=%lu\r\n", value, ftl_gblock_plane((WORD)value), ftl_gblock_block((WORD)value), ftl_block_state_name(ftl_block_state(b)), b->valid_count, b->sb_id, b->erase_count);
        return FUNC_STATUS_SUCCESS;
    }

    (void)LOGE("usage: addr pba <pba> | paa <paa> [ch ce] | g <g> [ch ce lun]\r\n");
    return FUNC_STATUS_ERROR;
}

/*
 * crash [clear] — 查看 / 清除 `.noinit` 崩溃记录。
 *
 * 记录放 `.noinit`（NOLOAD，启动不清零）⇒ **软复位后保留**，且当前没有任何路径会自动
 * 清除它 ⇒ 一次断言会被此后每次复位反复打印成 `[FAULT] previous assert: ...`，
 * 看上去像"还在崩"（实际只是同一条旧记录）。
 *
 * `crash clear` 给一个**显式归零点**：清完再复位，若记录重新出现，才是真的又崩了。
 * ⚠ 掉电（POR）本来就清 `.noinit` ⇒ 断电重启后看到的记录一定是新的、无需 clear。
 */
static int uart_cli_crash(int argc, char *argv[])
{
    const crash_record_t *rec;

    if (argc >= 2)
    {
        /* 选项**严格**校验（同 bbt：静默忽略拼错的子参数会让人以为"清过了"） */
        if (strcmp(argv[1], "clear") != 0)
        {
            LOGW("crash: unknown option '%s' (only clear)\r\n", argv[1]);
            return FUNC_STATUS_ERROR;
        }
        crash_record_clear();
        (void)uart_log_write("crash record cleared\r\n", 22U);
        return FUNC_STATUS_SUCCESS;
    }

    rec = crash_record_get();
    if (rec == NULL)
    {
        (void)uart_log_write("crash: no record\r\n", 18U);
        return FUNC_STATUS_SUCCESS;
    }

    (void)LOGI("crash kind=%lu line=%lu\r\n", rec->kind, rec->line);
    (void)LOGI("  expr: %s\r\n", rec->expression);
    (void)LOGI("  file: %s\r\n", rec->file);
    if (rec->kind == CRASH_KIND_FAULT)
    {
        (void)LOGI("  fault kind=%lu pc=%08lx lr=%08lx sp=%08lx xpsr=%08lx\r\n", rec->fault.fault_kind, rec->fault.pc, rec->fault.lr, rec->fault.sp, rec->fault.xpsr);
        (void)LOGI("  cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx\r\n", rec->fault.cfsr, rec->fault.hfsr, rec->fault.mmfar, rec->fault.bfar);
    }
    return FUNC_STATUS_SUCCESS;
}

/*
 * stack — 打印栈哨兵状态（paint 区间 / 栈峰值 / 余量 / 告警）。纯只读，可反复调。
 *
 * `peak` 是「自 stack_guard_init 起，栈最深到过哪里」，`low` 是对应地址。
 * ⚠ `peak` 只在**堆没被用过**时才是栈峰值——堆从 `dtcm_bottom` 向上长，同样会抹掉
 *   pattern。所以报告把 `dtcm_bottom` 一并打出来：`low` 贴着它 ⇒ 那是堆顶不是栈底。
 * 告警只在**首次置位**时打过 LOGI/LOGE；`hits` 持续增长说明还在踩。
 */
static int uart_cli_stack(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    stack_guard_report();
    (void)LOGI("[STK ] heap_used=%lu B (heap+stack share DTCM)\r\n", syscalls_heap_used());
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
    bool do_erase = false;

#ifndef NDEBUG
    /* 负例注入：bbt fault <plane> <block> —— 把指定物理块标 BAD 并触发候选池重算。
     * 用于证明「BAD 槽真的被 shared metadata spare 顶替，记录仍可定位」。
     * ⚠ 若该块持有活元数据，会丢一份副本；只在 debug 档提供。 */
    if ((argc == 4) && (strcmp(argv[1], "fault") == 0))
    {
        ftl_ctx_t *ftl = g_ctrl.ftl[0];

        if (ftl == NULL)
        {
            LOGE("bbt fault: no initialized FTL instance\r\n");
            return FUNC_STATUS_ERROR;
        }

        QWORD plane_q = 0U;
        QWORD block_q = 0U;

        if ((!uart_cli_parse_uint(argv[2], &plane_q)) || (!uart_cli_parse_uint(argv[3], &block_q)) || (plane_q >= PLANE_NUM) || (block_q >= BLOCKS_PER_PLANE))
        {
            LOGE("bbt fault: plane/block out of range\r\n");
            return FUNC_STATUS_ERROR;
        }

        const BYTE plane  = (BYTE)plane_q;
        const WORD block  = (WORD)block_q;
        const WORD gblock = ftl_gblock(plane, block);

        LOGW("bbt fault: mark gblock=%hu (plane=%hhu block=%hu) BAD (debug injection)\r\n", gblock, plane, block);

        (void)bbt_mark_bad(&ftl->alloc, gblock);
        md_pool_service(ftl); /* 元数据 job 忙时会顺延到下一轮 ftl_service */

        if (ftl->alloc.pools->dirty)
        {
            LOGW("bbt fault: pool rebuild deferred (metadata job busy) — run 'pool' again\r\n");
        }

        md_pool_print(ftl->metadata_dir);
        return FUNC_STATUS_SUCCESS;
    }
#endif

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
        do_erase = true;
        LOGW("bbt --erase: erasing EVERY block of every instance (incl. metadata). Do not reset until done.\r\n");
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_t *ftl = g_ctrl.ftl[die];

        if (ftl == NULL)
        {
            continue;
        }
        /* 擦除前先关掉本实例的后台写：擦除要跑十几秒，且擦完介质已空而 RAM 状态
         * 陈旧（CP_TIME_INTERVAL_MS=10s 的定时 checkpoint 会把旧状态写回去，
         * 用户就永远看不到"擦干净"）。置位后 ftl_service 跳过后台调度步，无清除路径。 */
        if (do_erase == true)
        {
            ftl->flags.bg_suspend = 1U;
            /* 页打包写缓冲里可能还攒着未编程的 CW——它们若在擦净后被 flush，
             * 会把数据编到刚擦空的芯片上（介质状态与 RAM 描述不再一致）。
             * 走 wbuf 的复位接口（不是裸 memset）：它会一并结算那些写组的在飞数，
             * 否则 host 写组永不完成。 */
            {
                wb_trigger_t trigger;
                wb_status_t  status;

                memset(&trigger, 0, sizeof(trigger));
                trigger.kind = WBUF_TRIGGER_RESET;
                (void)wb_handle(ftl->wbuf, &trigger, &status);
            }

            /* 全片擦除会抹掉 BBT 池：先等在飞的 BBT 异步写完成，避免它在擦净后
             * 又把一条 record 写回已擦空的块（RISK-BBT-20 的同类重叠）。 */
            for (WORD spin = 0U; (ftl->alloc.bbt.handle.waiting == true) && (spin < 1000U); spin++)
            {
                ftl_service_pump();
            }
        }
        bbt_print_bad_map(&ftl->alloc, do_erase);
    }

    if (do_erase == true)
    {
        LOGW("bbt --erase: done. POWER-CYCLE NOW — the running FTL rewrites metadata if left idle.\r\n");
    }

    return FUNC_STATUS_SUCCESS;
}

/*
 * format — boot 失败后的恢复格式化（破坏性）
 *
 * 用途：CP/目录检查点介质状态无法被 pfr_media_check_format 判定为「空片/版本不符」
 * （例如上一轮 bbt --erase 后仍有擦不掉的坏块残留数据），自动格式化会保守拒绝。
 * 本命令直接对已 ftl_init 的实例执行 pfr_media_format：重落 BBT（带
 * FORMAT_IN_PROGRESS）→ 擦除 BBT 池之外的块 → 写当前版本初始 Checkpoint。
 *
 * ⚠ 擦除全部用户数据与元数据；执行后必须断电重启，不能在运行中的 FTL 上继续用。
 */
static int uart_cli_format(int argc, char *argv[])
{
    BYTE ch;
    BYTE ce;
    BYTE lun;
    BYTE done = 0U;

    /* 可选的 --verify：对**出厂坏块标记已丢失的二手片**做一次全片擦/写/读体检，
     * 重建可信坏块表（耗时数分钟）。默认不做，避免例行 format 变慢。 */
    const bool verify = (argc >= 2) && (strcmp(argv[1], "--verify") == 0);

    if ((argc >= 2) && (!verify))
    {
        (void)uart_log_write("error: unknown option (only --verify)\r\n", sizeof("error: unknown option (only --verify)\r\n") - 1U);
        return FUNC_STATUS_ERROR;
    }

    LOGW("format: ERASING every block and writing initial metadata. Do not reset until done.\r\n");

    if (verify)
    {
        LOGW("format --verify: full-chip erase check enabled (erase OK = good block).\r\n");
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ch  = die_2_ch(die);
        ce  = die_2_ce(die);
        lun = die_2_lun(die);

        ftl_ctx_t *ftl = g_ctrl.ftl[die];

        if (ftl == NULL)
        {
            continue;
        }

        /* 格式化期间禁止后台写；与 bbt --erase 同口径。 */
        ftl->flags.bg_suspend = 1U;
        {
            wb_trigger_t trigger;
            wb_status_t  status;

            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_RESET;
            (void)wb_handle(ftl->wbuf, &trigger, &status);
        }

        /* 格式化会重写 BBT 池：先等在飞的 BBT 异步写完成，避免 sync save 与它
         * 二次编程同一槽（RISK-BBT-20；bbt_save 也会拒绝这种重叠，这里把正常
         * 路径等干净，而不是让 format 直接失败）。泵是全局的，顺便推进其它实例。 */
        for (WORD spin = 0U; (ftl->alloc.bbt.handle.waiting == true) && (spin < 1000U); spin++)
        {
            ftl_service_pump();
        }

        if (pfr_media_format(ftl, verify) != FTL_OK)
        {
            LOGE("format: failed ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
            return FUNC_STATUS_ERROR;
        }

        done = 1U;
        LOGI("format: done ch=%u ce=%u lun=%u\r\n", ch, ce, lun);
    }

    if (done == 0U)
    {
        LOGE("format: no initialized FTL instance\r\n");
        return FUNC_STATUS_ERROR;
    }

    LOGW("format: done. POWER-CYCLE NOW.\r\n");
    return FUNC_STATUS_SUCCESS;
}

/*
 * pool — 打印固定元数据分区的候选块池（只读诊断）
 *
 * 用于验证 BBT + spare 候选池：nominal 块被标 BAD 后，候选数不变，
 * 坏块由 shared metadata spare 顶替（打印 remapped -> spare）。
 */
static int uart_cli_pool(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    BYTE done = 0U;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const BYTE ch  = die_2_ch(die);
        const BYTE ce  = die_2_ce(die);
        const BYTE lun = die_2_lun(die);

        const ftl_ctx_t *ftl = g_ctrl.ftl[die];

        if (ftl == NULL)
        {
            continue;
        }

        LOGI(LOG_TAG_POOL "ch=%hhu ce=%hhu lun=%hhu\r\n", ch, ce, lun);
        md_pool_print(ftl->metadata_dir);
        done = 1U;
    }

    if (done == 0U)
    {
        LOGE("pool: no initialized FTL instance\r\n");
        return FUNC_STATUS_ERROR;
    }

    return FUNC_STATUS_SUCCESS;
}

/*
 * rdpaa — 按 PAA 读一个 NAND 页（只读诊断）
 *
 * 默认：与 FTL 数据面相同的正常配置（scrambler ON、BCH ECC、per-CW CRC；
 *       页内 META 提供 LBA，LBA 未知时由 FC 自描述读回）。
 * --raw：LDPC_RAW 裸读——不做 ECC/解扰，坏页也能拿到原始位（分析 UNC 用）。
 * 同时用 gc_read_page_with_lba 打印该页逐 CW 的 META（页内反向映射）。
 * ⚠ PAA 不含 ch/ce：本命令固定用 ch=0/ce=0（当前板单芯片）。
 */
static int uart_cli_rdpaa(int argc, char *argv[])
{
    char *end = NULL;

    const DWORD paa = strtoul(argv[1], &end, 0); /* DWORD ≡ unsigned long：同型，无强转 */
    const bool  raw = (argc >= 3) && (strcmp(argv[2], "--raw") == 0);

    if ((argc >= 3) && (!raw))
    {
        (void)uart_log_write("error: unknown option (only --raw)\r\n", sizeof("error: unknown option (only --raw)\r\n") - 1U);
        return FUNC_STATUS_ERROR;
    }

    if ((end == NULL) || (*end != '\0') || (paa_is_valid_paa(paa) == 0U))
    {
        (void)uart_log_write("error: bad paa\r\n", 16U);
        return FUNC_STATUS_ERROR;
    }

    if ((g_cli_pending.busy != 0U) || usb_msc_host_busy())
    {
        (void)uart_log_write("error: busy, pending request\r\n", 29U);
        return FUNC_STATUS_ERROR;
    }

    /* PAA 的 cw 位对整页读无意义：清掉，固定按页基址读。 */
    const DWORD pba = pba_page(paa_to_pba(0U, 0U, paa));

    /* raw 读需要 4 × 528B；静态缓冲，避免占 CLI 栈。 */
    static BYTE buf[PAGE_SIZE];

    (void)memset(buf, 0, sizeof(buf));

    const ftl_status_e st = raw ? ftl_sync_read_page_raw(pba, buf) : ftl_sync_read_page(pba, buf, PAGE_MAIN_SIZE, FTL_PAGE_CFG_DATA_ANY());

    DWORD              cw_lba[CW_NUM];
    const ftl_status_e meta_st = gc_read_page_with_lba(pba, cw_lba);

    (void)LOGI("rdpaa paa=0x%08lx pba=0x%08lx mode=%s read=%s meta=%s\r\n", paa, pba, raw ? "raw" : "normal", (st == FTL_OK) ? "ok" : "FAIL", (meta_st == FTL_OK) ? "ok" : "FAIL");

    if (meta_st == FTL_OK)
    {
        (void)LOGI("  cw_lba: %08lx %08lx %08lx %08lx\r\n", cw_lba[0], cw_lba[1], cw_lba[2], cw_lba[3]);
    }

    if (st != FTL_OK)
    {
        LOGE("rdpaa: read failed (ECC/CRC/META?) — use --raw 取原始位\r\n");
        return FUNC_STATUS_ERROR;
    }

    /* 只 dump 前 256B（16 行）：诊断足够，且不会把串口写满。 */
    for (DWORD offset = 0U; (offset < 256U) && (offset < PAGE_MAIN_SIZE); offset += 16U)
    {
        (void)LOGI("%08lx: %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx  %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx\r\n", offset, buf[offset], buf[offset + 1U], buf[offset + 2U], buf[offset + 3U], buf[offset + 4U], buf[offset + 5U], buf[offset + 6U], buf[offset + 7U], buf[offset + 8U], buf[offset + 9U], buf[offset + 10U], buf[offset + 11U], buf[offset + 12U], buf[offset + 13U], buf[offset + 14U], buf[offset + 15U]);
    }

    return FUNC_STATUS_SUCCESS;
}

/* rd 完成：compare 模式与 g_wr_buf pattern 比对，报 PASS / 首个不一致偏移 */
static void uart_cli_rd_report(void)
{
    DWORD total_bytes = g_cli_pending.nblocks * FTL_IU_BYTES; /* n 计扇区（512B），非页 */
    DWORD offset;

    if (g_cli_pending.compare == 0U)
    {
        /* hex dump 前 64B 防刷屏 */
        (void)LOGI("rd ok n=%lu, first 64 bytes:\r\n", g_cli_pending.nblocks);
        for (offset = 0U; (offset < 64U) && (offset < total_bytes); offset += 16U)
        {
            (void)LOGI("%08lx: %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx  %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx\r\n", offset, g_wr_buf[offset], g_wr_buf[offset + 1U], g_wr_buf[offset + 2U], g_wr_buf[offset + 3U], g_wr_buf[offset + 4U], g_wr_buf[offset + 5U], g_wr_buf[offset + 6U], g_wr_buf[offset + 7U], g_wr_buf[offset + 8U], g_wr_buf[offset + 9U], g_wr_buf[offset + 10U], g_wr_buf[offset + 11U], g_wr_buf[offset + 12U], g_wr_buf[offset + 13U], g_wr_buf[offset + 14U], g_wr_buf[offset + 15U]);
        }
        return;
    }

    /* compare：按 4B 粒度与期望 pattern 比对（plane0=0x55555555 / plane1=0xAAAAAAAA） */
    for (offset = 0U; offset + sizeof(DWORD) <= total_bytes; offset += sizeof(DWORD))
    {
        DWORD expect = uart_cli_expect_word(offset);
        DWORD actual;

        memcpy(&actual, &g_wr_buf[offset], sizeof(actual));
        if (actual != expect)
        {
            (void)LOGE("rd FAIL at byte %lu: got %08lx expect %08lx\r\n", offset, actual, expect);
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
            (void)LOGI("wr done lba=%llu n=%lu status=%d\r\n", g_cli_pending.lba, g_cli_pending.nblocks, (int)g_cli_pending.req.completion_status);
            break;
        case SSD_REQUEST_READ:
            uart_cli_rd_report();
            break;
        case SSD_REQUEST_TRIM:
            (void)LOGI("tr done lba=%llu status=%d\r\n", g_cli_pending.lba, (int)g_cli_pending.req.completion_status);
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
    {   "help",                                                                                                                                                              "help",                                                                         "show command list", 1U,  1U,    uart_cli_help},
    { "status",                                                                                                                                                            "status",                                                        "show UART error and queue counters", 1U,  1U,  uart_cli_status},
    {"version",                                                                                                                                                           "version",                                                             "show firmware console version", 1U,  1U, uart_cli_version},
    {    "log",                                                                                                                                 "log level <error|warn|info|debug>",                                                                     "set runtime log level", 3U,  3U,     uart_cli_log},
 /* UART CLI 驱动 FTL（C 档，D10）：wr/rd/tr/fl/info 模拟 NVMe host IO */
    {     "wr",                                                                                                                                                      "wr <lba> <n>",                                                         "write n blocks with fixed pattern", 3U,  3U,      uart_cli_wr},
    {     "rd",                                                                                                                                                  "rd <lba> <n> [c]",                                                         "read n blocks (c=compare pattern)", 3U,  4U,      uart_cli_rd},
    {     "tr",                                                                                                                                                          "tr <lba>",                                                                              "trim one lba", 2U,  2U,      uart_cli_tr},
    {     "fl",                                                                                                                                                                "fl",                                                         "flush dirty L2P + journal to NAND", 1U,  1U,      uart_cli_fl},
    {     "wp",                                                                                                                                                  "wp <lba> <n> <k>",                                     "submit k writes in one pair window (pack-buffer diag)", 4U,  4U,      uart_cli_wp},
    {   "info",                                                                                                                                                              "info",                                                          "show controller + FTL statistics", 1U,  1U,    uart_cli_info},
    {    "map",                                                                                                                                                         "map <lba>",                               "print L2P mapping + target block BST entry (read-only diag)", 2U,  2U,     uart_cli_map},
    {   "addr",                                                                                                            "addr pba <pba> | paa <paa> [ch ce] | g <g> [ch ce lun]",                                  "translate PBA/PAA/gblock address fields (read-only diag)", 3U,  6U,    uart_cli_addr},
    {  "rdpaa",                                                                                                                                               "rdpaa <paa> [--raw]",                  "read one NAND page by PAA (default: ECC/CRC/descramble; --raw: raw bits)", 2U,  3U,   uart_cli_rdpaa},
    {  "crash",                                                                                                                                                     "crash [clear]",                                                       "show/clear the .noinit crash record", 1U,  2U,   uart_cli_crash},
    {  "stack",                                                                                                                                                             "stack",                                        "show stack guard: paint region, peak usage, alarms", 1U,  1U,   uart_cli_stack},
    {    "bbt",                                                                                                                             "bbt [--erase | fault <plane> <block>]",       "print factory bad block map (--erase: erase each block; fault: debug BAD injection)", 1U,  4U,     uart_cli_bbt},
    { "format",                                                                                                                                                 "format [--verify]", "ERASE all blocks + write initial metadata (destructive; --verify = full-chip erase check)", 1U,  2U,  uart_cli_format},
    {   "pool",                                                                                                                                                              "pool",                               "print fixed-metadata candidate block pools (read-only diag)", 1U,  1U,    uart_cli_pool},
#if defined(FC_NAND_TEST_ENABLE)
    {   "test", "test [--seed N] [--iterations N] [--single] [--list] [--list-params] [--log-visible info|debug] [--log-project info|debug] [--param name value]... [test_name...]",                                                                 "run munit unit test suite", 1U, 24U,    uart_cli_test},
#endif
};

static int uart_cli_help(int argc, char *argv[])
{
    DWORD index;

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
        return FUNC_STATUS_ERROR;
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
        return FUNC_STATUS_ERROR;
    }

    (void)uart_log_write(updated, sizeof(updated) - 1U);
    return FUNC_STATUS_SUCCESS;
}

#if defined(FC_NAND_TEST_ENABLE)
/*
 * 跑 UT 期间的**项目日志**级别，默认整档关闭（LOG_OFF）。
 *
 * 与 munit 的 `--log-visible` **分开**：后者控制测试自身证据（`[EV]` summary/detail），
 * 前者只控制 FTL/FC 内部日志。二者混在一起时，想看 `[EV]` 就必须同时被 boot/format/GC
 * 的 `[INF]` 刷屏 ⇒ 证据反而看不见。
 *
 *   test /ftl/                          ⇒ 只有用例名 + 结果（项目日志静默、[EV] 隐藏）
 *   test --log-visible info /ftl/       ⇒ 额外打印 [EV] 简单前后对比
 *   test --log-visible debug /ftl/      ⇒ 额外打印 [EV] 完整对比
 *   test --log-visible info --log-project info /ftl/ ⇒ 再叠加项目 INFO 日志（排障用）
 *
 * 跑完恢复原级别（main 的默认 INFO）。
 */
static log_level_e uart_cli_test_project_level(int argc, char *argv[])
{
    log_level_e level = LOG_OFF;

    for (int i = 1; (i + 1) < argc; i++)
    {
        if (strcmp(argv[i], "--log-project") != 0)
        {
            continue;
        }
        if (strcmp(argv[i + 1], "debug") == 0)
        {
            level = LOG_DEBUG;
        }
        else if (strcmp(argv[i + 1], "info") == 0)
        {
            level = LOG_INFO;
        }
        else if (strcmp(argv[i + 1], "warning") == 0)
        {
            level = LOG_WARNING;
        }
        else
        {
            level = LOG_ERROR;
        }
        break;
    }
    return level;
}

static int uart_cli_test(int argc, char *argv[])
{
    const log_level_e saved = log_get_level();
    char             *filtered[UART_CLI_MAX_ARGUMENTS];
    int               filtered_argc = 0;
    int               rc;

    log_set_level(uart_cli_test_project_level(argc, argv));

    /* munit 不认识 --log-project（它只认自己的 --log-visible/--log-fatal 等），
     * 直接透传会走 "unknown argument" 分支并跳过全部用例。这里在调用前把它和取值摘掉。 */
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "--log-project") == 0)
        {
            if ((i + 1) < argc)
            {
                i++;
            }
            continue;
        }
        if (filtered_argc < (int)UART_CLI_MAX_ARGUMENTS)
        {
            filtered[filtered_argc++] = argv[i];
        }
    }

    rc = ut_run_all(filtered_argc, filtered);
    log_set_level(saved);
    return rc;
}
#endif

/*
 * 仅支持空格和 Tab 分隔，不支持引号、转义或二进制参数。该约束让调试 CLI 保持小且可预测；
 * 产测或固件下载应使用独立的二进制帧协议，而不是继续扩展这个文本解析器。
 */
#define UART_CLI_ARGC_INVALID (UINT32_MAX)
static DWORD uart_cli_tokenize(char *line, char *argv[], DWORD max_arguments)
{
    DWORD argc   = 0U;
    char *cursor = line;

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
            return UART_CLI_ARGC_INVALID;
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

    return argc;
}

static void uart_execute_command(char *line)
{
    static const char unknown[] = "error: unknown command\r\n";
    char             *argv[UART_CLI_MAX_ARGUMENTS];
    DWORD             count;
    DWORD             index;

    count = uart_cli_tokenize(line, argv, UART_CLI_MAX_ARGUMENTS);
    if (count == 0U)
    {
        return;
    }
    if (count == UART_CLI_ARGC_INVALID)
    {
        (void)uart_log_write("error: too many arguments\r\n", 27U);
        return;
    }

    const int argc = (int)count; /* count ≤ UART_CLI_MAX_ARGUMENTS，窄化安全 */

    for (index = 0U; index < UART_ARRAY_SIZE(g_uart_cli_commands); index++)
    {
        const uart_cli_command_t *command = &g_uart_cli_commands[index];

        if (strcmp(argv[0], command->name) != 0)
        {
            continue;
        }

        /* ⚠ 只有**参数个数**不合法才打 usage。handler 的返回码表示「执行失败」，
         *   而失败原因它自己已经打过（busy / invalid lba / submit failed …）。
         *   旧写法把两者并进同一个 `||`：handler 返回 FUNC_STATUS_ERROR(=1) 也被当成
         *   "参数错" ⇒ `wr 0 32` 在 busy 时会紧跟一条 `usage: wr <lba> <n>`，看着像
         *   语法错误、实际参数完全合法（wr/rd/tr/fl 四个命令的 busy 分支全中）。 */
        if ((argc < command->min_argc) || (argc > command->max_argc))
        {
            (void)LOGE("usage: %s\r\n", command->usage);
            return;
        }

        (void)command->handler(argc, argv);
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
    BYTE byte;

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
