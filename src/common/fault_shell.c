/*
 * fault_shell.c — 断言/abort 之后的只读故障 shell（SSD_FAULT_ACTION_SHELL）。
 *
 * 终端顺序：assertion.c 已经做过 crash_platform_report（它会把 USART 切到
 * 轮询控制台、关掉日志 DMA/TXE）。因此这里：
 *   · 输出一律走 fault_port_putc（轮询），不碰异步日志环；
 *   · 查询函数产生的 LOG* 通过 log_set_sink 临时重定向到同一个 putc；
 *   · 只服务白名单命令，不恢复业务、不分配、不刷新看门狗。
 */

#include "fault_shell.h"

#include "crash.h"
#include "fault.h"
#include "ftl.h"
#include "log.h"
#include "pc_sample.h"
#include "stack_guard.h"
#include "usart.h"

#include <stdio.h>
#include <string.h>

#if (SSD_FAULT_ACTION == SSD_FAULT_ACTION_SHELL)

#    define SHELL_LINE_MAX 64U
#    define SHELL_PC_SHOW  32U

static char  s_line[SHELL_LINE_MAX];
static DWORD s_line_len;

/* 把任意日志/查询文本重定向到轮询控制台。 */
static DWORD fault_shell_sink(const char *message, DWORD length)
{
    if (message == NULL)
    {
        return 0U;
    }

    for (DWORD i = 0U; i < length; i++)
    {
        fault_port_putc(message[i]);
    }

    return length;
}

static void fault_shell_puts(const char *text)
{
    (void)fault_shell_sink(text, (DWORD)strlen(text));
}

static void fault_shell_prompt(void)
{
    fault_shell_puts("fault> ");
}

static void fault_shell_pc(void)
{
    char buffer[48];

    if (pc_sample_prev_valid() == false)
    {
        fault_shell_puts("pc: no previous-run samples\r\n");
        return;
    }

    const DWORD count = pc_sample_count(true);
    const DWORD show  = (count < SHELL_PC_SHOW) ? count : SHELL_PC_SHOW;

    (void)snprintf(buffer, sizeof(buffer), "pc: %lu samples (previous run, first %lu)\r\n", count, show);
    fault_shell_puts(buffer);

    for (DWORD i = 0U; i < show; i++)
    {
        (void)snprintf(buffer, sizeof(buffer), "  [%02lu] 0x%08lx\r\n", i, pc_sample_at(true, i));
        fault_shell_puts(buffer);
    }
}

static void fault_shell_help(void)
{
    fault_shell_puts("commands (read-only):\r\n");
    fault_shell_puts("  ftl_ctx [module]  alloc|pend|seq|cache|jrn|cp|mig|md|wb (default all)\r\n");
    fault_shell_puts("  crash             print the .noinit crash record\r\n");
    fault_shell_puts("  stack             stack guard paint/peak/alarms\r\n");
    fault_shell_puts("  pc                previous-run PC samples\r\n");
    fault_shell_puts("  help              this text\r\n");
}

static void fault_shell_exec(char *line)
{
    char *arg = strchr(line, ' ');

    if (arg != NULL)
    {
        *arg = '\0';
        arg++;
        while ((*arg == ' ') || (*arg == '\t'))
        {
            arg++;
        }
        if (*arg == '\0')
        {
            arg = NULL;
        }
    }

    if (line[0] == '\0')
    {
        return;
    }
    if (strcmp(line, "help") == 0)
    {
        fault_shell_help();
    }
    else if (strcmp(line, "ftl_ctx") == 0)
    {
        if (ftl_debug_dump(arg) == false)
        {
            fault_shell_puts("ftl_ctx: unknown module (alloc|pend|seq|cache|jrn|cp|mig|md|wb)\r\n");
        }
    }
    else if (strcmp(line, "crash") == 0)
    {
        crash_platform_report(crash_record_get());
    }
    else if (strcmp(line, "stack") == 0)
    {
        stack_guard_report();
    }
    else if (strcmp(line, "pc") == 0)
    {
        fault_shell_pc();
    }
    else
    {
        fault_shell_puts("unknown command; type help\r\n");
    }
}

_Noreturn void fault_shell_run(void)
{
    /* 测试镜像默认把项目日志设为 LOG_OFF；shell 里的查询走 LOGW/LOGI，
     * 不抬级别会被静默过滤。这里是事后现场，直接抬到 INFO。 */
    log_set_level(LOG_INFO);
    log_set_sink(fault_shell_sink);

    fault_shell_puts("\r\n=== FAULT SHELL (read-only) ===\r\n");
    fault_shell_help();
    fault_shell_prompt();

    for (;;)
    {
        BYTE byte = 0U;

        if (uart_rx_read(&byte) == 0U)
        {
            continue;
        }

        if ((byte == (BYTE)'\r') || (byte == (BYTE)'\n'))
        {
            fault_port_putc('\r');
            fault_port_putc('\n');
            s_line[s_line_len] = '\0';
            fault_shell_exec(s_line);
            s_line_len = 0U;
            fault_shell_prompt();
        }
        else if ((byte == 8U) || (byte == 127U))
        {
            if (s_line_len > 0U)
            {
                s_line_len--;
                fault_shell_puts("\b \b");
            }
        }
        else if ((byte >= 32U) && (byte < 127U) && (s_line_len < (SHELL_LINE_MAX - 1U)))
        {
            s_line[s_line_len] = (char)byte;
            s_line_len++;
            fault_port_putc((char)byte);
        }
        else
        {
            /* 其它控制字符忽略。 */
        }
    }
}

#else

_Noreturn void fault_shell_run(void)
{
    for (;;)
    {
    }
}

#endif /* SSD_FAULT_ACTION == SSD_FAULT_ACTION_SHELL */

/* 平台复位钩子：默认保持现场（死循环），由平台覆盖。 */
__attribute__((weak)) void fault_platform_reset(void)
{
    for (;;)
    {
    }
}
