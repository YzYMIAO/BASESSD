#include "usart.h"

#include "common/common.h"
#include "dma.h"
#include "ftl/ftl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FC_NAND_TEST_ENABLE)
#    include "ut_main.h"
#endif

#define USART_RX_RING_MASK     (USART_RX_RING_CAPACITY - 1U)
#define USART_TX_RING_MASK     (USART_TX_RING_CAPACITY - 1U)
#define UART_CLI_MAX_ARGUMENTS 10U
#define UART_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#if ((USART_RX_RING_CAPACITY & USART_RX_RING_MASK) != 0U)
#    error "USART_RX_RING_CAPACITY must be a power of two"
#endif

#if ((USART_TX_RING_CAPACITY & USART_TX_RING_MASK) != 0U)
#    error "USART_TX_RING_CAPACITY must be a power of two"
#endif

/*
 * CLI 打印 lba / FTL_LBA_MAX 一律用 %lu，不用 %llu：
 * 本工具链的 newlib-nano（-specs=nano.specs，newlib 3.3.0）不带 long long
 * 格式化支持，%llu 会把 "lu" 当字面量原样打出来（`lba_max=%llu` → `lba_max=lu`），
 * 而 GCC 的 -Wformat 按标准语义检查，编译期不会报。这些值都受
 * uart_cli_lba_range_ok() 约束在 FTL_LBA_MAX 以内，装得下 32 位才安全。
 */
_Static_assert(FTL_LBA_MAX <= 0xFFFFFFFFUL, "FTL_LBA_MAX 超出 32 位，CLI 的 %lu 打印会截断");

UART_HandleTypeDef g_uart1_handle = {0};

/*
 * RX 环形队列：USART IRQ 是唯一生产者，uart_service_poll() 是唯一消费者。
 * IRQ 只发布 g_rx_head，主循环只推进 g_rx_tail，这是无锁 SPSC 队列成立的前提。
 */
static uint8_t           g_rx_ring[USART_RX_RING_CAPACITY];
static volatile uint16_t g_rx_head;
static volatile uint16_t g_rx_tail;

/*
 * TX 环形队列：普通固件上下文是生产者，USART IRQ 是消费者。
 * uart_log_write() 用极短的关中断区保护“入队 + 打开 TXEIE”的交接，防止日志丢在队列中。
 */
/* The padding makes final cache-line maintenance safe when a DMA segment ends at ring end. */
static uint8_t           g_tx_ring[USART_TX_RING_CAPACITY + 31U] __ALIGNED(32);
static volatile uint16_t g_tx_head;
static volatile uint16_t g_tx_tail;
static volatile uint16_t g_tx_dma_length;
static volatile uint8_t  g_tx_dma_active;
static uint8_t           g_tx_dma_ready;
static volatile uint8_t  g_tx_transport_fault;

static uart_stats_t     g_uart_stats;
static uart_log_level_t g_uart_log_level = UART_LOG_INFO;
static char             g_cli_line[USART_CLI_LINE_CAPACITY];
static uint16_t         g_cli_length;
static uint8_t          g_cli_discarding;
static uint8_t          g_usart_ready;

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

static const char *uart_log_level_name(uart_log_level_t level)
{
    static const char *const names[] = {"ERR", "WRN", "INF", "DBG"};

    return (level <= UART_LOG_DEBUG) ? names[level] : "UNK";
}

static uint8_t uart_rx_dequeue(uint8_t *byte)
{
    uint16_t tail = g_rx_tail;

    if (tail == g_rx_head)
    {
        return FUNC_STATUS_SUCCESS;
    }

    /*
     * IRQ 必须先写 ring[head]，再更新 head；DMB 防止编译器/CPU 重排这两个动作。
     * 这里的 DMB 与生产者中的 DMB 配对，确保消费者不会读到尚未写完的数据。
     */
    *byte = g_rx_ring[tail];
    __DMB();
    g_rx_tail = (tail + 1U) & USART_RX_RING_MASK;
    return FUNC_STATUS_ERROR;
}

