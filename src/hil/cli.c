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

#include "assertion.h"
#include "bbt.h"
#include "crash.h"
#include "delay.h"        /* delay_now_ms（stat 的本次开机时间） */
#include "fc_interface.h" /* 池/队列深度等模块常量（内含 fc_interface.h） */
#include "fc_interface.h" /* fc_get_resource_stats（q 的队列/三池水位） */
#include "ftl.h"
#include "ftl_fc_interface.h" /* ftl_sync_read_page / FTL_PAGE_CFG_DATA_ANY（rdpaa 诊断）*/
#include "ftl_op.h"           /* ftl_op_req_count（q 的同步等待在飞数） */
#include "gc.h"               /* gc_read_page_with_lba（rdpaa 打印页内 META）*/
#include "l2p_cache.h"
#include "log.h"
#include "metadata.h"               /* md_pool_print（pool 诊断） */
#include "nand_device.h"            /* nand_get_timing_mode（通用器件查询） */
#include "pc_sample.h"              /* PC 采样环（pc 命令） */
#include "pending.h"                /* pend_slot/group_get_count（q） */
#include "power_failure_recovery.h" /* pfr_media_format（boot 失败后的恢复格式化） */
#include "runtime.h"                /* ssd_runtime_pump：CLI 同步等待命令自带泵 */
#include "ssd_request.h"
#include "stack_guard.h"
#include "usart.h"
#include "usb_msc.h"
#include "watchdog.h" /* storage_wdg_kick：长同步命令的有界喂狗 */
#include "wbuf.h"
#include "wear_leveling.h" /* wl_ec_spread_get / WL_THRESHOLD（wear 的 WL 判据） */

#include <errno.h>
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

/* uart_log_write 需要显式长度；字面量统一由本宏取 sizeof，避免手写长度与文本脱节
 * （H4/H6：改文本忘改长度 = 静默截断，写长 = 越界读）。只用于数组/字面量实参。 */
#define UART_CLI_WRITE(text) (void)uart_log_write((text), sizeof(text) - 1U)

/* 破坏性命令（format / bbt erase）默认只在 debug/bench 档暴露：它们会擦掉
 * 元数据与用户数据，并长时间占用主循环。量产档走 main.c 的自动恢复路径
 * （pfr_media_check_format 判定空片/版本不符）；若确需在 release 保留手动恢复，
 * 把本宏置 1。 */
#if defined(NDEBUG)
#    define UART_CLI_DESTRUCTIVE_ENABLE 0U
#else
#    define UART_CLI_DESTRUCTIVE_ENABLE 1U
#endif

/*
 * QWORD（host LBA）一律用 %llu；DWORD 用 %lu。完整 newlib 支持标准长度修饰符。
 * ssd_ctl_s.host_lba_max 仍是 DWORD（32-bit LBA × 512B = 2 TiB 上限），
 * 故仍用 _Static_assert 钉住 FTL_LBA_MAX 装得进 32 位。
 */
_Static_assert(FTL_LBA_MAX_AT_MAX <= 0xFFFFFFFFUL, "FTL_LBA_MAX 超出 32 位，host_lba_max 装不下");

/* 行编辑状态：g_cli_line 累积当前行（不含行尾 '\n'），g_cli_length 是已收字节数。
 * 行超过 USART_CLI_LINE_CAPACITY-1 时置 g_cli_discarding 并丢弃到下一个 '\n'
 * —— 否则截断后的半行会被当成一条完整命令执行。 */
static char g_cli_line[USART_CLI_LINE_CAPACITY];
static WORD g_cli_length;
static BYTE g_cli_discarding;

/*
 * 命令表把「命令字符串 -> 语法 -> 说明 -> 参数约束 -> 行为位 -> 处理函数」集中管理。
 * argc 包含命令名自身，例如 "log level debug" 被切分后 argc 为 3。
 *
 * handler 返回 void：失败原因由 handler 自己打印；dispatcher 只负责语法/策略，
 * 不做返回值分派（H38）。`test` 由 munit 适配层打印结果，返回码无消费者。
 */
typedef void (*uart_cli_handler_fn_t)(int argc, char *argv[]);

/* 命令分组：只决定 `help` 的分节输出，顺序即帮助顺序。 */
typedef enum UART_CLI_GROUP_E
{
    UART_CLI_GROUP_SYSTEM = 0,
    UART_CLI_GROUP_HOST_IO,
    UART_CLI_GROUP_DIAG,
    UART_CLI_GROUP_DESTRUCTIVE,
    UART_CLI_GROUP_TEST,
    UART_CLI_GROUP_COUNT
} uart_cli_group_e;

/*
 * 命令行为位（**本仓自拟**：U-Boot 的命令表只有 repeatable 一个行为位，
 * Zephyr 无对应字段；这里是因为调试 CLI 与 USB host 共用同一个 controller）。
 * `flags` 用 BYTE 存 ⇒ 结构体仍 20 B（新字段填掉原 padding）；判定走
 * UART_CLI_CMD_HAS_FLAG（H15：位语义由宏名表达）。
 */
#define UART_CLI_FLAG_NONE        0x00U
#define UART_CLI_FLAG_BUSY        0x01U /* 提交前要求 controller 无在飞 host 请求 */
#define UART_CLI_FLAG_DESTRUCTIVE 0x02U /* 量产档拒绝（当前 destructive 命令同时编译期隐藏） */

#define UART_CLI_CMD_HAS_FLAG(command, flag) (((command)->flags & (flag)) != 0U)

typedef struct UART_CLI_COMMAND_S
{
    const char           *name;
    const char           *syntax; /* <required> / [optional] / subcommand | subcommand */
    const char           *help;   /* 一行说明；选项只写在 syntax，不在这里 */
    BYTE                  min_argc;
    BYTE                  max_argc;
    BYTE                  flags; /* UART_CLI_FLAG_* 位图 */
    BYTE                  group; /* uart_cli_group_e；用 BYTE 存以保持结构体 20 B */
    uart_cli_handler_fn_t handler;
} uart_cli_cmd_s;

static void uart_cli_help(int argc, char *argv[]);
static void uart_cli_status(int argc, char *argv[]);
static void uart_cli_version(int argc, char *argv[]);
static void uart_cli_log(int argc, char *argv[]);
static void uart_cli_wr(int argc, char *argv[]);
static void uart_cli_rd(int argc, char *argv[]);
static void uart_cli_tr(int argc, char *argv[]);
static void uart_cli_fl(int argc, char *argv[]);
static void uart_cli_wp(int argc, char *argv[]); /* 页打包写缓冲合并诊断（§七 方案 A） */
static void uart_cli_info(int argc, char *argv[]);
static void uart_cli_map(int argc, char *argv[]);     /* L2P 映射 + 目标块 BST 条目（只读诊断） */
static void uart_cli_addr(int argc, char *argv[]);    /* PBA/PAA/gblock 地址字段转换（只读诊断） */
static void uart_cli_rdpaa(int argc, char *argv[]);   /* 按 PAA 读一个 NAND 页（诊断） */
static void uart_cli_crash(int argc, char *argv[]);   /* .noinit 崩溃记录查看/清除 */
static void uart_cli_pc(int argc, char *argv[]);      /* PC 采样热点（死循环定位） */
static void uart_cli_q(int argc, char *argv[]);       /* 队列 / 三池 / 背压水位 */
static void uart_cli_cp(int argc, char *argv[]);      /* Checkpoint job 现场 */
static void uart_cli_jrn(int argc, char *argv[]);     /* Journal 环 / WAL / 水位 */
static void uart_cli_ftl_ctx(int argc, char *argv[]); /* FTL 模块关键现场（可过滤） */
static void uart_cli_stat(int argc, char *argv[]);    /* 计数类诊断（从 info 拆出） */
static void uart_cli_id(int argc, char *argv[]);      /* 编译期配置 / 容量 / 介质版本 */
static void uart_cli_ecc(int argc, char *argv[]);     /* per-block ECC 健康（E1） */
static void uart_cli_wear(int argc, char *argv[]);    /* 擦除 / read-disturb / WL 差值 */
#if defined(FC_NAND_TEST_ENABLE)
static void uart_cli_test(int argc, char *argv[]);
#endif

/* 同步命令自带泵：在 handler 内推进后台服务，直到本命令完成。 */
static void uart_cli_wait_host(void);
static void uart_cli_wait_wp(void);

/**
 * @brief 输出 CLI 提示符
 */
static void uart_write_prompt(void)
{
    static const char prompt[] = "ssd> ";

    (void)uart_log_write(prompt, sizeof(prompt) - 1U);
}

/**
 * @brief 打印 UART 错误/队列计数
 */
