/*
 * pc_sample.c — 运行期 PC 采样器（死循环 / 跑飞定位）
 *
 * 记录放 .noinit：NOLOAD、启动不清零，可跨软复位 / 看门狗复位保留
 * （与 common/crash.c 同一机制；POR 会丢）。
 *
 * 活跃环 + 上一轮快照双缓冲：开机第一条 pc_sample_init 把上一轮的活跃环
 * **按时间顺序重排**后快照到 prev，避免启动期间的采样立刻覆盖故障现场。
 * 活跃环由 SysTick ISR 单写、主循环只读，DWORD 读写原子；clear/rate 通过 ready
 * 标志与 ISR 解耦。
 */

#include "pc_sample.h"

#include "assertion.h"

#define PC_SAMPLE_MAGIC   (0x50435341U) /* "PCSA" */
#define PC_SAMPLE_VERSION (2U)          /* v2：加 rate / tick_count / prev_rate */

typedef struct
{
    /* 跨复位保留的有效性标记（首次上电是随机 SRAM，靠 magic 判） */
    DWORD magic;
    DWORD version;
    DWORD active_valid;

    /* 本轮活跃环：SysTick ISR 单写 */
    DWORD rate;       /* 每多少个 tick 采一次（1..PC_SAMPLE_RATE_MAX） */
    DWORD head;       /* 下一个写位置 */
    DWORD total;      /* 累计样本数（用于算有效条目数） */
    DWORD tick_count; /* 分频计数（运行期） */
    DWORD ring[PC_SAMPLE_RING];

    /* 上一轮快照：pc_sample_init 一次写，之后只读；环已按时间顺序重排 */
    DWORD prev_valid;
    DWORD prev_rate;
    DWORD prev_count; /* 有效条目数（<= PC_SAMPLE_RING） */
    DWORD prev_ring[PC_SAMPLE_RING];
} pc_sample_record_t;

static volatile pc_sample_record_t g_pc_sample __attribute__((section(".noinit")));

/* ISR 与主循环共享：H16 要求 volatile；由 pc_sample_init 置位。 */
static volatile bool s_pc_sample_ready;

_Static_assert((PC_SAMPLE_RING & (PC_SAMPLE_RING - 1U)) == 0U, "PC_SAMPLE_RING 必须是 2 的幂（掩码取模）");

static DWORD pc_sample_rate_clamp(DWORD rate)
{
    if ((rate < 1U) || (rate > PC_SAMPLE_RATE_MAX))
    {
        return 1U;
    }
    return rate;
}

void pc_sample_init(void)
{
    DWORD index;

    /*
     * 快照上一轮：只有 magic/version/active_valid 三者都匹配才认为是有意义的环。
     * 首次上电（POR）或 SRAM 残留不匹配时 prev_valid=0，不展示垃圾数据。
     *
     * 活跃环是环形写；这里按 oldest→newest 重排进 prev，CLI 直接顺序打印即可。
     */
    if ((g_pc_sample.magic == PC_SAMPLE_MAGIC) && (g_pc_sample.version == PC_SAMPLE_VERSION) && (g_pc_sample.active_valid == 1U))
    {
        const DWORD total  = g_pc_sample.total;
        const DWORD count  = (total < PC_SAMPLE_RING) ? total : PC_SAMPLE_RING;
        const DWORD oldest = (total >= PC_SAMPLE_RING) ? g_pc_sample.head : 0U;

        for (index = 0U; index < count; index++)
        {
            g_pc_sample.prev_ring[index] = g_pc_sample.ring[(oldest + index) & (PC_SAMPLE_RING - 1U)];
        }
        g_pc_sample.prev_count = count;
        g_pc_sample.prev_rate  = pc_sample_rate_clamp(g_pc_sample.rate);
        g_pc_sample.prev_valid = (count != 0U) ? 1U : 0U;
        g_pc_sample.rate       = g_pc_sample.prev_rate; /* 复位后沿用上一轮 rate */
    }
    else
    {
        g_pc_sample.prev_count = 0U;
        g_pc_sample.prev_rate  = 1U;
        g_pc_sample.prev_valid = 0U;
        g_pc_sample.rate       = 1U;
    }

    g_pc_sample.magic        = PC_SAMPLE_MAGIC;
    g_pc_sample.version      = PC_SAMPLE_VERSION;
    g_pc_sample.active_valid = 1U;
    g_pc_sample.head         = 0U;
    g_pc_sample.total        = 0U;
    g_pc_sample.tick_count   = 0U;

    s_pc_sample_ready = true;
}

void pc_sample_on_tick(DWORD pc)
{
    DWORD index;

    // ISR 上下文：只做有界内存写，不加锁、不打日志（H21）。
    if (s_pc_sample_ready == false)
    {
        return;
    }

    g_pc_sample.tick_count++;
    if (g_pc_sample.tick_count < g_pc_sample.rate)
    {
        return;
    }
    g_pc_sample.tick_count = 0U;

    index                   = g_pc_sample.head;
    g_pc_sample.ring[index] = pc;
    g_pc_sample.head        = (index + 1U) & (PC_SAMPLE_RING - 1U);
    g_pc_sample.total++;
}

void pc_sample_clear(void)
{
    // 先停采样再重置，避免 clear 与 tick 交错写同一组游标；并发期间的丢失样本
    // 属于预期（clear 是诊断归零点，不是数据路径）。rate 保留。
    s_pc_sample_ready = false;

    g_pc_sample.head       = 0U;
    g_pc_sample.total      = 0U;
    g_pc_sample.tick_count = 0U;
    g_pc_sample.prev_count = 0U;
    g_pc_sample.prev_valid = 0U;

    s_pc_sample_ready = true;
}

bool pc_sample_prev_valid(void)
{
    return (g_pc_sample.prev_valid == 1U);
}

DWORD pc_sample_count(bool previous)
{
    if (previous)
    {
        return g_pc_sample.prev_count;
    }
    return (g_pc_sample.total < PC_SAMPLE_RING) ? g_pc_sample.total : PC_SAMPLE_RING;
}

DWORD pc_sample_at(bool previous, DWORD index)
{
    ASSERT(index < PC_SAMPLE_RING);

    return previous ? g_pc_sample.prev_ring[index] : g_pc_sample.ring[index];
}

DWORD pc_sample_head(void)
{
    return g_pc_sample.head;
}

DWORD pc_sample_rate(bool previous)
{
    return previous ? g_pc_sample.prev_rate : g_pc_sample.rate;
}

bool pc_sample_rate_set(DWORD rate)
{
    if ((rate < 1U) || (rate > PC_SAMPLE_RATE_MAX))
    {
        return false;
    }

    // 与 ISR 解耦：改 rate 时把分频计数清零，避免旧计数跨到新间隔。
    s_pc_sample_ready      = false;
    g_pc_sample.rate       = rate;
    g_pc_sample.tick_count = 0U;
    s_pc_sample_ready      = true;
    return true;
}