static uint8_t uart_tx_dequeue(uint8_t *byte)
{
    uint16_t tail = g_tx_tail;

    if (tail == g_tx_head)
    {
        return FUNC_STATUS_SUCCESS;
    }

    /* 先取出数据再推进 tail；推进 tail 后，该槽位才可以被生产者重新使用。 */
    *byte = g_tx_ring[tail];
    __DMB();
    g_tx_tail = (tail + 1U) & USART_TX_RING_MASK;
    __SEV();
    return FUNC_STATUS_ERROR;
}

/* Called with IRQs masked by uart_log_write(), or from the DMA completion ISR. */
static void uart_tx_dma_start_locked(void)
{
    uint16_t tail;
    uint16_t length;

    if ((g_tx_dma_ready == 0U) || (g_tx_dma_active != 0U) || (g_tx_tail == g_tx_head))
    {
        return;
    }
    tail   = g_tx_tail;
    length = (g_tx_head > tail) ? (uint16_t)(g_tx_head - tail) : (uint16_t)(USART_TX_RING_CAPACITY - tail);

    /* TX Ring is cacheable AXI SRAM; DMA must see bytes written by the CPU before it starts. */
    SCB_CleanDCache_by_Addr((uint32_t *)(uintptr_t)&g_tx_ring[tail], (int32_t)((length + 31U) & ~31U));
    g_tx_dma_length = length;
    g_tx_dma_active = 1U;
    if (HAL_DMA_Start_IT(&g_dma_handle, (uint32_t)&g_tx_ring[tail], (uint32_t)&USART_UX->TDR, length) != HAL_OK)
    {
        g_tx_dma_active = 0U;
        g_uart_stats.tx_dma_errors++;
        g_uart_stats.tx_transport_errors++;
        g_tx_transport_fault = 1U;
        /* No tail movement occurred; the TXE fallback drains the retained bytes. */
        g_tx_dma_ready = 0U;
        USART_UX->CR1 |= USART_CR1_TXEIE_TXFNFIE;
        __SEV();
        return;
    }
    USART_UX->CR3 |= USART_CR3_DMAT;
}

static void uart_tx_dma_complete(DMA_HandleTypeDef *dma_handle)
{
    uint16_t transferred;

    (void)dma_handle;
    USART_UX->CR3 &= ~USART_CR3_DMAT;
    transferred     = g_tx_dma_length;
    g_tx_dma_length = 0U;
    g_tx_tail       = (uint16_t)((g_tx_tail + transferred) & USART_TX_RING_MASK);
    __DMB();
    g_tx_dma_active = 0U;
    __SEV();
    uart_tx_dma_start_locked();
}

static void uart_tx_dma_error(DMA_HandleTypeDef *dma_handle)
{
    uint16_t remaining;
    uint16_t transferred;

    (void)dma_handle;
    USART_UX->CR3 &= ~USART_CR3_DMAT;
    remaining = (uint16_t)__HAL_DMA_GET_COUNTER(&g_dma_handle);
    if (remaining > g_tx_dma_length)
    {
        remaining = g_tx_dma_length;
    }
    transferred     = (uint16_t)(g_tx_dma_length - remaining);
    g_tx_tail       = (uint16_t)((g_tx_tail + transferred) & USART_TX_RING_MASK);
    g_tx_dma_length = 0U;
    g_tx_dma_active = 0U;
    g_uart_stats.tx_dma_errors++;
    g_uart_stats.tx_transport_errors++;
    g_tx_transport_fault = 1U;
    /* The transferred prefix has reached the UART FIFO; TXE sends only the remainder. */
    g_tx_dma_ready = 0U;
    USART_UX->CR1 |= USART_CR1_TXEIE_TXFNFIE;
    __SEV();
}

static void uart_write_prompt(void)
{
    static const char prompt[] = "ssd> ";

    (void)uart_log_write(prompt, sizeof(prompt) - 1U);
}