static void uart_print_status(void)
{
    uart_stats_s stats;

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

/* 在飞组数：wp 绕过 CLI 的 pending gate（要在飞多组），但 g_wp_buf 的切片
 * 在**未下发的 run** 里仍被组持有 ⇒ 上一批没走完前不许再 wp。 */
static BYTE g_wp_inflight;

/* wr/rd/tr/fl 的 pending 记录：handler 提交后在本函数内用 ssd_runtime_pump
 * 自旋等待完成（同步语义），完成后由 uart_cli_pending_poll 打印并释放。 */
typedef struct UART_CLI_PENDING_S
{
    BYTE              busy;    /* 1=有未决 CLI 请求 */
    ssd_host_entry_s *entry;   /* HIL host queue entry（tag/owner/request） */
    BYTE              compare; /* rd: 1=compare 模式，0=hex dump */
    QWORD             lba;     /* 本次请求起始 LBA（结果打印用） */
    DWORD             nblocks; /* 本次请求块数 */
} uart_cli_pend_s;

static uart_cli_pend_s g_cli_pending;

/**
 * @brief CLI 是否有未决 host 请求（含 wp 直连 FTL 的在飞组）
 *
 * USB 前端用本谓词让路；wp 不经过 controller，必须单独计入，否则 USB 会在
 * wp 组仍持有 g_wp_buf 时提交 controller。
 *
 * @return true = g_cli_pending 有未决请求，或 wp 组仍在飞
 */
BOOL cli_host_busy(void)
{
    return (g_cli_pending.busy != 0U) || (g_wp_inflight != 0U);
}

/**
 * @brief 预填 g_wr_buf 的 0x55/0xAA pattern
 */
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

/**
 * @brief 取 compare 模式在给定偏移的期望 pattern 字
 *
 * @param byte_offset 字节偏移
 * @return 期望 32-bit 值
 */
static DWORD uart_cli_expect_word(DWORD byte_offset)
{
    return (byte_offset < (FTL_READ_USER_SIZE * CW_NUM)) ? 0x55555555U : 0xAAAAAAAAU;
}

/**
 * @brief 按 LBA 路由到所属 FTL 实例
 *
 * @param lba 起始 LBA
 * @return 实例指针；NULL = 未就绪
 */
static ftl_ctx_s *uart_cli_ftl_for_lba(QWORD lba)
{
    return g_ctrl.ftl[lba_2_die(lba)];
}

/**
 * @brief 解析十进制/十六进制数（CLI 的全部数值入口）。成功且全部消费返回 true。
 *
 * ⚠ 显式拒绝前导 '-'：strtoull 按标准接受它并回绕成 ULLONG_MAX，靠后续范围检查
 *   兜底只是碰巧——这里在入口就判死。ERANGE（超出 QWORD）同样拒绝，不静默取
 *   ULLONG_MAX。
 *
 * @param text 输入文本
 * @param out 解析结果出参
 * @return true = 合法且全部消费
 */
static BOOL uart_cli_parse_uint(const char *text, QWORD *out)
{
    char *end = NULL;
    QWORD value;

    if ((text == NULL) || (text[0] == '\0') || (text[0] == '-'))
    {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if ((end == NULL) || (*end != '\0') || (errno == ERANGE))
    {
        return false; /* 非法字符残留 / 数值溢出 */
    }
    if (out != NULL)
    {
        *out = value;
    }
    return true;
}

/**
 * @brief 解析 32-bit 地址
 *
 * @param text 输入文本
 * @param out 解析结果出参
 * @return true = 合法且未溢出 32 位
 */
static BOOL uart_cli_parse_dword(const char *text, DWORD *out)
{
    QWORD value;

    if (!uart_cli_parse_uint(text, &value) || (value > UINT32_MAX))
    {
        return false;
    }
    *out = (DWORD)value;
    return true;
}

/**
 * @brief wr/rd/tr/fl 的公共提交前导（唯一漏斗）：填 pending + 交给 controller。
 *
 * host 冲突策略不在这里：dispatcher 按 UART_CLI_FLAG_BUSY 单点执行。
 * 参数已由各命令校验；失败时回滚 busy 并打印统一错误。
 *
 * @param op 请求类型
 * @param lba 起始 LBA
 * @param count LBA 数
 * @param buffer 数据缓冲
 * @param buffer_size 缓冲容量
 * @param compare compare 模式标志
 * @return true = controller 已受理
 */
static BOOL uart_cli_host_submit(ssd_req_opcode_e op, QWORD lba, DWORD count, void *buffer, size_t buffer_size, BYTE compare)
{
    g_cli_pending.entry = ssd_host_queue_alloc(&g_ctrl.host_queue, SSD_HOST_OWNER_CLI);
    if (g_cli_pending.entry == NULL)
    {
        UART_CLI_WRITE("error: host queue full\r\n");
        return false;
    }

    g_cli_pending.busy    = 1U;
    g_cli_pending.compare = compare;
    g_cli_pending.lba     = lba;
    g_cli_pending.nblocks = count;

    ssd_req_s *request = &g_cli_pending.entry->request;

    memset(request, 0, sizeof(*request));
    request->opcode      = op;
    request->start_lba   = lba;
    request->block_count = count;
    request->buffer      = buffer;
    request->buffer_size = buffer_size;

    if (ssd_controller_submit(&g_ctrl, request) != SSD_STATUS_OK)
    {
        ssd_host_queue_free(&g_ctrl.host_queue, g_cli_pending.entry);
        g_cli_pending.entry = NULL;
        g_cli_pending.busy  = 0U;
        UART_CLI_WRITE("error: submit failed\r\n");
        return false;
    }
    return true;
}

/**
 * @brief wr 命令：按固定 pattern 写 n 个扇区
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_wr(int argc, char *argv[])
{
    QWORD lba;
    QWORD n_parsed;
    DWORD n;

    (void)argc;

    if ((!uart_cli_parse_uint(argv[1], &lba)) || (!uart_cli_parse_uint(argv[2], &n_parsed)) || (n_parsed == 0U) || (n_parsed > UINT32_MAX))
    {
        UART_CLI_WRITE("error: invalid lba/n\r\n");
        return;
    }
    n = (DWORD)n_parsed;
    if (n > UART_CLI_MAX_SECTORS)
    {
        (void)LOGE("error: n exceeds g_wr_buf (max %lu sectors)\r\n", (DWORD)UART_CLI_MAX_SECTORS);
        return;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, n))
    {
        UART_CLI_WRITE("error: lba range exceeds lba_max\r\n");
        return;
    }

    /* ⚠ `rd` 把读到的数据写进 g_wr_buf（同一块缓冲），会覆盖 pattern。
     * 若只在 init 里预填一次，`wr` 紧跟在 `rd` 之后时会把**读回的旧数据**当写源，
     * 表现为「写成功但内容不对」。每次 wr 重新预填，保证命令语义是「写固定 pattern」。 */
    uart_cli_pattern_init();

    if (uart_cli_host_submit(SSD_REQUEST_WRITE, lba, n, g_wr_buf, sizeof(g_wr_buf), 0U) == false)
    {
        return;
    }

    (void)LOGI("wr submit ok lba=%llu n=%lu, waiting...\r\n", lba, n);
    uart_cli_wait_host();
    return;
}

/**
 * @brief rd 命令：读 n 个扇区；--compare 校验 pattern
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_rd(int argc, char *argv[])
{
    QWORD lba;
    QWORD n_parsed;
    DWORD n;
    BYTE  compare = 0U;

    if ((!uart_cli_parse_uint(argv[1], &lba)) || (!uart_cli_parse_uint(argv[2], &n_parsed)) || (n_parsed == 0U) || (n_parsed > UINT32_MAX))
    {
        UART_CLI_WRITE("error: invalid lba/n\r\n");
        return;
    }
    n = (DWORD)n_parsed;
    if (n > UART_CLI_MAX_SECTORS)
    {
        (void)LOGE("error: n exceeds g_wr_buf (max %lu sectors)\r\n", (DWORD)UART_CLI_MAX_SECTORS);
        return;
    }

    /* 选项严格校验（同 bbt/crash/format）：拼错的 `--compare` 不能静默降级为 hex dump。 */
    if (argc >= 4)
    {
        if (strcmp(argv[3], "--compare") != 0)
        {
            UART_CLI_WRITE("error: unknown option (only --compare)\r\n");
            return;
        }
        compare = 1U;
    }

    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, n))
    {
        UART_CLI_WRITE("error: lba range exceeds lba_max\r\n");
        return;
    }

    /* 读数据覆盖 pattern；`wr` 每次会重新预填（见 uart_cli_wr），所以 rd→wr 相邻也安全。 */
    if (uart_cli_host_submit(SSD_REQUEST_READ, lba, n, g_wr_buf, sizeof(g_wr_buf), compare) == false)
    {
        return;
    }

    (void)LOGI("rd submit ok lba=%llu n=%lu%s, waiting...\r\n", lba, n, (compare != 0U) ? " --compare" : "");
    uart_cli_wait_host();
    return;
}

/**
 * @brief tr 命令：TRIM 一个 LBA
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_tr(int argc, char *argv[])
{
    QWORD lba;

    (void)argc;

    if (!uart_cli_parse_uint(argv[1], &lba))
    {
        UART_CLI_WRITE("error: invalid lba\r\n");
        return;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, 1U))
    {
        UART_CLI_WRITE("error: lba range exceeds lba_max\r\n");
        return;
    }
    if (uart_cli_host_submit(SSD_REQUEST_TRIM, lba, 1U, g_wr_buf, sizeof(g_wr_buf), 0U) == false)
    {
        return;
    }

    UART_CLI_WRITE("tr submit ok\r\n");
    uart_cli_wait_host();
    return;
}

/**
 * @brief wp 组完成回调：递减在飞计数
 *
 * @param ctx 回调上下文
 */
static void uart_cli_wp_complete(void *ctx)
{
    (void)ctx;

    if (g_wp_inflight > 0U)
    {
        g_wp_inflight--;
    }
}

/**
 * @brief wp <lba> <n> <k> — 页打包写缓冲合并诊断
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
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_wp(int argc, char *argv[])
{
    QWORD      lba;
    QWORD      n_parsed;
    QWORD      k_parsed;
    DWORD      n;
    DWORD      k;
    DWORD      i;
    DWORD      j;
    BOOL       failed = false;
    ftl_ctx_s *ftl;

    (void)argc;

    /* wp 只绕过「wp 自身」的 busy 闸门（要在飞多组），仍须让路给其他 CLI 请求
     * 与 USB host 请求；上一批 wp 未走完时 g_wp_buf 仍被未下发的 run 持有 ⇒
     * 拒绝。 */
    if (cli_host_busy() || usb_msc_host_busy())
    {
        UART_CLI_WRITE("error: busy (USB host request / previous wp in flight)\r\n");
        return;
    }

    if ((!uart_cli_parse_uint(argv[1], &lba)) || (!uart_cli_parse_uint(argv[2], &n_parsed)) || (!uart_cli_parse_uint(argv[3], &k_parsed)) || (n_parsed == 0U) || (k_parsed < 2U) || (n_parsed > UINT32_MAX) || (k_parsed > UINT32_MAX))
    {
        UART_CLI_WRITE("error: invalid lba/n/k (need n>=1, k>=2)\r\n");
        return;
    }
    n = (DWORD)n_parsed;
    k = (DWORD)k_parsed;
    if ((QWORD)n * (QWORD)k > FTL_LBA_PER_PAIR)
    {
        UART_CLI_WRITE("error: n*k must be <= 8 (one pair window)\r\n");
        return;
    }
    if ((lba & (FTL_LBA_PER_PAIR - 1U)) != 0U)
    {
        UART_CLI_WRITE("error: lba must be 8-aligned (pair window base)\r\n");
        return;
    }
    const DWORD sectors = n * k;

    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, sectors))
    {
        UART_CLI_WRITE("error: lba range exceeds lba_max\r\n");
        return;
    }

    /* 按窗口基址路由到所属实例（与 controller/FTL 的分片规则一致）。 */
    ftl = uart_cli_ftl_for_lba(lba);
    if (ftl == NULL)
    {
        UART_CLI_WRITE("error: ftl instance not ready\r\n");
        return;
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

    /* 逐组提交（不查 pending gate）：k 个组同时在飞 ⇒ 窗口填满时合并成 1 次编程。
     * 各组 complete_flag/status_out 指向静态数组，完成前始终有效；on_complete
     * 只递减在飞计数，保护 g_wp_buf。 */
    for (i = 0U; i < k; i++)
    {
        ftl_io_req_s req;

        memset(&req, 0, sizeof(req));
        req.lba             = lba + ((QWORD)i * (QWORD)n);
        req.buffer          = &g_wp_buf[(i * n) * FTL_IU_BYTES];
        req.count           = n;
        req.complete_flag   = &g_wp_done[i];
        req.status_out      = &g_wp_status[i];
        req.on_complete     = uart_cli_wp_complete;
        req.on_complete_ctx = NULL;

        g_wp_inflight++;
        if (ftl_submit_write(ftl, &req) != FTL_OK)
        {
            g_wp_inflight--; /* 本组未建组；此前各组仍会在完成时各自递减 */
            (void)LOGE("wp: group %lu submit failed\r\n", i);
            failed = true;
            break;
        }
    }

    /* 失败也要等已提交的组走完：它们仍持有 g_wp_buf 的切片。 */
    uart_cli_wait_wp();
    if (failed)
    {
        return;
    }

    (void)LOGI("wp done lba=%llu n=%lu k=%lu (expect 1 program on flush)\r\n", lba, n, k);
    return;
}

/**
 * @brief fl 命令：同步 cache / L2P / journal
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_fl(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (uart_cli_host_submit(SSD_REQUEST_FLUSH, 0U, 1U, g_wr_buf, sizeof(g_wr_buf), 0U) == false)
    {
        return;
    }

    UART_CLI_WRITE("fl submit ok\r\n");
    uart_cli_wait_host();
    return;
}

/* 控制器状态名：info 与 id 共用（同一映射只写一处）。 */
static const char *const g_controller_state_names[] = {"RESET", "INITIALIZING", "READY", "FAILED"};
_Static_assert(UART_ARRAY_SIZE(g_controller_state_names) == (SSD_CONTROLLER_FAILED + 1U), "控制器状态名表必须覆盖全部状态");

/**
 * @brief info 命令：控制器 + FTL 身份/状态
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_info(int argc, char *argv[])
{
    BYTE ch;
    BYTE ce;
    BYTE lun;

    (void)argc;
    (void)argv;

    if (g_ctrl.state <= SSD_CONTROLLER_FAILED)
    {
        (void)LOGI("controller state=%s lba_max=%lu\r\n", g_controller_state_names[g_ctrl.state], ssd_controller_host_lba_max(&g_ctrl));
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ch  = die_2_ch(die);
        ce  = die_2_ce(die);
        lun = die_2_lun(die);

        const ftl_ctx_s *ftl = g_ctrl.ftl[die];

        if (ftl == NULL)
        {
            continue;
        }

        BYTE                timing_mode = 0U;
        char                mode_text[4];
        const nand_result_e timing_result = nand_get_timing_mode(ch, ce, &timing_mode);

        if (timing_result == NAND_RESULT_OK)
        {
            (void)snprintf(mode_text, sizeof(mode_text), "%hhu", timing_mode);
        }
        else
        {
            (void)snprintf(mode_text, sizeof(mode_text), "?");
        }

        (void)LOGI("ftl[%hhu][%hhu][%hhu] mode=%s free=%hu op=%hu/%hu spare=%hu/%hu bad=%hu closed=%hu live=%hu thrash=%d miss=%hhu%% cp=%lu cpa=%d jrn=%lu wr=%lu\r\n", ch, ce, lun, mode_text, ftl->alloc.free_count, alc_op_get_count(&ftl->alloc, 0U), alc_op_get_count(&ftl->alloc, 1U), alc_spare_get_count(&ftl->alloc, 0U), alc_spare_get_count(&ftl->alloc, 1U), ftl->alloc.bad_count, ftl->alloc.closed_sb_count, md_live_block_count_get(ftl->metadata_dir), ftl->cache->thrash.thrashing, ftl->cache->thrash.miss_pct, ftl->cp_handle->cp_state.sequence, ftl->cp_handle->active, ftl->journal->handle.state.sequence_counter, ftl->stats.total_writes);

        /* 计数类诊断（见 `stat`）：info 只保留身份/状态，避免两套计数视图。 */

        /* 元数据整理 job 的现场（心跳默认档不打，靠这里观测）。
         * 只在有 job 在跑 / 有待回收块时多打一行，空闲时不刷屏。 */
        const md_dir_cache_s *md  = ftl->metadata_dir;
        const md_handle_s    *job = &md->handle;

        if ((job->phase != MD_JOB_IDLE) || (md->reclaim.count != 0U))
        {
            (void)LOGI("md[%hhu][%hhu][%hhu] phase=%d moved=%hu stall_armed=%d pending=%hu victim=%hu\r\n", ch, ce, lun, job->phase, job->moved, (int)job->submit_deadline.armed, md->reclaim.count, job->victim);
        }
    }

    return;
}

