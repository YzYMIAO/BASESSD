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
#include "l2p_cache.h"
#include "log.h"
#include "ssd_request.h"
#include "stack_guard.h"
#include "usart.h"
#include "wbuf.h"

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
static int uart_cli_wp(int argc, char *argv[]); /* 页打包写缓冲合并诊断（§七 方案 A） */
static int uart_cli_info(int argc, char *argv[]);
static int uart_cli_map(int argc, char *argv[]);   /* L2P 映射 + 目标块 BST 条目（只读诊断） */
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
 * 固定 pattern 写 buffer：16KB，落 **AXI_RAM .bss**（启动清零；cli_init 仍主动预填
 * 0x55/0xAA）。阶段 C 起 SRAM1+SRAM2 合并给 FTL tables（2×123,896B），
 * 16KB CLI 缓冲放不进 SRAM12 的 14KB 余量，故迁回 AXI（见 main.c 内存断言）。
 *
 * ⚠ CW 级映射（IU = 1 CW = 512B）：`wr lba n` / `rd lba n` 的 n 单位是**扇区**
 *   （每个 512B），缓冲步进 = FTL_IU_BYTES。故 **n 上限 = 16KB / 512B = 32**
 *   （旧页级模型下 n 计 2048B 页，上限 8）。超出由 controller 的 buffer_size
 *   校验拒绝（写爆 g_wr_buf 是真实隐患）。
 */
static uint8_t g_wr_buf[FTL_READ_USER_SIZE * CW_NUM * PLANE_NUM]; /* 16KB → AXI .bss */

/* n（扇区数）上限：g_wr_buf 容量 ÷ 映射单元字节数 */
#define UART_CLI_MAX_SECTORS ((uint32_t)(sizeof(g_wr_buf) / FTL_IU_BYTES))

/* ── wp 诊断缓冲（§七 方案 A 合并可观测性）──
 *
 * 一个 pair 窗口 = FTL_LBA_PER_PAIR（= 8）个 CW = 8 扇区。wp 一次提交 k 个组、
 * 每组 n 个扇区（k*n ≤ 8），各组的源数据取自本缓冲的**不相交切片**（组 i 用
 * 扇区段 [i*n, i*n+n)）。切片内容按组号填不同字节，便于 rd 回读时肉眼分辨
 * 「哪一段是哪个组写的」——若打包把 CW 错位，回读会看到串段的字节。 */