static void uart_print_status(void)
{
    uart_stats_t stats;

    uart_get_stats(&stats);
    (void)uart_log_printf(UART_LOG_INFO,
                          "uart: rx_ovf=%lu tx_drop=%lu tx_ctx=%lu flush_to=%lu dma_err=%lu tx_fault=%lu ore=%lu fe=%lu ne=%lu pe=%lu\r\n",
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

/*
 * UART CLI 驱动 FTL（C 档，设计文档 §13.4 D10：Host 命令来源 = UART CLI）。
 *
 * 固定 pattern 写 buffer：16KB = 2048B（FTL_READ_USER_SIZE）× 4 CW × 2 Plane，
 * 落 SRAM2（.sram2_data 为 NOLOAD 段，上电不清零 → usart_init 主动预填）。
 *   plane0 区（每页前 2048B）= 0x55555555，plane1 区（后 2048B）= 0xAAAAAAAA。
 * Multi-Plane 写（pages=2）时前 2048B 进 plane0（LBA n）、后 2048B 进 plane1
 * （LBA n+1）——写后读回一眼可确认双页簿记正确。
 */
static uint8_t g_wr_buf[FTL_READ_USER_SIZE * CW_NUM * PLANE_NUM] /* 16KB → SRAM2 */
    __attribute__((section(".sram2_data")));

/* wr/rd 两段式：handler 只提交请求到 controller（异步），结果由
 * uart_service_poll 每轮末尾检查 ssd_controller_is_complete 打印（D11
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
    uint32_t *plane0         = (uint32_t *)(void *)g_wr_buf;
    uint32_t *plane1         = (uint32_t *)(void *)(g_wr_buf + FTL_READ_USER_SIZE * CW_NUM);

    for (i = 0U; i < words_per_half; i++)
    {
        plane0[i] = 0x55555555U;
        plane1[i] = 0xAAAAAAAAU;
    }
}

/* 从 g_wr_buf 对应 plane 区取期望 pattern 值（compare 模式用） */
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
    g_cli_pending.req.buffer_size = (size_t)n * FTL_READ_USER_SIZE;

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)uart_log_printf(UART_LOG_INFO, "wr submit ok lba=%lu n=%lu, waiting...\r\n", (unsigned long)lba, (unsigned long)n);
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
    g_cli_pending.req.buffer_size = (size_t)n * FTL_READ_USER_SIZE;

    if (ssd_controller_submit(&g_ctrl, &g_cli_pending.req) != SSD_STATUS_OK)
    {
        g_cli_pending.busy = 0U;
        (void)uart_log_write("error: submit failed\r\n", 23U);
        return FUNC_STATUS_ERROR;
    }

    (void)uart_log_printf(UART_LOG_INFO, "rd submit ok lba=%lu n=%lu%s, waiting...\r\n", (unsigned long)lba, (unsigned long)n, g_cli_pending.compare ? " c" : "");
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
        (void)uart_log_printf(UART_LOG_INFO, "controller state=%s lba_max=%lu\r\n", state_names[g_ctrl.state], (unsigned long)FTL_LBA_MAX);
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
                (void)uart_log_printf(UART_LOG_INFO,
                                      "ftl[%u][%u][%u] free=%u op=%u spare=%u pre_cond=%u bad=%u closed=%u\r\n",
                                      (unsigned)ch,
                                      (unsigned)ce,
                                      (unsigned)lun,
                                      (unsigned)ftl->alloc.free_count,
                                      (unsigned)ftl->alloc.op_count,
                                      (unsigned)ftl->alloc.spare_count,
                                      (unsigned)ftl->alloc.pre_cond_count,
                                      (unsigned)ftl->alloc.bad_count,
                                      (unsigned)ftl->alloc.closed_sb_count);
            }
        }
    }
    return FUNC_STATUS_SUCCESS;
}