/**
 * @brief map <lba> — 只读诊断：打印该 LBA 的 L2P 映射与**目标块**的 BST 条目。
 *
 * 用途：block metadata 的 null-mask 必须带 plane 掩码；缺失时被日志块掩住的用户块
 * 进不了 Free Pool、也被 full metadata rebuild 跳过 ⇒ `valid_count` 恒 0，而 L2P 仍可能指向它
 * ⇒ 此后**每次**覆盖写该 LBA 都撞 `bst.h` 的 `b->valid_count > 0U` 断言。
 * 本命令把「L2P 指向谁」与「BST 怎么看那个块」摆在一起，用于在**不复现崩溃**的前提下
 * 判定现场是否已经悬空（末行 VERDICT）。
 *
 * ⚠ 判据：L2P 指向某块 ⇒ 至少该 CW 有效 ⇒ `valid_count ≥ 1` 是**不变量**；
 *   `valid_count == 0` 即悬空，与 `[FTL] dangling` 守卫同一判据。
 *   `state` 用于一次归因：BAD=坏块本不可读 / FREE=会被复用**丢数据** /
 *   LOG=判据过宽 / NONE=从未认领。
 *
 * 按 LBA 路由到所属实例（与 rd/wr 经 controller 路由到的实例一致）。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_map(int argc, char *argv[])
{
    ftl_ctx_s         *ftl;
    const bst_block_s *b;
    DWORD              pba;
    QWORD              lba;
    WORD               g;

    (void)argc;

    if (!uart_cli_parse_uint(argv[1], &lba))
    {
        UART_CLI_WRITE("error: invalid lba\r\n");
        return;
    }
    if (!ssd_controller_lba_range_ok(&g_ctrl, lba, 1U))
    {
        UART_CLI_WRITE("error: lba range exceeds lba_max\r\n");
        return;
    }

    ftl = uart_cli_ftl_for_lba(lba);
    if (ftl == NULL)
    {
        UART_CLI_WRITE("error: ftl instance not ready\r\n");
        return;
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
        return;
    }
    // L2P 条目是介质读回（H29）：非 INVALID 也要合法，否则下面的 BST 摘要会指向
    // 另一块、给出误导性结论（掩码字段保证下标不越界，但语义已错）。
    if (pba_is_valid(pba) == false)
    {
        (void)LOGE("map lba=%llu -> invalid PBA 0x%08lx (L2P entry corrupt)\r\n", lba, pba);
        return;
    }

    g = ftl_pba_gblock(pba);
    b = &ftl->alloc.blocks[g];
    (void)LOGI("map lba=%llu pba=%08lx paa=%08lx ch=%hhu ce=%hhu lun=%hhu plane=%hhu blk=%hu pg=%hhu cw=%hhu g=%hu\r\n", lba, pba, pba_to_paa(pba), pba_to_ch(pba), pba_to_ce(pba), pba_to_lun(pba), pba_to_plane(pba), pba_to_block(pba), pba_to_page(pba), pba_to_cw(pba), g);
    (void)LOGI("    block g=%hu state=%s valid=%hu sb=%hu ec=%lu\r\n", g, ftl_block_state_name(ftl_block_state(b)), b->valid_count, b->sb_id, b->erase_count);

    if (b->valid_count == 0U)
    {
        (void)LOGE("    VERDICT: 悬空 —— 覆盖写该 LBA 会撞 bst.h valid_count>0 断言\r\n");
    }
    else
    {
        (void)LOGI("    VERDICT: ok —— 覆盖写安全\r\n");
    }
    return;
}

/**
 * @brief addr — PBA/PAA/gblock 的只读地址转换。
 *
 *   addr pba <pba>                  PBA → PAA + ch/ce/lun/plane/block/page/cw + g
 *   addr paa <paa> [ch ce]          PAA → PBA + 展开字段（默认单 chip ch=ce=0）
 *   addr g <g> [ch ce lun]          实例内全局块号 → plane/block + BST 摘要
 *
 * LBA → 上述字段的入口是 `map <lba>`；这里不再提供 `addr lba`，避免两套入口（C9）。
 * 所有字段展开都复用 PBA/PAA getter 与 ftl_gblock helper，不手写位运算。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_addr(int argc, char *argv[])
{
    DWORD value;

    if (argc < 3)
    {
        (void)LOGE("usage: addr pba <pba> | paa <paa> [ch ce] | g <g> [ch ce lun]\r\n");
        return;
    }

    if (strcmp(argv[1], "pba") == 0)
    {
        if ((argc != 3) || (!uart_cli_parse_dword(argv[2], &value)) || (!pba_is_valid(value)))
        {
            (void)LOGE("addr: invalid pba\r\n");
            return;
        }

        (void)LOGI("addr pba=0x%08lx paa=0x%08lx ch=%hhu ce=%hhu lun=%hhu g=%hu plane=%hhu blk=%hu pg=%hhu cw=%hhu\r\n", value, pba_to_paa(value), pba_to_ch(value), pba_to_ce(value), pba_to_lun(value), ftl_pba_gblock(value), pba_to_plane(value), pba_to_block(value), pba_to_page(value), pba_to_cw(value));
        return;
    }

    if (strcmp(argv[1], "paa") == 0)
    {
        DWORD ch = 0U;
        DWORD ce = 0U;

        if ((argc != 3) && (argc != 5))
        {
            (void)LOGE("usage: addr paa <paa> [ch ce]\r\n");
            return;
        }
        if ((!uart_cli_parse_dword(argv[2], &value)) || (paa_is_valid_paa(value) == 0U))
        {
            (void)LOGE("addr: invalid paa\r\n");
            return;
        }
        if (argc == 5)
        {
            if ((!uart_cli_parse_dword(argv[3], &ch)) || (!uart_cli_parse_dword(argv[4], &ce)) || (ch >= CH_NUM) || (ce >= CE_NUM))
            {
                (void)LOGE("addr: invalid ch/ce\r\n");
                return;
            }
        }

        const DWORD pba = paa_to_pba(ch, ce, value);

        (void)LOGI("addr paa=0x%08lx pba=0x%08lx ch=%hhu ce=%hhu lun=%hhu g=%hu plane=%hhu blk=%hu pg=%hhu cw=%hhu\r\n", value, pba, pba_to_ch(pba), pba_to_ce(pba), pba_to_lun(pba), ftl_pba_gblock(pba), pba_to_plane(pba), pba_to_block(pba), pba_to_page(pba), pba_to_cw(pba));
        return;
    }

    if (strcmp(argv[1], "g") == 0)
    {
        DWORD ch  = 0U;
        DWORD ce  = 0U;
        DWORD lun = 0U;

        if ((argc != 3) && (argc != 6))
        {
            (void)LOGE("usage: addr g <g> [ch ce lun]\r\n");
            return;
        }
        if ((!uart_cli_parse_dword(argv[2], &value)) || (value >= FTL_BLOCK_TOTAL))
        {
            (void)LOGE("addr: invalid gblock\r\n");
            return;
        }
        if (argc == 6)
        {
            if ((!uart_cli_parse_dword(argv[3], &ch)) || (!uart_cli_parse_dword(argv[4], &ce)) || (!uart_cli_parse_dword(argv[5], &lun)) || (ch >= CH_NUM) || (ce >= CE_NUM) || (lun >= LUN_NUM))
            {
                (void)LOGE("addr: invalid ch/ce/lun\r\n");
                return;
            }
        }

        const BYTE         die = chcelun_2_die((BYTE)ch, (BYTE)ce, (BYTE)lun);
        const ftl_ctx_s   *ftl = ((die == FTL_DIE_INVALID) || (die >= DIE_MAX)) ? NULL : g_ctrl.ftl[die];
        const bst_block_s *b;

        if (ftl == NULL)
        {
            (void)LOGE("addr: ftl instance not ready\r\n");
            return;
        }
        b = &ftl->alloc.blocks[value];
        (void)LOGI("addr g=%lu plane=%hhu blk=%hu state=%s valid=%hu sb=%hu ec=%lu\r\n", value, ftl_gblock_plane((WORD)value), ftl_gblock_block((WORD)value), ftl_block_state_name(ftl_block_state(b)), b->valid_count, b->sb_id, b->erase_count);
        return;
    }

    (void)LOGE("usage: addr pba <pba> | paa <paa> [ch ce] | g <g> [ch ce lun]\r\n");
    return;
}

/**
 * @brief crash [clear] — 查看 / 清除 `.noinit` 崩溃记录。
 *
 * 记录放 `.noinit`（NOLOAD，启动不清零）⇒ **软复位后保留**，且当前没有任何路径会自动
 * 清除它 ⇒ 一次断言会被此后每次复位反复打印成 `[FAULT] previous assert: ...`，
 * 看上去像"还在崩"（实际只是同一条旧记录）。
 *
 * `crash clear` 给一个**显式归零点**：清完再复位，若记录重新出现，才是真的又崩了。
 * ⚠ 掉电（POR）本来就清 `.noinit` ⇒ 断电重启后看到的记录一定是新的、无需 clear。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_crash(int argc, char *argv[])
{
    const crash_record_s *rec;

    if (argc >= 2)
    {
        /* 子命令**严格**校验（同 bbt：静默忽略拼错的子参数会让人以为"清过了"） */
        if (strcmp(argv[1], "clear") != 0)
        {
            LOGW("crash: unknown subcommand '%s' (only clear)\r\n", argv[1]);
            return;
        }
        crash_record_clear();
        UART_CLI_WRITE("crash record cleared\r\n");
        return;
    }

    rec = crash_record_get();
    if (rec == NULL)
    {
        UART_CLI_WRITE("crash: no record\r\n");
        return;
    }

    (void)LOGI("crash kind=%lu line=%lu\r\n", rec->kind, rec->line);
    (void)LOGI("  expr: %s\r\n", rec->expression);
    (void)LOGI("  file: %s\r\n", rec->file);
    if (rec->kind == CRASH_KIND_FAULT)
    {
        (void)LOGI("  fault kind=%lu pc=%08lx lr=%08lx sp=%08lx xpsr=%08lx\r\n", rec->fault.fault_kind, rec->fault.pc, rec->fault.lr, rec->fault.sp, rec->fault.xpsr);
        (void)LOGI("  cfsr=%08lx hfsr=%08lx mmfar=%08lx bfar=%08lx\r\n", rec->fault.cfsr, rec->fault.hfsr, rec->fault.mmfar, rec->fault.bfar);
    }
    return;
}

/**
 * @brief stack — 打印栈哨兵状态（paint 区间 / 栈峰值 / 余量 / 告警）。纯只读，可反复调。
 *
 * `peak` 是「自 stack_guard_init 起，栈最深到过哪里」，`low` 是对应地址。
 * ⚠ `peak` 只在**堆没被用过**时才是栈峰值——堆从 `dtcm_bottom` 向上长，同样会抹掉
 *   pattern。所以报告把 `dtcm_bottom` 一并打出来：`low` 贴着它 ⇒ 那是堆顶不是栈底。
 * 告警只在**首次置位**时打过 LOGI/LOGE；`hits` 持续增长说明还在踩。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_stack(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    stack_guard_report();
    (void)LOGI("[STK ] heap_used=%lu B (heap+stack share DTCM)\r\n", syscalls_heap_used());
    return;
}

/* pc 的聚合视图 top-N；原始序列默认只打最近 64 个，`pc all` 打全部。 */
#define UART_CLI_PC_TOP      (8U)
#define UART_CLI_PC_RAW_TAIL (64U)