static uint8_t g_wp_buf[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 × 512B = 4KB */
/* 每组一个完成标志/状态（wp 不等待，仅作在飞期间的写回目标，避免悬垂指针）。 */
static volatile uint8_t      g_wp_done[FTL_LBA_PER_PAIR];
static volatile ssd_status_t g_wp_status[FTL_LBA_PER_PAIR];

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

/* CLI 诊断命令（map / pair 窗口）按 LBA 取所属 FTL 实例。
 * 复用 l2p_cache.h 的分片公式；8-LBA pair 窗口不跨 region，故按窗口基址取即可。 */
static ftl_context_t *uart_cli_ftl_for_lba(uint64_t lba)
{
    return g_ctrl.ftl[lba_route_ch(lba)][lba_route_ce(lba)][lba_route_lun(lba)];
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
 *       wbuf_handle(FLUSH) 的 `[FTL] wbuf flush` 行是辅助证据，且是 LOGD ⇒ **仅 debug 档可见**。
 *       随后 `rd <lba> <k*n>` 回读，第 (i*n+j) 段字节应为 0xA0+i。
 */
static int uart_cli_wp(int argc, char *argv[])
{
    uint64_t       lba;
    uint64_t       n_parsed;
    uint64_t       k_parsed;
    uint32_t       n;
    uint32_t       k;
    uint32_t       i;
    uint32_t       j;
    ftl_context_t *ftl;

    (void)argc;

    if ((uart_cli_parse_uint(argv[1], &lba) != 0) ||
        (uart_cli_parse_uint(argv[2], &n_parsed) != 0) ||
        (uart_cli_parse_uint(argv[3], &k_parsed) != 0) ||
        (n_parsed == 0U) || (k_parsed < 2U) || (n_parsed > 0xFFFFFFFFU) || (k_parsed > 0xFFFFFFFFU))
    {
        (void)uart_log_write("error: invalid lba/n/k (need n>=1, k>=2)\r\n", 41U);
        return FUNC_STATUS_ERROR;
    }
    n = (uint32_t)n_parsed;
    k = (uint32_t)k_parsed;
    if ((uint64_t)n * (uint64_t)k > FTL_LBA_PER_PAIR)
    {
        (void)uart_log_write("error: n*k must be <= 8 (one pair window)\r\n", 42U);
        return FUNC_STATUS_ERROR;
    }
    if ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U)
    {
        (void)uart_log_write("error: lba must be 8-aligned (pair window base)\r\n", 47U);
        return FUNC_STATUS_ERROR;
    }
    if (!uart_cli_lba_range_ok(lba, n * k))
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
        uint8_t *slice = &g_wp_buf[(i * n) * FTL_IU_BYTES];

        for (j = 0U; j < (n * FTL_IU_BYTES); j++)
        {
            slice[j] = (uint8_t)(0xA0U + i);
        }
        g_wp_done[i]   = 0U;
        g_wp_status[i] = SSD_STATUS_OK;
    }

    /* 逐组提交（不查 busy）：k 个组同时在飞 ⇒ 窗口填满时合并成 1 次编程。
     * 各组 complete_flag/status_out 指向静态数组，完成前始终有效。 */
    for (i = 0U; i < k; i++)
    {
        ftl_io_request_t req;

        memset(&req, 0, sizeof(req));
        req.lba           = lba + ((uint64_t)i * (uint64_t)n);
        req.buffer        = &g_wp_buf[(i * n) * FTL_IU_BYTES];
        req.count         = n;
        req.complete_flag = &g_wp_done[i];
        req.status_out    = &g_wp_status[i];

        if (ftl_submit_write(ftl, &req) != FTL_OK)
        {
            (void)LOGE("wp: group %lu submit failed\r\n", (unsigned long)i);
            return FUNC_STATUS_ERROR;
        }
    }

    (void)LOGI("wp submit ok lba=%lu n=%lu k=%lu (expect 1 program on flush)\r\n",
               (unsigned long)lba,
               (unsigned long)n,
               (unsigned long)k);
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
                (void)LOGI("ftl[%u][%u][%u] free=%u op=%u/%u spare=%u/%u bad=%u closed=%u thrash=%u miss=%u%%\r\n",
                           (unsigned)ch,
                           (unsigned)ce,
                           (unsigned)lun,
                           (unsigned)ftl->alloc.free_count,
                           (unsigned)allocator_op_count(&ftl->alloc, 0U),
                           (unsigned)allocator_op_count(&ftl->alloc, 1U),
                           (unsigned)allocator_spare_count(&ftl->alloc, 0U),
                           (unsigned)allocator_spare_count(&ftl->alloc, 1U),
                           (unsigned)ftl->alloc.bad_count,
                           (unsigned)ftl->alloc.closed_sb_count,
                           (unsigned)ftl->cache->thrashing,
                           (unsigned)ftl->cache->miss_pct);

                /* 元数据整理 job 的现场（心跳默认档不打，靠这里观测）。
                 * 只在有 job 在跑 / 有待回收块时多打一行，空闲时不刷屏。 */
                const metadata_dir_cache_t *md  = ftl->metadata_dir;
                const metadata_handle_t    *job = &md->handle;

                if ((job->phase != (uint8_t)MD_JOB_IDLE) || (md->reclaim_pending_count != 0U))
                {
                    (void)LOGI("md[%u][%u][%u] phase=%u moved=%u stall=%lums pending=%u victim=%u\r\n",
                               (unsigned)ch,
                               (unsigned)ce,
                               (unsigned)lun,
                               (unsigned)job->phase,
                               (unsigned)job->moved,
                               (unsigned long)job->stall_ms,
                               (unsigned)md->reclaim_pending_count,
                               (unsigned)job->victim);
                }
            }
        }
    }
    return FUNC_STATUS_SUCCESS;
}