/* rd 完成：compare 模式与 g_wr_buf pattern 比对，报 PASS / 首个不一致偏移 */
static void uart_cli_rd_report(void)
{
    uint32_t total_bytes = g_cli_pending.nblocks * FTL_READ_USER_SIZE;
    uint32_t offset;

    if (g_cli_pending.compare == 0U)
    {
        /* hex dump 前 64B 防刷屏 */
        (void)uart_log_printf(UART_LOG_INFO, "rd ok n=%lu, first 64 bytes:\r\n", (unsigned long)g_cli_pending.nblocks);
        for (offset = 0U; (offset < 64U) && (offset < total_bytes); offset += 16U)
        {
            (void)uart_log_printf(UART_LOG_INFO,
                                  "%08lx: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
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
            (void)uart_log_printf(UART_LOG_ERROR, "rd FAIL at byte %lu: got %08lx expect %08lx\r\n", (unsigned long)offset, (unsigned long)actual, (unsigned long)expect);
            return;
        }
    }
    (void)uart_log_write("rd PASS\r\n", 8U);
}

/* uart_service_poll 每轮末尾调：检查未决 CLI 请求是否被主循环 ftl_service 完成 */
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
            (void)uart_log_printf(UART_LOG_INFO, "wr done lba=%lu n=%lu status=%d\r\n", (unsigned long)g_cli_pending.lba, (unsigned long)g_cli_pending.nblocks, (int)g_cli_pending.req.completion_status);
            break;
        case SSD_REQUEST_READ:
            uart_cli_rd_report();
            break;
        case SSD_REQUEST_TRIM:
            (void)uart_log_printf(UART_LOG_INFO, "tr done lba=%lu status=%d\r\n", (unsigned long)g_cli_pending.lba, (int)g_cli_pending.req.completion_status);
            break;
        case SSD_REQUEST_FLUSH:
            (void)uart_log_printf(UART_LOG_INFO, "fl done status=%d\r\n", (int)g_cli_pending.req.completion_status);
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
    {   "help",                                                    "help",                  "show command list", 1U,  1U,    uart_cli_help},
    { "status",                                                  "status", "show UART error and queue counters", 1U,  1U,  uart_cli_status},
    {"version",                                                 "version",      "show firmware console version", 1U,  1U, uart_cli_version},
    {    "log",                       "log level <error|warn|info|debug>",              "set runtime log level", 3U,  3U,     uart_cli_log},
 /* UART CLI 驱动 FTL（C 档，D10）：wr/rd/tr/fl/info 模拟 NVMe host IO */
    {     "wr",                                            "wr <lba> <n>",  "write n blocks with fixed pattern", 3U,  3U,      uart_cli_wr},
    {     "rd",                                        "rd <lba> <n> [c]",  "read n blocks (c=compare pattern)", 3U,  4U,      uart_cli_rd},
    {     "tr",                                                "tr <lba>",                       "trim one lba", 2U,  2U,      uart_cli_tr},
    {     "fl",                                                      "fl",  "flush dirty L2P + journal to NAND", 1U,  1U,      uart_cli_fl},
    {   "info",                                                    "info",   "show controller + FTL statistics", 1U,  1U,    uart_cli_info},
#if defined(FC_NAND_TEST_ENABLE)
  /* TODO: Level 3 完成后将 uart_cli_nand 替换为 ut_run_all */
  /* { "nand", ... }, */
    {   "test", "test [--seed N] [--list] [--list-params] [test_name...]",          "run munit unit test suite", 1U, 10U,    uart_cli_test},
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

        (void)uart_log_printf(UART_LOG_INFO, "%-38s %s\r\n", command->usage, command->description);
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
        uart_log_set_level(UART_LOG_ERROR);
    }
    else if (strcmp(argv[2], "warn") == 0)
    {
        uart_log_set_level(UART_LOG_WARNING);
    }
    else if (strcmp(argv[2], "info") == 0)
    {
        uart_log_set_level(UART_LOG_INFO);
    }
    else if (strcmp(argv[2], "debug") == 0)
    {
        uart_log_set_level(UART_LOG_DEBUG);
    }
    else
    {
        return -1;
    }

    (void)uart_log_write(updated, sizeof(updated) - 1U);
    return FUNC_STATUS_SUCCESS;
}

#if defined(FC_NAND_TEST_ENABLE)
#    include "ut_main.h"

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
            (void)uart_log_printf(UART_LOG_ERROR, "usage: %s\r\n", command->usage);
        }
        return;
    }

    (void)uart_log_write(unknown, sizeof(unknown) - 1U);
}

/*
 * newlib-nano 的 printf 最终会调用 _write()。把它接到软件 TX 队列后，现有 printf
 * 自动变成非阻塞日志；返回短写表示队列已满，业务代码不应据此重试或等待。
 *
 * 行尾归一化：裸机串口终端（PuTTY/minicom 等）要求 \r\n 才能正确换行。
 * printf/puts/fprintf 在 newlib-nano 中只输出 \n，此处扫描 buffer，
 * 在裸 \n（前面不是 \r）前插入 \r，同时避免已有 \r\n 被重复加 \r。
 * 所有走 printf 的输出（SSD_LOGI/munit 测试报告等）自动受益，
 * 直接调 uart_log_write() 的不受影响。
 */