/**
 * @brief pc [live] [all | top] | pc clear | pc rate [n] — PC 采样（死循环 / 跑飞定位）。
 *
 * 默认打印**上一轮**（复位前）的原始 PC 序列（oldest→newest，4 个一行，最近 64 个）：
 * 一行行重复的地址就是循环；`all` 打完整 256 个，`top` 打命中次数排行。
 * `live` 看本轮；`clear` 归零后复现；`rate <n>` 设每 n 个 tick 采一次（窗口 256×n ms，
 * 持久化到 .noinit，复位后沿用；`pc rate 1` 恢复 1 ms）。
 * 反查必须用**产生该次运行的镜像**的 ELF/map（CLAUDE.md §3.3）。
 *
 * ⚠ 原始序列是**采样顺序**，不是指令顺序；紧凑循环（< rate ms 且整除）仍会混叠。
 * ⚠ 中断关闭 / 更高优先级 ISR 内的死循环采不到；.noinit 不跨 POR。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_pc(int argc, char *argv[])
{
    BOOL  previous = true;
    BOOL  show_all = false;
    BOOL  top_view = false;
    DWORD count;
    DWORD rate;
    DWORD span;

    if ((argc >= 2) && (strcmp(argv[1], "clear") == 0))
    {
        if (argc != 2)
        {
            UART_CLI_WRITE("usage: pc clear\r\n");
            return;
        }
        pc_sample_clear();
        UART_CLI_WRITE("pc samples cleared (rate kept)\r\n");
        return;
    }

    if ((argc >= 2) && (strcmp(argv[1], "rate") == 0))
    {
        DWORD new_rate;

        if (argc == 2)
        {
            rate = pc_sample_rate(false);
            (void)LOGI("pc: rate=%lu window=%lums\r\n", rate, rate * PC_SAMPLE_RING);
            return;
        }
        if ((argc != 3) || (uart_cli_parse_dword(argv[2], &new_rate) == false) || (pc_sample_rate_set(new_rate) == false))
        {
            (void)LOGE("pc: rate must be 1..%u\r\n", PC_SAMPLE_RATE_MAX);
            return;
        }
        (void)LOGI("pc: rate=%lu window=%lums (kept across reset; 'pc rate 1' restores)\r\n", new_rate, new_rate * PC_SAMPLE_RING);
        return;
    }

    if ((argc >= 2) && (strcmp(argv[1], "live") == 0))
    {
        previous = false;
        if (argc == 3)
        {
            if (strcmp(argv[2], "all") == 0)
            {
                show_all = true;
            }
            else if (strcmp(argv[2], "top") == 0)
            {
                top_view = true;
            }
            else
            {
                UART_CLI_WRITE("usage: pc [live] [all | top]\r\n");
                return;
            }
        }
        else if (argc > 3)
        {
            UART_CLI_WRITE("usage: pc [live] [all | top]\r\n");
            return;
        }
    }
    else if (argc == 2)
    {
        if (strcmp(argv[1], "all") == 0)
        {
            show_all = true;
        }
        else if (strcmp(argv[1], "top") == 0)
        {
            top_view = true;
        }
        else
        {
            UART_CLI_WRITE("usage: pc [live] [all | top] | pc clear | pc rate [n]\r\n");
            return;
        }
    }
    else if (argc > 2)
    {
        UART_CLI_WRITE("usage: pc [live] [all | top] | pc clear | pc rate [n]\r\n");
        return;
    }

    count = pc_sample_count(previous);
    rate  = pc_sample_rate(previous);
    span  = count * rate;

    if (count == 0U)
    {
        (void)LOGI("pc: no %s samples (clear, reproduce, reset, then run 'pc')\r\n", (previous == true) ? "previous-run" : "live");
        return;
    }

    if (top_view == true)
    {
        DWORD top_pc[UART_CLI_PC_TOP]  = {0U};
        DWORD top_cnt[UART_CLI_PC_TOP] = {0U};
        BYTE  top_n                    = 0U;
        DWORD i;

        // O(n^2) 的 top-N（n <= 256）：不占栈、不排序，CLI 诊断足够。
        for (i = 0U; i < count; i++)
        {
            const DWORD pc          = pc_sample_at(previous, i);
            DWORD       occurrences = 0U;
            BOOL        already_top = false;
            BYTE        k;

            for (k = 0U; k < top_n; k++)
            {
                if (top_pc[k] == pc)
                {
                    already_top = true;
                    break;
                }
            }
            if (already_top == true)
            {
                continue;
            }

            for (DWORD j = 0U; j < count; j++)
            {
                if (pc_sample_at(previous, j) == pc)
                {
                    occurrences++;
                }
            }

            if ((top_n < UART_CLI_PC_TOP) || (occurrences > top_cnt[UART_CLI_PC_TOP - 1U]))
            {
                BYTE pos = (top_n < UART_CLI_PC_TOP) ? top_n : (UART_CLI_PC_TOP - 1U);

                if (top_n < UART_CLI_PC_TOP)
                {
                    top_n++;
                }
                while ((pos > 0U) && (top_cnt[pos - 1U] < occurrences))
                {
                    top_cnt[pos] = top_cnt[pos - 1U];
                    top_pc[pos]  = top_pc[pos - 1U];
                    pos--;
                }
                top_cnt[pos] = occurrences;
                top_pc[pos]  = pc;
            }
        }

        (void)LOGI("pc[%s] samples=%lu rate=%lu span=%lums top=%hhu\r\n", (previous == true) ? "prev" : "live", count, rate, span, top_n);
        for (i = 0U; i < top_n; i++)
        {
            (void)LOGI("  pc=0x%08lx count=%lu share=%lu%%\r\n", top_pc[i], top_cnt[i], (top_cnt[i] * 100U) / count);
        }
    }
    else
    {
        const DWORD head  = pc_sample_head();
        const DWORD start = (show_all == true || count <= UART_CLI_PC_RAW_TAIL) ? 0U : (count - UART_CLI_PC_RAW_TAIL);
        DWORD       i;

        (void)LOGI("pc[%s] samples=%lu rate=%lu span=%lums raw %s (oldest -> newest)\r\n", (previous == true) ? "prev" : "live", count, rate, span, (show_all == true) ? "all" : "last 64");

        // 上一轮快照已在 init 时按时间顺序重排；本轮活跃环按 head 回绕。
        // 4 个地址一行；最后一行不足 4 个时补 0（不会与真实地址混淆的概率可忽略）。
        for (i = start; i < count; i += 4U)
        {
            DWORD addr[4] = {0U, 0U, 0U, 0U};

            for (BYTE k = 0U; (k < 4U) && ((i + k) < count); k++)
            {
                const DWORD chrono = i + k;
                const DWORD phys   = ((previous == true) || (count < PC_SAMPLE_RING)) ? chrono : ((head + chrono) & (PC_SAMPLE_RING - 1U));

                addr[k] = pc_sample_at(previous, phys);
            }
            (void)LOGI("  %03lu: 0x%08lx 0x%08lx 0x%08lx 0x%08lx\r\n", i, addr[0], addr[1], addr[2], addr[3]);
        }
    }

    UART_CLI_WRITE("resolve: arm-none-eabi-addr2line -f -C -i -e <same-elf> <pc>\r\n");
}

/**
 * @brief bbt [erase | fault <plane> <block>] — 逐块探测并打印坏块图。
 *
 * 默认**只读**：读每块 Page 0 的 spare 首字节（列 2048），非 0xFF ⇒ 出厂坏块。
 * 不写介质，可反复调用。
 *
 * 两个子命令都只在 debug/bench 档提供（release 的 usage 只列 `bbt`）：
 *   erase —— **破坏性**：每个实例的每个块都会被擦除（含元数据区）。
 *   fault —— 负例注入：把指定物理块标 BAD 并触发候选池重算，证明 BAD 槽被
 *            shared metadata spare 顶替后记录仍可定位；若该块持有活元数据会丢一份副本。
 *
 * 子命令严格校验：不认识就报错返回，绝不静默降级为只读。静默忽略曾两次把
 * `-erase` / `--rease`（拼错）当成合法擦除执行完、实际一条擦除命令都没发。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_bbt(int argc, char *argv[])
{
    BOOL do_erase = false;

    if (argc >= 2)
    {
#if (UART_CLI_DESTRUCTIVE_ENABLE != 0U)
        if (strcmp(argv[1], "erase") == 0)
        {
            if (argc != 2)
            {
                LOGE("usage: bbt erase\r\n");
                return;
            }
            do_erase = true;
            LOGW("bbt erase: erasing EVERY block of every instance (incl. metadata). Do not reset until done.\r\n");
        }
#    if defined(FC_NAND_TEST_ENABLE)
        else if (strcmp(argv[1], "fault") == 0)
        {
            if (argc != 4)
            {
                LOGE("usage: bbt fault <plane> <block>\r\n");
                return;
            }

            ftl_ctx_s *ftl = g_ctrl.ftl[0];

            if (ftl == NULL)
            {
                LOGE("bbt fault: no initialized FTL instance\r\n");
                return;
            }

            QWORD plane_q = 0U;
            QWORD block_q = 0U;

            if ((!uart_cli_parse_uint(argv[2], &plane_q)) || (!uart_cli_parse_uint(argv[3], &block_q)) || (plane_q >= PLANE_NUM) || (block_q >= BLOCKS_PER_PLANE))
            {
                LOGE("bbt fault: plane/block out of range\r\n");
                return;
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
            return;
        }
#    endif /* FC_NAND_TEST_ENABLE */
        else
        {
            LOGW("bbt: unknown subcommand '%s'\r\n", argv[1]);
            return;
        }
#else
        (void)LOGE("bbt: subcommand '%s' disabled in this build (erase/fault are debug-only)\r\n", argv[1]);
        return;
#endif
    }

    /* 全片擦除会抹掉元数据：host 有在飞请求时拒绝，避免它的完成回调落到
     * 正在擦/重写的介质上。 */
    if (do_erase == true)
    {
        if (cli_host_busy() || usb_msc_host_busy())
        {
            UART_CLI_WRITE("error: busy — wait for the host request to finish\r\n");
            return;
        }
    }

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_s *ftl = g_ctrl.ftl[die];

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
                wb_trigger_s trigger;
                wb_status_s  status;

                memset(&trigger, 0, sizeof(trigger));
                trigger.kind = WBUF_TRIGGER_RESET;
                (void)wb_handle(ftl->wbuf, &trigger, &status);
            }

            /* 全片擦除会抹掉 BBT 池：先等在飞的 BBT 异步写完成，避免它在擦净后
             * 又把一条 record 写回已擦空的块（同类重叠）。 */
            for (WORD spin = 0U; (ftl->alloc.bbt.handle.waiting == true) && (spin < 1000U); spin++)
            {
                ftl_service_pump();
            }
        }
        bbt_print_bad_map(&ftl->alloc, do_erase);
    }

    if (do_erase == true)
    {
        LOGW("bbt erase: done. POWER-CYCLE NOW — the running FTL rewrites metadata if left idle.\r\n");
    }

    return;
}

