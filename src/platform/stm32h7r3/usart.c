#include "common/common.h"
#include "usart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define USART_RX_RING_MASK     (USART_RX_RING_CAPACITY - 1U)
#define USART_TX_RING_MASK     (USART_TX_RING_CAPACITY - 1U)
#define UART_CLI_MAX_ARGUMENTS 8U
#define UART_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

#if ((USART_RX_RING_CAPACITY & USART_RX_RING_MASK) != 0U)
#    error "USART_RX_RING_CAPACITY must be a power of two"
#endif

#if ((USART_TX_RING_CAPACITY & USART_TX_RING_MASK) != 0U)
#    error "USART_TX_RING_CAPACITY must be a power of two"
#endif

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
static uint8_t           g_tx_ring[USART_TX_RING_CAPACITY];
static volatile uint16_t g_tx_head;
static volatile uint16_t g_tx_tail;

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
    return FUNC_STATUS_ERROR;
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
                          "uart: rx_ovf=%lu tx_drop=%lu ore=%lu fe=%lu ne=%lu pe=%lu\r\n",
                          (unsigned long)stats.rx_overflow,
                          (unsigned long)stats.tx_dropped,
                          (unsigned long)stats.overrun_errors,
                          (unsigned long)stats.framing_errors,
                          (unsigned long)stats.noise_errors,
                          (unsigned long)stats.parity_errors);
}

static int uart_cli_help(int argc, char *argv[]);
static int uart_cli_status(int argc, char *argv[]);
static int uart_cli_version(int argc, char *argv[]);
static int uart_cli_log(int argc, char *argv[]);

/*
 * 第一版只注册只读诊断和日志等级控制。危险操作应按产品生命周期、认证状态和权限
 * 单独注册，不能把 raw NAND 或任意内存访问默认暴露在量产 UART 上。
 */
static const uart_cli_command_t g_uart_cli_commands[] = {
    {   "help",                              "help",                  "show command list", 1U, 1U,    uart_cli_help},
    { "status",                            "status", "show UART error and queue counters", 1U, 1U,  uart_cli_status},
    {"version",                           "version",      "show firmware console version", 1U, 1U, uart_cli_version},
    {    "log", "log level <error|warn|info|debug>",              "set runtime log level", 3U, 3U,     uart_cli_log},
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
 */
int _write(int file, char *buffer, int length)
{
    (void)file;

    if ((buffer == NULL) || (length <= 0))
    {
        return FUNC_STATUS_SUCCESS;
    }

    return uart_log_write(buffer, (uint32_t)length);
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

    /*
     * HAL 仅负责时钟、GPIO、BRR 和基本帧格式初始化。从这里开始 RXNE/TXE 的所有权
     * 交给自定义 IRQ 处理器；不能再同时调用 HAL_UART_Receive_IT()，否则两个状态机
     * 会竞争 RDR 和中断使能位。
     */
    USART_UX->CR1 |= USART_CR1_RXNEIE_RXFNEIE | USART_CR1_PEIE;
    USART_UX->CR3 |= USART_CR3_EIE;
    g_usart_ready = 1U;
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
    rcc_periph_clk_init_struct.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
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

    if (((USART_UX->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0U) &&
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
    uint32_t primask;

    if ((!g_usart_ready) || (data == NULL) || (length == 0U))
    {
        return FUNC_STATUS_SUCCESS;
    }

    /*
     * 保护“生产者入队”和“打开 TXEIE”的原子交接：若 IRQ 恰好看到队列为空并关闭 TXEIE，
     * 主循环又在此刻只入队，会导致新日志无人触发发送、永久滞留。临界区内仅复制 RAM 和
     * 写一个控制寄存器，严禁加入等待外设或 printf 等可能拉长关中断时间的操作。
     */
    primask = __get_PRIMASK();
    __disable_irq();
    while (written < length)
    {
        uint16_t head = g_tx_head;
        uint16_t next = (head + 1U) & USART_TX_RING_MASK;

        if (next == g_tx_tail)
        {
            /* 日志不能反向阻塞存储 I/O；饱和时明确丢弃新字节而非等待串口发送。 */
            g_uart_stats.tx_dropped += length - written;
            break;
        }

        g_tx_ring[head] = (uint8_t)data[written++];
        __DMB();
        g_tx_head = next;
    }

    /* 置位 TXEIE 后，TX FIFO 有空位就会进入 IRQ 并异步抽干软件队列。 */
    if (written != 0U)
    {
        USART_UX->CR1 |= USART_CR1_TXEIE_TXFNFIE;
    }

    __set_PRIMASK(primask);

    return (int)written;
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
}