int _write(int file, char *buffer, int length)
{
    int          i;
    uint8_t      needs_cr;
    int          src_i;
    unsigned int dst_len;

    (void)file;

    if ((buffer == NULL) || (length <= 0))
    {
        return FUNC_STATUS_SUCCESS;
    }

    /* ── 快速路径：buffer 中没有裸 \n，原样发送 ── */
    needs_cr = 0U;
    for (i = 0; i < length; i++)
    {
        if ((buffer[i] == '\n') && ((i == 0) || (buffer[i - 1] != '\r')))
        {
            needs_cr = 1U;
            break;
        }
    }
    if (needs_cr == 0U)
    {
        return uart_log_write(buffer, (uint32_t)length);
    }

    /* ── 慢速路径：在裸 \n 前插入 \r ── */
#define WRITE_BUF_SIZE 512U
    static char converted[WRITE_BUF_SIZE];
    dst_len = 0;

    for (src_i = 0; src_i < length; src_i++)
    {
        if ((buffer[src_i] == '\n') && ((src_i == 0) || (buffer[src_i - 1] != '\r')))
        {
            /* 需要写 \r\n，检查剩余空间 */
            if (dst_len + 2 > WRITE_BUF_SIZE)
            {
                /* 刷新已转换部分再继续 */
                (void)uart_log_write(converted, (uint32_t)dst_len);
                dst_len = 0;
            }
            converted[dst_len++] = '\r';
            converted[dst_len++] = '\n';
        }
        else
        {
            if (dst_len + 1 > WRITE_BUF_SIZE)
            {
                (void)uart_log_write(converted, (uint32_t)dst_len);
                dst_len = 0;
            }
            converted[dst_len++] = buffer[src_i];
        }
    }

    /* 刷剩余 */
    if (dst_len > 0)
    {
        (void)uart_log_write(converted, (uint32_t)dst_len);
    }

    /* 告知上层全部发出（不重试） */
    return length;
#undef WRITE_BUF_SIZE
}