#if (UART_CLI_DESTRUCTIVE_ENABLE != 0U)
/**
 * @brief format — boot 失败后的恢复格式化（破坏性）
 *
 * 用途：CP/目录检查点介质状态无法被 pfr_media_check_format 判定为「空片/版本不符」
 * （例如上一轮 bbt erase 后仍有擦不掉的坏块残留数据），自动格式化会保守拒绝。
 * 本命令直接对已 ftl_init 的实例执行 pfr_media_format：重落 BBT（带
 * FORMAT_IN_PROGRESS）→ 擦除 BBT 池之外的块 → 写当前版本初始 Checkpoint。
 *
 * ⚠ 擦除全部用户数据与元数据；执行后必须断电重启，不能在运行中的 FTL 上继续用。
 * ⚠ 只在 debug/bench 档注册（`UART_CLI_DESTRUCTIVE_ENABLE`）；量产档走 main.c 的
 *   自动恢复路径。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_format(int argc, char *argv[])
{
    /* host 冲突由 dispatcher 的 UART_CLI_FLAG_BUSY 单点拒绝。 */
    BYTE ch;
    BYTE ce;
    BYTE lun;
    BYTE done = 0U;

    /* 可选的 --verify：对**出厂坏块标记已丢失的二手片**做一次全片擦/写/读体检，
     * 重建可信坏块表（耗时数分钟）。默认不做，避免例行 format 变慢。 */
    const BOOL verify = (argc >= 2) && (strcmp(argv[1], "--verify") == 0);

    if ((argc >= 2) && (!verify))
    {
        UART_CLI_WRITE("error: unknown option (only --verify)\r\n");
        return;
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

        ftl_ctx_s *ftl = g_ctrl.ftl[die];

        if (ftl == NULL)
        {
            continue;
        }

        /* 格式化期间禁止后台写；与 bbt erase 同口径。 */
        ftl->flags.bg_suspend = 1U;
        {
            wb_trigger_s trigger;
            wb_status_s  status;

            memset(&trigger, 0, sizeof(trigger));
            trigger.kind = WBUF_TRIGGER_RESET;
            (void)wb_handle(ftl->wbuf, &trigger, &status);
        }

        /* 格式化会重写 BBT 池：先等在飞的 BBT 异步写完成，避免 sync save 与它
         * 二次编程同一槽（bbt_save 也会拒绝这种重叠，这里把正常
         * 路径等干净，而不是让 format 直接失败）。泵是全局的，顺便推进其它实例。 */
        for (WORD spin = 0U; (ftl->alloc.bbt.handle.waiting == true) && (spin < 1000U); spin++)
        {
            ftl_service_pump();
        }

        if (pfr_media_format(ftl, verify) != FTL_OK)
        {
            LOGE("format: failed ch=%hhu ce=%hhu lun=%hhu\r\n", ch, ce, lun);
            return;
        }

        done = 1U;
        LOGI("format: done ch=%hhu ce=%hhu lun=%hhu\r\n", ch, ce, lun);
    }

    if (done == 0U)
    {
        LOGE("format: no initialized FTL instance\r\n");
        return;
    }

    LOGW("format: done. POWER-CYCLE NOW.\r\n");
    return;
}
#endif /* UART_CLI_DESTRUCTIVE_ENABLE */

/**
 * @brief pool — 打印固定元数据分区的候选块池（只读诊断）
 *
 * 用于验证 BBT + spare 候选池：nominal 块被标 BAD 后，候选数不变，
 * 坏块由 shared metadata spare 顶替（打印 remapped -> spare）。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_pool(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    BYTE done = 0U;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const BYTE ch  = die_2_ch(die);
        const BYTE ce  = die_2_ce(die);
        const BYTE lun = die_2_lun(die);

        const ftl_ctx_s *ftl = g_ctrl.ftl[die];

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
        return;
    }

    return;
}

/**
 * @brief q — 控制器队列 / 三池 / 背压水位（只读）。
 *
 * 对应 §26 的「队列 + 门铃 + 完成」：FC 的 SQ/HSQ/CQ 与三池是硬件 FCC 的软件替身，
 * FTL 的 pending 组/槽、L2P 在飞与 mig 在飞是上层背压。FC 统计是单例，
 * pending/在飞数按 FTL 实例分开打印。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_q(int argc, char *argv[])
{
    fc_resource_stats_s rs;

    (void)argc;
    (void)argv;

    fc_get_resource_stats(&rs);
    (void)LOGI("fc sq=%hu/%hu hsq=%hu/%hu cq=%hu/%hu cmd=%hu/%hu desc=%hu/%hu buf=%hu/%hu hwm=%hu/%hu/%hu\r\n", rs.sq_used, (WORD)FC_SQ_DEPTH, rs.hsq_used, (WORD)FC_HSQ_DEPTH, rs.cq_used, (WORD)FC_CQ_DEPTH, rs.command_used, (WORD)FC_COMMAND_POOL_DEPTH, rs.descriptor_used, (WORD)FC_DESCRIPTOR_POOL_DEPTH, rs.buffer_used, (WORD)FC_BUFFER_POOL_DEPTH, rs.command_high, rs.descriptor_high, rs.buffer_high);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl = g_ctrl.ftl[die];
        const BYTE       ch  = die_2_ch(die);
        const BYTE       ce  = die_2_ce(die);
        const BYTE       lun = die_2_lun(die);

        if (ftl == NULL)
        {
            continue;
        }

        (void)LOGI("q[%hhu][%hhu][%hhu] pend_slot=%hu pend_group=%hu ftl_op=%hu l2p_load=%hhu l2p_flush=%hhu mig=%hu wb_busy=%d\r\n", ch, ce, lun, pend_slot_get_count(&ftl->pending), pend_group_get_count(&ftl->pending), ftl_op_req_count(), ftl->cache->inflight.load, ftl->cache->inflight.flush, ftl->mig_handle->state.inflight, (int)(wb_is_idle(ftl->wbuf) == false));
    }
}

/**
 * @brief cp — Checkpoint 异步 job 的现场（只读）。
 *
 * 第一行是状态机（phase / 等待 / 失败 / 超时 / 心跳年龄），第二行是池游标与
 * 水位（sequence 是 CP 池自己的序号，与 journal sequence 互不相干）。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_cp(int argc, char *argv[])
{
    const DWORD now = delay_now_ms();

    (void)argc;
    (void)argv;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s   *ftl = g_ctrl.ftl[die];
        const cp_handle_s *cp;
        BYTE               ch;
        BYTE               ce;
        BYTE               lun;

        if (ftl == NULL)
        {
            continue;
        }
        cp  = ftl->cp_handle;
        ch  = die_2_ch(die);
        ce  = die_2_ce(die);
        lun = die_2_lun(die);

        (void)LOGI("cp[%hhu][%hhu][%hhu] active=%d phase=%d wait=%d fail=%d/%d timed=%d/%d age=%lums\r\n", ch, ce, lun, (int)cp->active, (int)cp->phase, (int)cp->waiting, (int)cp->failed, (int)cp->fail_phase, (int)cp->timed_out, (int)cp->timeout_phase, log_heartbeat_age_ms(&cp->hb));
        (void)LOGI("cp[%hhu][%hhu][%hhu] seq=%lu widx=%hu cursor=%d adv_next=%d jrn=%lu last_writes=%lu t3=%lu last_cp=%lu now=%lu\r\n", ch, ce, lun, cp->cp_state.sequence, cp->cp_state.write_idx, (int)cp->cp_state.cursor_valid, (int)cp->cp_state.advance_next, cp->cp_state.last_journal_sequence, cp->cp_state.last_cp_writes, cp->cp_state.next_t3_ms, cp->cp_state.last_cp_ms, now);
    }
}

/**
 * @brief jrn — Journal 环形 / WAL / 水位（只读）。
 *
 * 三行：写游标与水位；job 状态与环状态；坏块策略与持久性失败观测。
 * `persisted` 是 WAL 闸门用的已落盘上界，`wpseq/wentry` 是
 * 页缓冲里尚未落盘的一页。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_jrn(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s   *ftl = g_ctrl.ftl[die];
        const l2p_jrn_s   *jrn;
        const jrn_state_s *state;
        const jrn_job_s   *job;
        BYTE               ch;
        BYTE               ce;
        BYTE               lun;

        if (ftl == NULL)
        {
            continue;
        }
        jrn   = ftl->journal;
        state = &jrn->handle.state;
        job   = &jrn->handle.job;
        ch    = die_2_ch(die);
        ce    = die_2_ce(die);
        lun   = die_2_lun(die);

        (void)LOGI("jrn[%hhu][%hhu][%hhu] water=%hhu seq=%lu persisted=%lu pages=%lu page_dirty=%d wal_seq=%lu wal_entry=%hu\r\n", ch, ce, lun, jrn_watermark_get(jrn), jrn_sequence_get(jrn), jrn_persisted_seq(jrn), jrn_page_writes(jrn), (int)jrn_has_pending_entries(jrn), state->write_page_seq, state->write_entry_count);
        (void)LOGI("jrn[%hhu][%hhu][%hhu] cur=%hhu/%hhu/%hu kind=%d phase=%d erase_pend=%d ring_full=%d ckpt=%d emerg=%d wfail=%d\r\n", ch, ce, lun, state->current_block, state->current_page, state->current_entry, (int)state->page_kind, (int)job->phase, (int)jrn_has_erase_pending(jrn), (int)state->ring_full, (int)jrn_needs_checkpoint(jrn), (int)jrn_is_emergency(jrn), (int)jrn_has_write_failure(jrn));
        (void)LOGI("jrn[%hhu][%hhu][%hhu] bad_mask=0x%02hhx erase_fail=%hhu/%hhu wfail_cnt=%lu wfail_seq=%lu merge_floor=%lu pre_erased=%hhu\r\n", ch, ce, lun, state->bad_mask, state->erase_fail_count, state->erase_fail_block, state->write_fail_count, state->write_fail_seq, state->merge_floor, state->pre_erased_block);
    }
}

/**
 * @brief ftl_ctx [module] — 打印 FTL 各模块关键现场（只读）。
 *
 * 不带参数 = 全部；带缩写只打印对应模块：
 *   alloc / pend / seq / cache / jrn / cp / mig / md / wb。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_ftl_ctx(int argc, char *argv[])
{
    const char *module = (argc >= 2) ? argv[1] : NULL;

    if (ftl_debug_dump(module) == false)
    {
        (void)LOGI("error: unknown FTL module '%s' (alloc|pend|seq|cache|jrn|cp|mig|md|wb)\r\n",
                   (module != NULL) ? module : "(null)");
    }
}

/**
 * @brief stat — 计数类诊断（只读）。
 *
 * 从 `info` 拆出：info 只保留身份/状态（控制器、几何、分配器、CP/Journal 序号），
 * 计数与历史（GC/WL、预取、写回、CMT、USB 直方图）统一在这里看，避免两套视图。
 * uptime 是**本次开机**时间；跨 POR 的累计值需要额外持久记录，当前不做。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_stat(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    (void)LOGI("stat up=%lus\r\n", delay_now_ms() / 1000U);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl = g_ctrl.ftl[die];
        const BYTE       ch  = die_2_ch(die);
        const BYTE       ce  = die_2_ce(die);
        const BYTE       lun = die_2_lun(die);

        if (ftl == NULL)
        {
            continue;
        }

        (void)LOGI("diag[%hhu][%hhu][%hhu] dng=%lu rrab=%lu mig=%u/%u rec=%lu mv=%lu nop=%lu nov=%lu erased=%hu streak=%hhu gcm=%d ng=%lu fpmin=%hu\r\n", ch, ce, lun, ftl->stats.dangling_total, ftl->read_recovery_handle.already_bad_reads, ((ftl->mig_handle->job.active != 0U) ? 1U : 0U), ((ftl->mig_handle->state.failure.paused != 0U) ? 1U : 0U), ftl->mig_handle->state.counters.reclaimed, ftl->mig_handle->state.counters.moved, ftl->mig_handle->state.counters.noop, ftl->mig_handle->state.counters.novictim, ftl->mig_handle->job.erased, ftl->mig_handle->state.failure.read_fail_streak, ftl->mig_handle->state.benefit.last_mode, ftl->mig_handle->state.counters.nogain, alc_free_min_plane_count(&ftl->alloc));
        /* 顺序读预取 / cache read 观测（只读计数，不逐事件打日志）。
         * 拆两行：单行最坏值会超过 log_vprintf 的 192 B 缓冲。 */
        (void)LOGI("l2p[%hhu][%hhu][%hhu] seq=%lu/%lu pf=%lu/%lu/%lu/%lu/%lu cr=%lu/%lu/%lu/%lu/%lu\r\n", ch, ce, lun, ftl->cache_stream.prefetch_run, ftl->cache_stream.prefetch_run_max, ftl->cache->prefetch.calls, ftl->cache->prefetch.started, ftl->cache->prefetch.skipped, ftl->cache->prefetch.resident, ftl->cache->prefetch.hits, ftl->cache_stream.read_batches, ftl->cache_stream.read_pairs, ftl->cache_stream.read_starts, ftl->cache_stream.read_declines, ftl->cache_stream.read_fallbacks);
        (void)LOGI("l2p[%hhu][%hhu][%hhu] cw=%lu/%lu/%lu/%lu sw=%lu/%lu pc=%lu jf=%lu mb=%lu/%lu/%lu d4=%lu wd=%lu\r\n", ch, ce, lun, ftl->cache_stream.wb_batches, ftl->cache_stream.wb_pages, ftl->cache_stream.wb_declines, ftl->cache_stream.wb_aborts, ftl->cache_stream.wb_starts, ftl->cache_stream.wb_page_ends, ftl->cache_stream.wb_pad_closes, jrn_page_writes(ftl->journal), ftl->mig_handle->state.counters.batch_jobs, ftl->mig_handle->state.counters.batch_pages, ftl->mig_handle->state.counters.batch_aborts, ftl->cache_stream.seq_breaks, ftl->cache->stats.wal_defer_count);

        /* entry 级 CMT：读/写命中率 + dirty 写回压力。 */
        (void)LOGI("l2p[%hhu][%hhu][%hhu] cmt hit=%lu/%lu wr=%lu/%lu dirty=%lu wb=%lu ev=%lu/%lu defer=%lu\r\n", ch, ce, lun, ftl->cache->stats.cmt_hits, ftl->cache->stats.cmt_misses, ftl->cache->stats.cmt_wr_hits, ftl->cache->stats.cmt_wr_misses, ftl->cache->stats.cmt_dirty, ftl->cache->stats.cmt_writebacks, ftl->cache->stats.cmt_evict_clean, ftl->cache->stats.cmt_evict_dirty, ftl->cache->stats.cmt_wb_defer);
        (void)LOGI("l2p[%hhu][%hhu][%hhu] line hint=%lu/%lu\r\n", ch, ce, lun, ftl->cache->stats.hint_hits, ftl->cache->stats.hint_misses);
    }

    usb_msc_io_stats_print(); /* WRITE(10) 分块直方图 */
}