/*
 * map <lba> — 只读诊断：打印该 LBA 的 L2P 映射与**目标块**的 BST 条目。
 *
 * 起因：`ftl_is_metadata_block` 缺 plane 掩码（v0.69 修）时，被日志块掩住的用户块
 * 进不了 Free Pool、也被 F2 重建跳过 ⇒ `valid_count` 恒 0，而 L2P 仍可能指向它
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
    ftl_context_t     *ftl;
    const ftl_block_t *b;
    nand_pba_t         pba;
    uint64_t           lba;
    uint16_t           g;

    (void)argc;

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

    ftl = uart_cli_ftl_for_lba(lba);
    if (ftl == NULL)
    {
        (void)uart_log_write("error: ftl instance not ready\r\n", 31U);
        return FUNC_STATUS_ERROR;
    }

    /* ⚠ CLI 上下文（不在完成派发里）：region 未驻留时自旋泵等加载完成。
     * 自旋失败（超预算）时 pba 保持 INVALID，输出按"未映射"处理——CLI 只读不写，
     * 误报一次"未映射"无副作用。 */
    if (l2p_cache_read_sync(ftl->cache, (lba_t)lba, &pba) != FTL_OK)
    {
        pba = NAND_PBA_INVALID;
    }
    if (pba == NAND_PBA_INVALID)
    {
        (void)LOGI("map lba=%lu -> INVALID (未映射)\r\n", (unsigned long)lba);
        return FUNC_STATUS_SUCCESS;
    }

    g = ftl_pba_gblock(pba);
    b = &ftl->alloc.blocks[g];
    (void)LOGI("map lba=%lu pba=%08lx ch=%lu ce=%lu lun=%lu plane=%lu blk=%lu pg=%lu cw=%lu g=%u\r\n",
               (unsigned long)lba,
               (unsigned long)pba,
               (unsigned long)pba_to_ch(pba),
               (unsigned long)pba_to_ce(pba),
               (unsigned long)pba_to_lun(pba),
               (unsigned long)pba_to_plane(pba),
               (unsigned long)pba_to_block(pba),
               (unsigned long)pba_to_page(pba),
               (unsigned long)pba_to_cw(pba),
               (unsigned)g);
    (void)LOGI("    block g=%u state=%s valid=%u sb=%u ec=%lu\r\n",
               (unsigned)g,
               ftl_block_state_name(ftl_block_state(b)),
               (unsigned)b->valid_count,
               (unsigned)b->sb_id,
               (unsigned long)b->erase_count);

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

    (void)LOGI("crash kind=%lu line=%lu\r\n", (unsigned long)rec->kind, (unsigned long)rec->line);
    (void)LOGI("  expr: %s\r\n", rec->expression);
    (void)LOGI("  file: %s\r\n", rec->file);
    if (rec->kind == (uint32_t)CRASH_KIND_FAULT)
    {
        (void)LOGI("  fault kind=%lu pc=%08lx lr=%08lx sp=%08lx xpsr=%08lx\r\n",
                   (unsigned long)rec->fault.fault_kind,
                   (unsigned long)rec->fault.pc,
                   (unsigned long)rec->fault.lr,
                   (unsigned long)rec->fault.sp,
                   (unsigned long)rec->fault.xpsr);
        (void)LOGI("  cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx\r\n",
                   (unsigned long)rec->fault.cfsr,
                   (unsigned long)rec->fault.hfsr,
                   (unsigned long)rec->fault.mmfar,
                   (unsigned long)rec->fault.bfar);
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
                    /* 页打包写缓冲里可能还攒着未编程的 CW——它们若在擦净后被 flush，
                     * 会把数据编到刚擦空的芯片上（介质状态与 RAM 描述不再一致）。
                     * 走 wbuf 的复位接口（不是裸 memset）：它会一并结算那些写组的在飞数，
                     * 否则 host 写组永不完成。 */
                    {
                        const wbuf_trigger_t trigger = {.kind = WBUF_TRIGGER_RESET};
                        wbuf_status_t        status;
                        (void)wbuf_handle(ftl->wbuf, &trigger, NULL, &status);
                    }
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
    {     "wp",                                                                                               "wp <lba> <n> <k>",        "submit k writes in one pair window (pack-buffer diag)", 4U,  4U,      uart_cli_wp},
    {   "info",                                                                                                           "info",                             "show controller + FTL statistics", 1U,  1U,    uart_cli_info},
    {    "map",                                                                                                      "map <lba>",  "print L2P mapping + target block BST entry (read-only diag)", 2U,  2U,     uart_cli_map},
    {  "crash",                                                                                                  "crash [clear]",                          "show/clear the .noinit crash record", 1U,  2U,   uart_cli_crash},
    {  "stack",                                                                                                          "stack",           "show stack guard: paint region, peak usage, alarms", 1U,  1U,   uart_cli_stack},
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

        /* ⚠ 只有**参数个数**不合法才打 usage。handler 的返回码表示「执行失败」，
         *   而失败原因它自己已经打过（busy / invalid lba / submit failed …）。
         *   旧写法把两者并进同一个 `||`：handler 返回 FUNC_STATUS_ERROR(=1) 也被当成
         *   "参数错" ⇒ `wr 0 32` 在 busy 时会紧跟一条 `usage: wr <lba> <n>`，看着像
         *   语法错误、实际参数完全合法（wr/rd/tr/fl 四个命令的 busy 分支全中）。 */
        if (((uint32_t)argc < command->min_argc) || ((uint32_t)argc > command->max_argc))
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
