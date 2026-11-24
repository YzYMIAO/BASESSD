/*
 * stack_guard.c — 栈哨兵实现（填 pattern + 增量扫描 + MSP 越界检查）
 *
 * 设计与语义边界见 include/common/stack_guard.h 的文件头。
 *
 * 文件结构：
 *   1. 链接脚本边界与常量
 *   2. 状态
 *   3. 实现        init / check / report / alarm
 *   4. 尾注        -fstack-protector-strong（本模块的补充，不是替代）
 */

#include "stack_guard.h"

#include "log.h"

/* ── 1. 链接脚本边界与常量 ── */

/* 堆起点 = DTCM 空闲区底部；栈顶 = DTCM 顶。两符号都来自
 * src/platform/stm32h743/linker/stm32h743xx_flash.ld（`PROVIDE(end = .)` / `_estack = ORIGIN+LENGTH`），
 * 与 src/platform/syscalls.c 取 &end 的方式一致。 */
extern char  end;
extern DWORD _estack;

/* pattern 用可读 ASCII："STAK"。不用 0xDEADBEEF/0xA5A5A5A5 这类常见填充——
 * 它们可能是别的代码/协议里正常出现的值，会制造假阴性。 */
#define STACK_GUARD_PATTERN (0x5354414BU)

/* 单次 check 最多向下扫多少个字。取小值是为了让「栈首次深下去」那一次
 * 不要把一整轮 superloop 卡住；摊还到多次调用后总代价不变。 */
#ifndef STACK_GUARD_SCAN_WORDS_PER_CALL
#    define STACK_GUARD_SCAN_WORDS_PER_CALL (64U)
#endif

/* init 自己的栈帧（push 的 r4/lr + 局部变量）位于入口 SP 之下；paint_hi
 * 必须把这段排除，否则 loop 会把保存的 LR 填成 STACK_GUARD_PATTERN，
 * 函数返回时跳进 0x5354414B（release -O2 实测：PC=0x5354414A，
 * MemManage IACCVIOL）。64 B 覆盖本函数在 -O2 下的帧并留出编译器余量。 */
#define STACK_GUARD_INIT_FRAME_MARGIN (64U)

/* 本文件跨 target 共享（同 syscalls.c）：只读 MSP，不 include 任何 SoC 头
 * ——那会依赖设备头提供 IRQn_Type，裸包含编译不过。 */
static DWORD stack_guard_msp(void)
{
    DWORD msp;

    __asm__ volatile("mrs %0, msp"
                     : "=r"(msp));
    return msp;
}

/* ── 2. 状态 ── */

static DWORD g_paint_lo;   /* 最低被填 pattern 的地址（= &end，4 对齐） */
static DWORD g_paint_hi;   /* 最高被填 pattern 的地址（= init 时的 SP，4 对齐） */
static DWORD g_low_mark;   /* 当前已知**被踩到**的最低地址，单调下移 */
static DWORD g_boot_msp;   /* init 时的 SP，报告里做基准 */
static DWORD g_armed;      /* init 过才允许 check */
static DWORD g_alarm;      /* STACK_GUARD_ALARM_* 位或 */
static DWORD g_alarm_hits; /* 告警次数（含重复；用于看"是一次还是持续"） */

/* ── 3. 实现 ── */

/* --------------------------------------------------------------------
 * stack_guard_init
 *   功能：把 [&end, 当前 SP) 填满 pattern，开始守卫。
 *   实现要点：
 *     - SP 向下 4 字节对齐后再作为 paint_hi：init 自身这一帧不算进被守卫区。
 *     - 布局异常（SP 已低于 &end）时**什么都不做**并返回——守卫宁可不装，
 *       也不能把堆/全局区当栈填掉。
 * -------------------------------------------------------------------- */
void stack_guard_init(void)
{
    DWORD lo  = ((uintptr_t)&end + 3U) & ~3U;
    DWORD msp = stack_guard_msp();
    DWORD hi  = ((msp > STACK_GUARD_INIT_FRAME_MARGIN) ? (msp - STACK_GUARD_INIT_FRAME_MARGIN) : msp) & ~3U;
    DWORD addr;

    if (hi <= lo)
    {
        return;
    }

    g_paint_lo   = lo;
    g_paint_hi   = hi;
    g_low_mark   = hi;
    g_boot_msp   = hi;
    g_alarm      = 0U;
    g_alarm_hits = 0U;

    for (addr = lo; addr < hi; addr += 4U)
    {
        *(volatile DWORD *)addr = STACK_GUARD_PATTERN;
    }

    g_armed = 1U;
}

/* --------------------------------------------------------------------
 * stack_guard_check
 *   功能：MSP 越界检查 + 增量向下扫描（每轮 superloop 一次）。
 *   实现要点：
 *     - MSP 越界**先查**且直接 return：MSP 都坏了，下面的扫描读的也是垃圾地址。
 *     - 扫描从 g_low_mark 往下走，遇到第一个仍为 pattern 的字就停 ⇒ low_mark
 *       只降不升，整个程序生命周期内总步数 ≤ 区域字数 ⇒ 摊还 O(1)。
 *     - 告警只打一次日志（置位那次），但 g_alarm_hits 每次都 +1：反复打日志会在
 *       已经不稳的栈上继续用 UART，得不偿失。
 * -------------------------------------------------------------------- */