/**
 * @brief id — 编译期配置 / 容量 / 介质版本（只读，无 NAND 访问）。
 *
 * 与 `info` 的分工：`id` 是静态身份（构建配置 + 运行时 die 数 + 介质布局版本），
 * `info` 是实时状态。ONFI 参数页未缓存，boot 已打印过一次，这里不重读
 * （避免在 host 在飞时插 NAND 同步读）。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_id(int argc, char *argv[])
{
    const QWORD host_mib = FTL_HOST_CAPACITY_BYTES / (1024U * 1024U);
    const char *state_name;

    (void)argc;
    (void)argv;

    state_name = (g_ctrl.state <= SSD_CONTROLLER_FAILED) ? g_controller_state_names[g_ctrl.state] : "?";

    (void)LOGI("id geometry ch=%hu ce=%hu lun=%hu dies=%hhu plane=%hhu blk/plane=%hu pages/blk=%hu cw=%hu iu=%hu\r\n", (WORD)CH_NUM, (WORD)CE_NUM, (WORD)LUN_NUM, DIE_MAX, (WORD)PLANE_NUM, (WORD)BLOCKS_PER_PLANE, (WORD)PAGES_PER_BLOCK, (WORD)CW_NUM, (WORD)FTL_IU_BYTES);
    (void)LOGI("id capacity lba=%llu iu_total=%llu host_mib=%llu\r\n", FTL_LBA_MAX, FTL_IU_TOTAL, host_mib);
    (void)LOGI("id ftl pair=%hhu blk_total=%hu group_max=%hu op_pool=%hu spare/plane=%hu cmt=%hu\r\n", (BYTE)FTL_LBA_PER_PAIR, (WORD)FTL_BLOCK_TOTAL, (WORD)FTL_GROUP_MAX, (WORD)FTL_OP_POOL_CAP, (WORD)FTL_SPARE_POOL_PER_PLANE, (WORD)L2P_CMT_SIZE);

    BYTE                timing_mode = 0U;
    char                mode_text[4];
    const nand_result_e timing_result = nand_get_timing_mode(0U, 0U, &timing_mode);

    if (timing_result == NAND_RESULT_OK)
    {
        (void)snprintf(mode_text, sizeof(mode_text), "%hhu", timing_mode);
    }
    else
    {
        (void)snprintf(mode_text, sizeof(mode_text), "?");
    }

    (void)LOGI("id media layout=v%lu timing=%s state=%s\r\n", ftl_media_layout_version(), mode_text, state_name);
}

/* ECC last-ecc 直方图分档：0/1/2/3/4/5/>=6（6 = RR_ECC_HIGH_WATER）。 */
#define UART_CLI_ECC_BINS (7U)
#define UART_CLI_ECC_TOP  (4U)

/**
 * @brief ecc — per-block ECC 健康（只读，E1 口径）。
 *
 * 数据源是 read-recovery 已经维护的 `bst_block_s.rr`：`last_ecc` 是**最近一次读**
 * 的最大纠正 bit 数（host 读与 GC 读都经 rr_record_ecc 更新），不是终身累计；
 * 开机后没读过的块 `read_count == 0`，用 read_blocks 表示覆盖率。
 * 终身累计（纠正 bit 总和 / UNC 事件）属 E2，需新增 ftl_stats 计数，本轮不做。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_ecc(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl                               = g_ctrl.ftl[die];
        const BYTE       ch                                = die_2_ch(die);
        const BYTE       ce                                = die_2_ce(die);
        const BYTE       lun                               = die_2_lun(die);
        WORD             bins[UART_CLI_ECC_BINS]           = {0U};
        WORD             health_count[RR_BLOCK_DYING + 1U] = {0U};
        WORD             read_blocks                       = 0U;
        WORD             rising                            = 0U;
        WORD             high                              = 0U;
        BYTE             last_max                          = 0U;
        WORD             top_g[UART_CLI_ECC_TOP]           = {0U};
        BYTE             top_ecc[UART_CLI_ECC_TOP]         = {0U};
        BYTE             top_rise[UART_CLI_ECC_TOP]        = {0U};
        BYTE             top_health[UART_CLI_ECC_TOP]      = {0U};
        WORD             top_rc[UART_CLI_ECC_TOP]          = {0U};
        BYTE             top_n                             = 0U;
        char             line[128];
        DWORD            used = 0U;
        BYTE             i;

        if (ftl == NULL)
        {
            continue;
        }

        for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
        {
            const bst_block_s      *b      = &ftl->alloc.blocks[g];
            const rr_block_health_e health = rr_get_health(&b->rr);
            BYTE                    hidx   = (BYTE)health;
            BYTE                    bin;

            if (hidx > (BYTE)RR_BLOCK_DYING)
            {
                hidx = (BYTE)RR_BLOCK_DYING;
            }
            health_count[hidx]++;

            bin = (b->rr.last_ecc < (UART_CLI_ECC_BINS - 1U)) ? b->rr.last_ecc : (UART_CLI_ECC_BINS - 1U);
            bins[bin]++;

            if (b->read_count != 0U)
            {
                read_blocks++;
            }
            if (b->rr.last_ecc > last_max)
            {
                last_max = b->rr.last_ecc;
            }
            if (b->rr.last_ecc >= RR_ECC_HIGH_WATER)
            {
                high++;
            }
            if (b->rr.rising_count != 0U)
            {
                rising++;
            }

            if ((top_n < UART_CLI_ECC_TOP) || (b->rr.last_ecc > top_ecc[UART_CLI_ECC_TOP - 1U]))
            {
                BYTE pos = (top_n < UART_CLI_ECC_TOP) ? top_n : (UART_CLI_ECC_TOP - 1U);

                if (top_n < UART_CLI_ECC_TOP)
                {
                    top_n++;
                }
                while ((pos > 0U) && (top_ecc[pos - 1U] < b->rr.last_ecc))
                {
                    top_ecc[pos]    = top_ecc[pos - 1U];
                    top_rise[pos]   = top_rise[pos - 1U];
                    top_health[pos] = top_health[pos - 1U];
                    top_rc[pos]     = top_rc[pos - 1U];
                    top_g[pos]      = top_g[pos - 1U];
                    pos--;
                }
                top_ecc[pos]    = b->rr.last_ecc;
                top_rise[pos]   = b->rr.rising_count;
                top_health[pos] = hidx;
                top_rc[pos]     = b->read_count;
                top_g[pos]      = g;
            }
        }

        (void)LOGI("ecc[%hhu][%hhu][%hhu] read_blocks=%hu/%hu last_max=%hhu high=%hu rising=%hu\r\n", ch, ce, lun, read_blocks, (WORD)FTL_BLOCK_TOTAL, last_max, high, rising);
        (void)LOGI("ecc[%hhu][%hhu][%hhu] bins 0=%hu 1=%hu 2=%hu 3=%hu 4=%hu 5=%hu 6+=%hu\r\n", ch, ce, lun, bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6]);
        (void)LOGI("ecc[%hhu][%hhu][%hhu] health ok=%hu warn=%hu crit=%hu dying=%hu\r\n", ch, ce, lun, health_count[0], health_count[1], health_count[2], health_count[3]);

        line[0] = '\0';
        for (i = 0U; i < top_n; i++)
        {
            const int written = snprintf(&line[used], sizeof(line) - used, "%s%hu/%hhu/%hhu/%hhu/%hu", (i == 0U) ? "" : " ", top_g[i], top_ecc[i], top_rise[i], top_health[i], top_rc[i]);

            if ((written < 0) || ((DWORD)written >= (sizeof(line) - used)))
            {
                break;
            }
            used += (DWORD)written;
        }
        if (top_n == 0U)
        {
            (void)snprintf(line, sizeof(line), "none");
        }
        (void)LOGI("ecc[%hhu][%hhu][%hhu] top g/ecc/rise/health/rc: %s\r\n", ch, ce, lun, line);
    }
}

/* wear 的 EC 分档上边界（本仓自选的展示分档；WL_THRESHOLD=200 落在 200..499 档）。 */
#define UART_CLI_WEAR_BINS (7U)
#define UART_CLI_WEAR_TOP  (4U)