void usart_init(uint32_t baudrate)
{
    g_uart1_handle.Instance                    = USART_UX;
    g_uart1_handle.Init.BaudRate               = baudrate;
    g_uart1_handle.Init.WordLength             = UART_WORDLENGTH_8B;
    g_uart1_handle.Init.StopBits               = UART_STOPBITS_1;
    g_uart1_handle.Init.Parity                 = UART_PARITY_NONE;
    g_uart1_handle.Init.Mode                   = UART_MODE_TX_RX;
    g_uart1_handle.Init.HwFlowCtl              = UART_HWCONTROL_NONE;
    g_uart1_handle.Init.OverSampling           = UART_OVERSAMPLING_16;
    g_uart1_handle.Init.OneBitSampling         = UART_ONE_BIT_SAMPLE_DISABLE;
    g_uart1_handle.Init.ClockPrescaler         = UART_PRESCALER_DIV1;
    g_uart1_handle.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    (void)HAL_UART_Init(&g_uart1_handle);

    g_tx_dma_ready = uart_tx_dma_init();
    if (g_tx_dma_ready != 0U)
    {
        g_dma_handle.XferCpltCallback  = uart_tx_dma_complete;
        g_dma_handle.XferErrorCallback = uart_tx_dma_error;
    }

    /*
     * HAL 仅负责时钟、GPIO、BRR 和基本帧格式初始化。从这里开始 RXNE/TXE 的所有权
     * 交给自定义 IRQ 处理器；不能再同时调用 HAL_UART_Receive_IT()，否则两个状态机
     * 会竞争 RDR 和中断使能位。
     */
    USART_UX->CR1 |= USART_CR1_RXNEIE_RXFNEIE | USART_CR1_PEIE;
    USART_UX->CR3 |= USART_CR3_EIE;
    g_usart_ready = 1U;

    /* UART CLI 驱动 FTL：g_wr_buf 落 NOLOAD SRAM2 段，上电不清零 → 主动预填 pattern */
    uart_cli_pattern_init();
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    RCC_PeriphCLKInitTypeDef rcc_periph_clk_init_struct = {0};
    GPIO_InitTypeDef         gpio_init_struct           = {0};

    if (huart->Instance != USART_UX)
    {
        return;
    }

    rcc_periph_clk_init_struct.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    /* USART1/6 时钟源=PCLK2=100 MHz；UART 波特率由该内核时钟分频得到。 */
    rcc_periph_clk_init_struct.Usart16ClockSelection = RCC_USART16CLKSOURCE_PCLK2;
    (void)HAL_RCCEx_PeriphCLKConfig(&rcc_periph_clk_init_struct);

    USART_UX_CLK_ENABLE();
    USART_TX_GPIO_CLK_ENABLE();
    USART_RX_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin       = USART_TX_GPIO_PIN;
    gpio_init_struct.Mode      = GPIO_MODE_AF_PP;
    gpio_init_struct.Pull      = GPIO_PULLUP;
    gpio_init_struct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio_init_struct.Alternate = USART_TX_GPIO_AF;
    HAL_GPIO_Init(USART_TX_GPIO_PORT, &gpio_init_struct);

    gpio_init_struct.Pin       = USART_RX_GPIO_PIN;
    gpio_init_struct.Alternate = USART_RX_GPIO_AF;
    HAL_GPIO_Init(USART_RX_GPIO_PORT, &gpio_init_struct);

    HAL_NVIC_SetPriority(USART_UX_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(USART_UX_IRQn);
}

void usart_irq_handler(void)
{
    uint32_t status      = USART_UX->ISR;
    uint32_t clear_flags = 0U;
    uint8_t  byte;

    /*
     * 先累计原因再清标志。即便出现 FE/NE/ORE，也要继续读取 RDR 抽干 RX FIFO，
     * 否则 RXNE 会持续触发中断，且下一字节更容易再次溢出。
     */
    if ((status & USART_ISR_PE) != 0U)
    {
        g_uart_stats.parity_errors++;
        clear_flags |= USART_ICR_PECF;
    }
    if ((status & USART_ISR_FE) != 0U)
    {
        g_uart_stats.framing_errors++;
        clear_flags |= USART_ICR_FECF;
    }
    if ((status & USART_ISR_NE) != 0U)
    {
        g_uart_stats.noise_errors++;
        clear_flags |= USART_ICR_NECF;
    }
    if ((status & USART_ISR_ORE) != 0U)
    {
        g_uart_stats.overrun_errors++;
        clear_flags |= USART_ICR_ORECF;
    }

    /*
     * 一次 IRQ 尽量抽干硬件 RX FIFO，而非仅处理一个字节，降低高速连续收包的 ORE 风险。
     * 软件队列满时采取“丢弃新字节、保留旧命令”的策略；rx_overflow 用于后续诊断。
     */
    while ((USART_UX->ISR & USART_ISR_RXNE_RXFNE) != 0U)
    {
        uint16_t head = g_rx_head;
        uint16_t next = (head + 1U) & USART_RX_RING_MASK;

        byte = (uint8_t)USART_UX->RDR;
        if (next == g_rx_tail)
        {
            /* Policy: preserve the oldest unprocessed CLI input and drop new data. */
            g_uart_stats.rx_overflow++;
        }
        else
        {
            g_rx_ring[head] = byte;
            __DMB();
            g_rx_head = next;
        }
    }

    if (clear_flags != 0U)
    {
        USART_UX->ICR = clear_flags;
    }

    if ((g_tx_dma_ready == 0U) && ((USART_UX->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0U) &&
        ((USART_UX->ISR & USART_ISR_TXE_TXFNF) != 0U))
    {
        /*
         * TXE/TXFNF 表示 TDR 或 TX FIFO 尚有位置。连续填充可减少每个字符一次 IRQ 的开销；
         * 真正最后一位离开 TX 引脚需观察 TC，本日志路径不等待 TC。
         */
        while (((USART_UX->ISR & USART_ISR_TXE_TXFNF) != 0U) && uart_tx_dequeue(&byte))
        {
            USART_UX->TDR = byte;
        }

        /* 软件队列为空时关闭 TXEIE，否则 TXE 始终为 1 会产生空转中断。 */
        if (g_tx_tail == g_tx_head)
        {
            USART_UX->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
        }
    }
}

int uart_log_write(const char *data, uint32_t length)
{
    uint32_t written = 0U;

    if ((!g_usart_ready) || (data == NULL) || (length == 0U))
    {
        return FUNC_STATUS_SUCCESS;
    }

    /* A DMA transport error has an ambiguous byte boundary; do not claim later writes are reliable. */
    if (g_tx_transport_fault != 0U)
    {
        return -1;
    }

    if ((__get_IPSR() != 0U) || (__get_PRIMASK() != 0U))
    {
        g_uart_stats.tx_context_errors++;
        return -1;
    }

    while (written < length)
    {
        uint32_t primask = __get_PRIMASK();

        if (g_tx_transport_fault != 0U)
        {
            return -1;
        }

        /* Publish bytes and arm the consumer atomically; never wait with IRQs masked. */
        __disable_irq();
        while (written < length)
        {
            uint16_t head = g_tx_head;
            uint16_t next = (head + 1U) & USART_TX_RING_MASK;

            if (next == g_tx_tail)
            {
                break;
            }

            g_tx_ring[head] = (uint8_t)data[written++];
            __DMB();
            g_tx_head = next;
        }

        if (g_tx_dma_ready != 0U)
        {
            uart_tx_dma_start_locked();
        }
        else
        {
            USART_UX->CR1 |= USART_CR1_TXEIE_TXFNFIE;
        }
        __set_PRIMASK(primask);

        if (written < length)
        {
            /* Consumers publish __SEV() after every tail advance, so no release is missed. */
            __WFE();
        }
    }

    return (int)written;
}

int uart_log_flush(uint32_t timeout_ms)
{
    uint32_t start_tick;

    if ((g_usart_ready == 0U) || (__get_IPSR() != 0U) || (__get_PRIMASK() != 0U))
    {
        return -1;
    }

    start_tick = HAL_GetTick();
    for (;;)
    {
        uint8_t  empty;
        uint8_t  dma_active;
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        empty      = (g_tx_head == g_tx_tail) ? 1U : 0U;
        dma_active = g_tx_dma_active;
        if (g_tx_transport_fault != 0U)
        {
            __set_PRIMASK(primask);
            return -1;
        }
        if ((empty != 0U) && (dma_active == 0U) && ((USART_UX->ISR & USART_ISR_TC) != 0U))
        {
            __set_PRIMASK(primask);
            return FUNC_STATUS_SUCCESS;
        }
        __set_PRIMASK(primask);

        if ((timeout_ms != UART_LOG_FLUSH_FOREVER) && ((HAL_GetTick() - start_tick) >= timeout_ms))
        {
            g_uart_stats.tx_flush_timeouts++;
            return -1;
        }
        if ((empty == 0U) || (dma_active != 0U))
        {
            /* tail updates call __SEV(); TC-only completion is polled below. */
            __WFE();
        }
    }
}

int uart_log_printf(uart_log_level_t level, const char *format, ...)
{
    char    message[192];
    int     prefix_length;
    int     message_length;
    va_list args;

    if ((format == NULL) || (level > g_uart_log_level))
    {
        return FUNC_STATUS_SUCCESS;
    }

    prefix_length = snprintf(message, sizeof(message), "[%s] ", uart_log_level_name(level));
    if ((prefix_length < 0) || ((uint32_t)prefix_length >= sizeof(message)))
    {
        return FUNC_STATUS_SUCCESS;
    }

    va_start(args, format);
    message_length = vsnprintf(message + prefix_length, sizeof(message) - (uint32_t)prefix_length, format, args);
    va_end(args);

    if (message_length < 0)
    {
        return FUNC_STATUS_SUCCESS;
    }
    if ((uint32_t)message_length >= (sizeof(message) - (uint32_t)prefix_length))
    {
        message_length = (int)(sizeof(message) - (uint32_t)prefix_length - 1U);
    }

    return uart_log_write(message, (uint32_t)(prefix_length + message_length));
}

void uart_log_set_level(uart_log_level_t level)
{
    if (level <= UART_LOG_DEBUG)
    {
        g_uart_log_level = level;
    }
}

uart_log_level_t uart_log_get_level(void)
{
    return g_uart_log_level;
}

void uart_get_stats(uart_stats_t *stats)
{
    if (stats != NULL)
    {
        *stats = g_uart_stats;
    }
}

void uart_service_poll(void)
{
    uint8_t byte;

    while (uart_rx_dequeue(&byte))
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