void stack_guard_check(void)
{
    DWORD msp;
    DWORD mark;
    DWORD budget;

    if (g_armed == 0U)
    {
        return;
    }

    msp = stack_guard_msp();
    if ((msp < g_paint_lo) || (msp >= (uintptr_t)&_estack))
    {
        g_alarm_hits++;
        if ((g_alarm & STACK_GUARD_ALARM_MSP) == 0U)
        {
            g_alarm |= STACK_GUARD_ALARM_MSP;
            LOGE("[STK ] MSP out of range: %08lx (valid [%08lx,%08lx))\r\n", msp, g_paint_lo, (unsigned long)(uintptr_t)&_estack);
        }
        return;
    }

    mark   = g_low_mark;
    budget = STACK_GUARD_SCAN_WORDS_PER_CALL;
    while ((mark > g_paint_lo) && (budget > 0U))
    {
        if (*(volatile const DWORD *)(mark - 4U) == STACK_GUARD_PATTERN)
        {
            break;
        }
        mark -= 4U;
        budget--;
    }
    g_low_mark = mark;

    /* 触底且底字已被改写 ⇒ 有东西写到了 paint 区最低处（栈触底 / 堆撞上来）。 */
    if ((mark == g_paint_lo) && (*(volatile const DWORD *)mark != STACK_GUARD_PATTERN))
    {
        g_alarm_hits++;
        if ((g_alarm & STACK_GUARD_ALARM_OVERFLOW) == 0U)
        {
            g_alarm |= STACK_GUARD_ALARM_OVERFLOW;
            LOGE("[STK ] paint bottom clobbered @%08lx (peak=%lu B of %lu B)\r\n", g_paint_lo, (g_paint_hi - g_low_mark), (g_paint_hi - g_paint_lo));
        }
    }
}

/* --------------------------------------------------------------------
 * stack_guard_report
 *   功能：打印守卫状态（CLI `stack`）。纯只读，可在任意上下文调。
 *   实现要点：打印 `dtcm_bottom` 与 `peak` 一起，是为了让「low 落在堆顶而非栈底」
 *             这种情况一眼可辨——堆一被用过，peak 就不再是栈峰值。
 * -------------------------------------------------------------------- */
void stack_guard_report(void)
{
    DWORD msp = stack_guard_msp();

    if (g_armed == 0U)
    {
        LOGI("[STK ] not armed (stack_guard_init not called)\r\n");
        return;
    }

    LOGI("[STK ] paint=[%08lx,%08lx) size=%lu B  dtcm_bottom=%08lx\r\n", g_paint_lo, g_paint_hi, (g_paint_hi - g_paint_lo), (unsigned long)(uintptr_t)&end);
    LOGI("[STK ] boot_msp=%08lx now_msp=%08lx peak=%lu B low=%08lx headroom=%lu B\r\n", g_boot_msp, msp, (g_paint_hi - g_low_mark), g_low_mark, (g_low_mark - g_paint_lo));
    LOGI("[STK ] alarm=0x%lx hits=%lu %s%s\r\n", g_alarm, g_alarm_hits, ((g_alarm & STACK_GUARD_ALARM_OVERFLOW) != 0U) ? "OVERFLOW " : "", ((g_alarm & STACK_GUARD_ALARM_MSP) != 0U) ? "MSP " : "");
}

DWORD stack_guard_alarm(void)
{
    return g_alarm;
}

/* ── 4. 尾注 ──
 *
 * 本模块**不覆盖**的一类：越界写踩坏「已用栈里的某个保存寄存器」。
 * debug 构建是 -O0 ⇒ GCC 保留帧指针 ⇒ 每个函数尾声都是
 *     mov sp, r7
 *     pop {r7, pc}
 * 于是「压栈的 r7 被写坏」会直接让 SP 变成垃圾值，随后 pop 从垃圾地址取 PC
 * —— 这正是 v0.71 那条记录 `SP/LR 同时落在同一块 4 KB 缓冲里、且 LR 带 Thumb 位`
 * 的形态（见 docs/design/common/fault_capture.md）。这种踩踏发生在**已用区**，
 * 填 pattern 看不见。
 *
 * 对应的工具是 `-fstack-protector-strong`：GCC 在「有局部数组的函数」的局部变量与
 * 保存寄存器之间插一个 canary 字，返回前比对，不等就调 `__stack_chk_fail()`。
 * 裸机上要自己提供 `__stack_chk_guard`（开机赋非零值）与 `__stack_chk_fail`（dump 后
 * 停机）。它是**构建期全局开关**，会改所有 TU 的代码生成 ⇒ 需要单独一次改动 + 上板回归，
 * 未并入本模块。
 */