/**
 * @brief wear — 擦除计数 / read-disturb / WL 触发差值（只读）。
 *
 * 统计排除 BAD 块（WL 判据也不看 BAD）；固定元数据块（state=NONE）仍计入磨损。
 * WL 差值不在这里重算：`wl_ec_spread_get` 与 `wl_should_trigger` 是同一漏斗（C9），
 * 前者只给数值，后者给判定。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_wear(int argc, char *argv[])
{
    static const DWORD wear_edges[UART_CLI_WEAR_BINS - 1U] = {10U, 50U, 100U, 200U, 500U, 1000U};

    (void)argc;
    (void)argv;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        const ftl_ctx_s *ftl                        = g_ctrl.ftl[die];
        const BYTE       ch                         = die_2_ch(die);
        const BYTE       ce                         = die_2_ce(die);
        const BYTE       lun                        = die_2_lun(die);
        WORD             bins[UART_CLI_WEAR_BINS]   = {0U};
        WORD             good                       = 0U;
        WORD             read_max                   = 0U;
        WORD             rd_high                    = 0U;
        DWORD            ec_sum                     = 0U;
        DWORD            ec_min                     = UINT32_MAX;
        DWORD            ec_max                     = 0U;
        WORD             top_g[UART_CLI_WEAR_TOP]   = {0U};
        DWORD            top_ec[UART_CLI_WEAR_TOP]  = {0U};
        WORD             top_rc[UART_CLI_WEAR_TOP]  = {0U};
        BYTE             top_ecc[UART_CLI_WEAR_TOP] = {0U};
        BYTE             top_n                      = 0U;
        DWORD            max_free                   = 0U;
        DWORD            min_used                   = 0U;
        BOOL             wl_valid;
        BOOL             wl_trig;
        DWORD            spread;
        char             line[128];
        DWORD            used = 0U;
        BYTE             i;

        if (ftl == NULL)
        {
            continue;
        }

        wl_valid = wl_ec_spread_get(&ftl->alloc, ftl->stats.total_writes, &max_free, &min_used);
        wl_trig  = wl_should_trigger(&ftl->alloc, ftl->stats.total_writes);
        spread   = (wl_valid && (max_free > min_used)) ? (max_free - min_used) : 0U;

        for (WORD g = 0U; g < FTL_BLOCK_TOTAL; g++)
        {
            const bst_block_s *b = &ftl->alloc.blocks[g];
            BYTE               bin;

            if (ftl_block_state(b) == FTL_BLOCK_STATE_BAD)
            {
                continue;
            }
            good++;
            ec_sum += b->erase_count;
            if (b->erase_count < ec_min)
            {
                ec_min = b->erase_count;
            }
            if (b->erase_count > ec_max)
            {
                ec_max = b->erase_count;
            }
            bin = 0U;
            while ((bin < (UART_CLI_WEAR_BINS - 1U)) && (b->erase_count >= wear_edges[bin]))
            {
                bin++;
            }
            bins[bin]++;

            if (b->read_count > read_max)
            {
                read_max = b->read_count;
            }
            if (b->read_count >= RR_READ_DISTURB_THRESHOLD)
            {
                rd_high++;
            }

            if ((top_n < UART_CLI_WEAR_TOP) || (b->erase_count > top_ec[UART_CLI_WEAR_TOP - 1U]))
            {
                BYTE pos = (top_n < UART_CLI_WEAR_TOP) ? top_n : (UART_CLI_WEAR_TOP - 1U);

                if (top_n < UART_CLI_WEAR_TOP)
                {
                    top_n++;
                }
                while ((pos > 0U) && (top_ec[pos - 1U] < b->erase_count))
                {
                    top_ec[pos]  = top_ec[pos - 1U];
                    top_rc[pos]  = top_rc[pos - 1U];
                    top_ecc[pos] = top_ecc[pos - 1U];
                    top_g[pos]   = top_g[pos - 1U];
                    pos--;
                }
                top_ec[pos]  = b->erase_count;
                top_rc[pos]  = b->read_count;
                top_ecc[pos] = b->rr.last_ecc;
                top_g[pos]   = g;
            }
        }

        const DWORD ec_avg = (good != 0U) ? (ec_sum / good) : 0U;

        (void)LOGI("wear[%hhu][%hhu][%hhu] good=%hu ec min/max/avg=%lu/%lu/%lu read_max=%hu rd_high=%hu writes=%lu\r\n", ch, ce, lun, good, (ec_min == UINT32_MAX) ? 0U : ec_min, ec_max, ec_avg, read_max, rd_high, ftl->stats.total_writes);
        (void)LOGI("wear[%hhu][%hhu][%hhu] ec [0,10)=%hu [10,50)=%hu [50,100)=%hu [100,200)=%hu [200,500)=%hu [500,1000)=%hu [1000+)=%hu\r\n", ch, ce, lun, bins[0], bins[1], bins[2], bins[3], bins[4], bins[5], bins[6]);
        (void)LOGI("wear[%hhu][%hhu][%hhu] wl_trig=%d valid=%d max_free=%lu min_used=%lu spread=%lu thr=%u\r\n", ch, ce, lun, (int)wl_trig, (int)wl_valid, max_free, min_used, spread, WL_THRESHOLD);

        line[0] = '\0';
        for (i = 0U; i < top_n; i++)
        {
            const int written = snprintf(&line[used], sizeof(line) - used, "%s%hu/%lu/%hu/%hhu", (i == 0U) ? "" : " ", top_g[i], top_ec[i], top_rc[i], top_ecc[i]);

            if ((written < 0) || ((DWORD)written >= (sizeof(line) - used)))
            {
                break;
            }
            used += (DWORD)written;
        }
        if (top_n == 0U)
        {
            (void)snprintf(line, sizeof(line), "none");
        }
        (void)LOGI("wear[%hhu][%hhu][%hhu] hot g/ec/rc/ecc: %s\r\n", ch, ce, lun, line);
    }
}

/**
 * @brief rdpaa — 按 PAA 读一个 NAND 页（只读诊断）
 *
 * 默认：与 FTL 数据面相同的正常配置（scrambler ON、BCH ECC、per-CW CRC；
 *       页内 META 提供 LBA，LBA 未知时由 FC 自描述读回）。
 * --raw：LDPC_RAW 裸读——不做 ECC/解扰，坏页也能拿到原始位（分析 UNC 用）。
 * 同时用 gc_read_page_with_lba 打印该页逐 CW 的 META（页内反向映射）。
 * ⚠ PAA 不含 ch/ce：本命令固定用 ch=0/ce=0（当前板单芯片）。
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_rdpaa(int argc, char *argv[])
{
    DWORD      paa = 0U;
    const BOOL raw = (argc >= 3) && (strcmp(argv[2], "--raw") == 0);

    if ((argc >= 3) && (!raw))
    {
        UART_CLI_WRITE("error: unknown option (only --raw)\r\n");
        return;
    }

    if ((!uart_cli_parse_dword(argv[1], &paa)) || (paa_is_valid_paa(paa) == 0U))
    {
        UART_CLI_WRITE("error: bad paa\r\n");
        return;
    }

    /* host 冲突由 dispatcher 的 UART_CLI_FLAG_BUSY 单点拒绝。 */

    /* PAA 的 cw 位对整页读无意义：清掉，固定按页基址读。 */
    const DWORD pba = pba_page(paa_to_pba(0U, 0U, paa));

    /* raw 读需要 4 × 528B；静态缓冲，避免占 CLI 栈。 */
    static BYTE buf[PAGE_SIZE];

    memset(buf, 0, sizeof(buf));

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
        return;
    }

    /* 只 dump 前 256B（16 行）：诊断足够，且不会把串口写满。 */
    for (DWORD offset = 0U; (offset < 256U) && (offset < PAGE_MAIN_SIZE); offset += 16U)
    {
        (void)LOGI("%08lx: %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx  %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx %02hhx\r\n", offset, buf[offset], buf[offset + 1U], buf[offset + 2U], buf[offset + 3U], buf[offset + 4U], buf[offset + 5U], buf[offset + 6U], buf[offset + 7U], buf[offset + 8U], buf[offset + 9U], buf[offset + 10U], buf[offset + 11U], buf[offset + 12U], buf[offset + 13U], buf[offset + 14U], buf[offset + 15U]);
    }

    return;
}

/**
 * @brief 打印 rd 完成结果（hex dump / compare）
 *
 * @param status 请求完成状态；非 OK 时只报告失败，不把旧缓冲当读回数据
 */
static void uart_cli_rd_report(ssd_status_e status)
{
    DWORD total_bytes = g_cli_pending.nblocks * FTL_IU_BYTES; /* n 计扇区（512B），非页 */
    DWORD offset;

    if (status != SSD_STATUS_OK)
    {
        (void)LOGE("rd FAIL status=%d\r\n", (int)status);
        return;
    }

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
    UART_CLI_WRITE("rd PASS\r\n");
}

/**
 * @brief 轮询 CLI 未决请求并在完成时打印结果
 */
static void uart_cli_pending_poll(void)
{
    if ((g_cli_pending.busy == 0U) || (g_cli_pending.entry == NULL))
    {
        return;
    }

    ssd_req_s *request = &g_cli_pending.entry->request;

    if (ssd_controller_is_complete(request) == 0U)
    {
        return;
    }

    switch (request->opcode)
    {
        case SSD_REQUEST_WRITE:
            (void)LOGI("wr done lba=%llu n=%lu status=%d\r\n", g_cli_pending.lba, g_cli_pending.nblocks, (int)request->completion_status);
            break;
        case SSD_REQUEST_READ:
            uart_cli_rd_report(request->completion_status);
            break;
        case SSD_REQUEST_TRIM:
            (void)LOGI("tr done lba=%llu status=%d\r\n", g_cli_pending.lba, (int)request->completion_status);
            break;
        case SSD_REQUEST_FLUSH:
            (void)LOGI("fl done status=%d\r\n", (int)request->completion_status);
            break;
        default:
            break;
    }

    ssd_host_queue_free(&g_ctrl.host_queue, g_cli_pending.entry);
    g_cli_pending.entry = NULL;
    g_cli_pending.busy  = 0U;
}

/**
 * @brief 同步等待当前 CLI host 请求完成；handler 自带后台泵。
 *
 * @note 泵的是运行时服务（FTL/FC/controller/USB/timer），不是 CLI 解析器，
 *       因此不会重入命令 handler。完成打印与释放仍由 uart_cli_pending_poll 负责。
 */
static void uart_cli_wait_host(void)
{
    while (g_cli_pending.busy != 0U)
    {
        ssd_runtime_pump();
        uart_cli_pending_poll();
        storage_wdg_kick();
    }
}

/**
 * @brief 同步等待 wp 的全部在飞组完成。
 *
 * @note 组完成回调只递减 g_wp_inflight；FTL 推进由 ssd_runtime_pump 负责。
 */
static void uart_cli_wait_wp(void)
{
    while (g_wp_inflight != 0U)
    {
        ssd_runtime_pump();
        storage_wdg_kick();
    }
}

/* ── 帮助输出用的常量 ── */
#define UART_CLI_HELP_LINE_CAPACITY (128U)

/*
 * 长语法串外提：AlignArrayOfStructures 按最长单元格对齐整表，160 字符的 test
 * 语法留在行内会把全表撑到 200+ 列。test 的选项是 munit 适配层既有契约，原样保留。
 *
 * bbt 的语法随构建档切换：release 的 help 不再宣传不可用的子命令。
 */
#define UART_CLI_SYNTAX_TEST "test [--seed N] [--iterations N] [--single] [--list] [--list-params] [--log-visible info|debug] [--log-project info|debug] [--param name value]... [test_name...]"
#if (UART_CLI_DESTRUCTIVE_ENABLE != 0U)
#    define UART_CLI_SYNTAX_BBT "bbt [erase | fault <plane> <block>]"
#else
#    define UART_CLI_SYNTAX_BBT "bbt"
#endif
#define UART_CLI_SYNTAX_ADDR "addr pba <pba> | paa <paa> [ch ce] | g <g> [ch ce lun]"

/*
 * 命令表。约定：
 *   · 一级命令用短词；动作/模式是裸子命令（bbt erase / crash clear / addr pba）；
 *     修饰项用 `--全词`（--compare / --verify / --raw）。
 *   · help 只写「动词 + 宾语」，不枚举选项（选项只出现在 syntax）。
 *   · flags 是命令级策略，由 dispatcher 单点执行；bbt erase 的破坏性检查在
 *     handler 内（命令级 flag 会把只读 bbt 一起拒掉）。
 *   · 危险操作只在 debug/bench 档暴露（UART_CLI_DESTRUCTIVE_ENABLE）；量产档走
 *     main.c 的自动恢复路径（pfr_media_check_format）。
 */
static const uart_cli_cmd_s g_uart_cli_commands[] = {
  /* ── 系统 ── */
    {   "help",                         "help [command]",                "show command list or one command's syntax", 1U,  2U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,    uart_cli_help},
    { "status",                                 "status",                       "show UART error and queue counters", 1U,  1U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,  uart_cli_status},
    {"version",                                "version",                            "show firmware console version", 1U,  1U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM, uart_cli_version},
    {    "log",      "log level <error|warn|info|debug>",                                    "set runtime log level", 3U,  3U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,     uart_cli_log},
    {  "stack",                                  "stack",             "show stack guard: paint region, peak, alarms", 1U,  1U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,   uart_cli_stack},
    {  "crash",                          "crash [clear]",                      "show/clear the .noinit crash record", 1U,  2U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,   uart_cli_crash},
    {     "pc", "pc [live] [all|top] | clear | rate [n]",    "show sampled PC sequence/hot spots (same-ELF resolve)", 1U,  3U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,      uart_cli_pc},
    {   "info",                                   "info",                 "show controller + FTL identity and state", 1U,  1U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,    uart_cli_info},
    {   "stat",                                   "stat",        "show runtime counters (uptime, GC, L2P, CMT, USB)", 1U,  1U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,    uart_cli_stat},
    {     "id",                                     "id",             "show build config, capacity and media layout", 1U,  1U,                             UART_CLI_FLAG_NONE,      UART_CLI_GROUP_SYSTEM,      uart_cli_id},
 /* ── host I/O（模拟 NVMe 命令，经 controller 全链路）── */
    {     "wr",                           "wr <lba> <n>",                     "write n sectors with a fixed pattern", 3U,  3U,                             UART_CLI_FLAG_BUSY,     UART_CLI_GROUP_HOST_IO,      uart_cli_wr},
    {     "rd",               "rd <lba> <n> [--compare]",            "read n sectors (--compare checks the pattern)", 3U,  4U,                             UART_CLI_FLAG_BUSY,     UART_CLI_GROUP_HOST_IO,      uart_cli_rd},
    {     "tr",                               "tr <lba>",                                             "trim one lba", 2U,  2U,                             UART_CLI_FLAG_BUSY,     UART_CLI_GROUP_HOST_IO,      uart_cli_tr},
    {     "fl",                                     "fl",                        "flush dirty L2P + journal to NAND", 1U,  1U,                             UART_CLI_FLAG_BUSY,     UART_CLI_GROUP_HOST_IO,      uart_cli_fl},
    {     "wp",                       "wp <lba> <n> <k>",                       "submit k writes in one pair window", 4U,  4U,                             UART_CLI_FLAG_NONE,     UART_CLI_GROUP_HOST_IO,      uart_cli_wp},
 /* ── 诊断 ── */
    {      "q",                                      "q",                "show FC queues, pools and pending backlog", 1U,  1U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,       uart_cli_q},
    {     "cp",                                     "cp",                   "show checkpoint job state and sequence", 1U,  1U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,      uart_cli_cp},
    {    "jrn",                                    "jrn",               "show journal ring, watermark and WAL state", 1U,  1U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,     uart_cli_jrn},
    {"ftl_ctx",                       "ftl_ctx [module]", "dump FTL context (alloc|pend|seq|cache|jrn|cp|mig|md|wb)", 1U,  2U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG, uart_cli_ftl_ctx},
    {    "map",                              "map <lba>",                   "print L2P mapping + target block entry", 2U,  2U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,     uart_cli_map},
    {   "addr",                     UART_CLI_SYNTAX_ADDR,                          "translate PBA/PAA/gblock fields", 3U,  6U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,    uart_cli_addr},
    {  "rdpaa",                    "rdpaa <paa> [--raw]",              "read one NAND page by PAA (--raw: raw bits)", 2U,  3U,                             UART_CLI_FLAG_BUSY,        UART_CLI_GROUP_DIAG,   uart_cli_rdpaa},
    {   "pool",                                   "pool",               "print fixed-metadata candidate block pools", 1U,  1U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,    uart_cli_pool},
    {    "bbt",                      UART_CLI_SYNTAX_BBT,                              "print factory bad block map", 1U,  4U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,     uart_cli_bbt},
    {    "ecc",                                    "ecc",    "show per-block ECC health and corrected-bit histogram", 1U,  1U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,     uart_cli_ecc},
    {   "wear",                                   "wear",       "show erase/read-disturb distribution and WL spread", 1U,  1U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_DIAG,    uart_cli_wear},
 /* ── 破坏性（仅 debug/bench 档；量产档整行不注册）── */
#if (UART_CLI_DESTRUCTIVE_ENABLE != 0U)
    { "format",                      "format [--verify]",              "erase all blocks and write initial metadata", 1U,  2U, UART_CLI_FLAG_BUSY | UART_CLI_FLAG_DESTRUCTIVE, UART_CLI_GROUP_DESTRUCTIVE,  uart_cli_format},
#endif
  /* ── 测试（munit 适配层既有语法）── */
#if defined(FC_NAND_TEST_ENABLE)
    {   "test",                     UART_CLI_SYNTAX_TEST,                                "run munit unit test suite", 1U, 24U,                             UART_CLI_FLAG_NONE,        UART_CLI_GROUP_TEST,    uart_cli_test},
#endif
};

/**
 * @brief 按名字查找命令表项
 *
 * @param name 命令名
 * @return 命令描述；NULL = 未找到
 */
static const uart_cli_cmd_s *uart_cli_find(const char *name)
{
    DWORD index;

    for (index = 0U; index < UART_ARRAY_SIZE(g_uart_cli_commands); index++)
    {
        const uart_cli_cmd_s *command = &g_uart_cli_commands[index];

        if (strcmp(name, command->name) == 0)
        {
            return command;
        }
    }
    return NULL;
}

/**
 * @brief 生成一个命令组的名字摘要行
 *
 * @param group_name 组名
 * @param group 组枚举
 */
static void uart_cli_help_group(const char *group_name, BYTE group)
{
    char  line[UART_CLI_HELP_LINE_CAPACITY];
    DWORD used = 0U;
    BOOL  any  = false;
    DWORD index;

    for (index = 0U; index < UART_ARRAY_SIZE(g_uart_cli_commands); index++)
    {
        const uart_cli_cmd_s *command = &g_uart_cli_commands[index];

        if (command->group != group)
        {
            continue;
        }

        const int written = snprintf(&line[used], sizeof(line) - used, "%s%s", (any == true) ? " " : "", command->name);
        /* 截断即停：help 是诊断输出，宁缺勿越界。 */
        if ((written < 0) || ((DWORD)written >= (sizeof(line) - used)))
        {
            break;
        }
        used += (DWORD)written;
        any = true;
    }

    if (any == true)
    {
        (void)LOGI("  %-11s: %s\r\n", group_name, line);
    }
}

/**
 * @brief help 命令：分组列表或单命令语法
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_help(int argc, char *argv[])
{
    static const char *const group_names[UART_CLI_GROUP_COUNT] = {
        [UART_CLI_GROUP_SYSTEM]      = "system",
        [UART_CLI_GROUP_HOST_IO]     = "host I/O",
        [UART_CLI_GROUP_DIAG]        = "diag",
        [UART_CLI_GROUP_DESTRUCTIVE] = "destructive",
        [UART_CLI_GROUP_TEST]        = "test",
    };
    BYTE group;

    if (argc >= 2)
    {
        const uart_cli_cmd_s *command = uart_cli_find(argv[1]);

        if (command == NULL)
        {
            (void)LOGE("help: unknown command '%s'\r\n", argv[1]);
            return;
        }
        (void)LOGI("usage: %s\r\n", command->syntax);
        (void)LOGI("  %s\r\n", command->help);
        return;
    }

    for (group = 0U; group < UART_CLI_GROUP_COUNT; group++)
    {
        uart_cli_help_group(group_names[group], group);
    }
    UART_CLI_WRITE("use 'help <command>' for syntax and details\r\n");
}

/**
 * @brief status 命令：UART 错误/队列计数
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_status(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    uart_print_status();
    return;
}

/**
 * @brief version 命令：固件控制台版本
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_version(int argc, char *argv[])
{
    static const char version[] = "soft_ssd debug console\r\n";

    (void)argc;
    (void)argv;
    (void)uart_log_write(version, sizeof(version) - 1U);
    return;
}

/**
 * @brief log 命令：设置运行期日志级别
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_log(int argc, char *argv[])
{
    static const char updated[] = "log level updated\r\n";

    (void)argc;
    if (strcmp(argv[1], "level") != 0)
    {
        UART_CLI_WRITE("usage: log level <error|warn|info|debug>\r\n");
        return;
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
        UART_CLI_WRITE("usage: log level <error|warn|info|debug>\r\n");
        return;
    }

    (void)uart_log_write(updated, sizeof(updated) - 1U);
    return;
}

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 跑 UT 期间的**项目日志**级别，默认整档关闭（LOG_OFF）。
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
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 * @return 日志级别（默认 LOG_OFF）
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

/**
 * @brief test 命令：运行 munit 套件
 *
 * @param argc 参数个数（含命令名）
 * @param argv 参数向量
 */
static void uart_cli_test(int argc, char *argv[])
{
    const log_level_e saved = log_get_level();
    char             *filtered[UART_CLI_MAX_ARGUMENTS];
    int               filtered_argc = 0;

    // UT 假定宿主与后台都静止：host 在飞请求会插进 FC 队列，pure teardown 的
    // `ftl_op_req_count()==0` 断言必红（现场 sq_head_op=0x31 是 host 的 cache 读）。
    // 运行前要求操作者拔 USB；这里至少拦住「请求正在飞」这一种。
    if ((cli_host_busy() == true) || (usb_msc_host_busy() == true))
    {
        UART_CLI_WRITE("error: host I/O in flight — unplug USB / stop host access before 'test'\r\n");
        return;
    }

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

    (void)ut_run_all(filtered_argc, filtered); /* munit 自行打印结果；CLI 不解释返回码 */
    log_set_level(saved);
}
#endif

/*
 * 仅支持空格和 Tab 分隔，不支持引号、转义或二进制参数。该约束让调试 CLI 保持小且可预测；
 * 产测或固件下载应使用独立的二进制帧协议，而不是继续扩展这个文本解析器。
 */
#define UART_CLI_ARGC_INVALID (UINT32_MAX)
/**
 * @brief 按空格/Tab 分词命令行
 *
 * @param line 命令行缓冲
 * @param argv 参数向量
 * @param max_arguments 最大参数数
 * @return 参数个数；UART_CLI_ARGC_INVALID = 超限
 */
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

/**
 * @brief 分词、查找命令并执行
 *
 * @param line 命令行缓冲
 */
static void uart_execute_command(char *line)
{
    char *argv[UART_CLI_MAX_ARGUMENTS];
    DWORD count;

    count = uart_cli_tokenize(line, argv, UART_CLI_MAX_ARGUMENTS);
    if (count == 0U)
    {
        return;
    }
    if (count == UART_CLI_ARGC_INVALID)
    {
        UART_CLI_WRITE("error: too many arguments\r\n");
        return;
    }

    const int argc = (int)count; /* count ≤ UART_CLI_MAX_ARGUMENTS，窄化安全 */

    const uart_cli_cmd_s *command = uart_cli_find(argv[0]);

    if (command == NULL)
    {
        UART_CLI_WRITE("error: unknown command\r\n");
        return;
    }

    /* 只有参数个数不合法才打 usage；执行失败由 handler 自己报告原因。 */
    if ((argc < command->min_argc) || (argc > command->max_argc))
    {
        (void)LOGE("usage: %s\r\n", command->syntax);
        return;
    }

    /* 命令级 host 冲突策略的唯一执行点（wr/rd/tr/fl/rdpaa/format）。 */
    if (UART_CLI_CMD_HAS_FLAG(command, UART_CLI_FLAG_BUSY) && (cli_host_busy() || usb_msc_host_busy()))
    {
        UART_CLI_WRITE("error: busy, pending request\r\n");
        return;
    }

#if (UART_CLI_DESTRUCTIVE_ENABLE == 0U)
    /* 当前 destructive 命令用编译期隐藏；这层保证以后漏包 #if 的条目在量产档仍被拒绝。 */
    if (UART_CLI_CMD_HAS_FLAG(command, UART_CLI_FLAG_DESTRUCTIVE))
    {
        (void)LOGE("error: '%s' disabled in this build (destructive)\r\n", command->name);
        return;
    }
#endif

    command->handler(argc, argv);
}

/**
 * @brief 初始化 CLI（预填固定 pattern）
 */
void cli_init(void)
{
    /* g_wr_buf 落 NOLOAD SRAM2 段，上电不清零 → 主动预填 pattern */
    uart_cli_pattern_init();
}

/**
 * @brief 主循环轮询：读取 UART、执行命令行、轮询完成
 */
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

            UART_CLI_WRITE("\r\n");
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
                UART_CLI_WRITE("\b \b");
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

    /* 兜底完成轮询：同步命令已在 handler 内等待完成；这里只处理异常残留。 */
    uart_cli_pending_poll();
}

/**
 * @brief 断言/abort 后的现场终端：在调用者栈上循环运行正常 CLI。
 *
 * @note 不推进 FTL/FC 后台任务，失败状态保持冻结；命令解析、只读诊断和自带泵的
 *       命令（如 format）仍可用。断言点的调用栈保留在下方，调试器可继续回溯。
 * @note 本函数是 assertion.c 弱钩子的强符号；弱默认只做死循环。
 */
_Noreturn void assertion_console_run(void)
{
    /* 断言可能发生在 uart_execute_command 内部：外层 cli_service_poll 还没执行到
     * 「清 g_cli_length」那一步。若把残留状态带进现场循环，UART 里剩余的 CR/LF
     * 会把同一条命令再提交一次（板端现象：test 断言后自动重跑整套用例）。 */
    g_cli_length     = 0U;
    g_cli_discarding = 0U;
    g_cli_line[0]    = '\0';

    log_set_level(LOG_INFO);
    UART_CLI_WRITE("\r\n[ASSERT] console active; FTL/FC background frozen\r\n");
    uart_write_prompt();

    for (;;)
    {
        cli_service_poll();
    }
}
