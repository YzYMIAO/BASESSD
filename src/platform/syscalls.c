/*
 * syscalls.c — 裸机 libc 系统调用桩（当前只有 _sbrk）
 *
 * 职责：
 *   为 newlib 的堆分配提供 sbrk 后端。工具链自带的是 libgloss/libnosys 版本
 *   （sbrk.c），它只做 `heap_end += incr`，**不做任何上界检查**；本文件用同名
 *   强符号覆盖它，补上堆/栈相撞的保护。
 *
 * 调用链：
 *   malloc/calloc/free（完整 libc.a: mallocr.o / freer.o）
 *     → _sbrk_r（libc: reent/sbrkr.c，把返回值 -1 翻译成 errno=ENOMEM）
 *       → _sbrk（本文件）
 *   链接顺序决定覆盖：目标文件里的 _sbrk 优先于归档成员，libnosys 版本被丢弃。
 *
 * 关键约定（DTCM 内存布局，见 src/platform/stm32h743/linker/stm32h743xx_flash.ld）：
 *   DTCM  = 0x20000000 .. 0x20020000（128 KB），同时装着堆和栈，两者相向生长：
 *     堆   ：起点 end = 0x20000000（链接脚本 PROVIDE(end = .)），向**上**长；
 *     栈   ：顶   _estack = 0x20020000，向**下**长。
 *   中间**没有**守卫区——这正是 libnosys 版本的危险之处：一次超量 malloc 就能把
 *   heap_end 推过 _estack，随后的 memset 会一路写穿 DTCM 末尾触发 BusFault。
 *   本实现把上界收紧到「当前 MSP - SBRK_STACK_GUARD」，且因为每轮都读实时 MSP，
 *   限制会随栈的实时深度自动下移。
 *
 * 生命周期：
 *   sys_init() 之前即可调用（只读 MSP，不碰外设）。无状态机、无中断参与。
 *
 * 文件结构：
 *   ① SBRK_STACK_GUARD —— 堆/栈之间的保留余量
 *   ② _sbrk()          —— 带上下界检查的堆生长
 */

#include "syscalls.h"

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stddef.h>
#include <stdint.h>

/* 本文件是**跨 target 共享**的平台源文件（放这里而不是 src/platform/<soc>/）：
 * 不 include 任何 SoC 头，也不出现 HAL 类型。唯一需要架构知识的是读 MSP ——
 * 用内联汇编直接取，不走内核头（那会依赖设备头提供 IRQn_Type，裸包含编译不过）。 */
static DWORD syscalls_current_msp(void)
{
    DWORD msp;

    __asm__ volatile("mrs %0, msp"
                     : "=r"(msp));
    return msp;
}

/* 堆顶与实时 MSP 之间必须留出的余量（字节）。
 *
 * 为什么需要：本函数返回后，调用方（malloc 内部）以及后续业务代码还会继续用栈。
 * 只判「不越过 MSP」是不够的——放行的那一刻可能已经贴着栈顶了。留 1 KB 让本
 * 函数自身的帧 + _sbrk_r/malloc 的帧 + 少量余量都有落脚点。
 * 为什么不更大：DTCM 只有 128 KB，堆与栈共享；历史实测栈峰值约 5.4 KB（v0.75，
 * `ftl_v075_board_verify.md`），不是归档文档里写的 55 KB。1 KB 余量与
 * _Min_Stack_Size 量级一致；堆/栈各自的真实用量用 CLI `stack` 观测。
 *
 * 本文件跨 target 共享，但余量取决于该 target 的 DTCM/堆栈布局 —— 允许目标用
 * -DSBRK_STACK_GUARD=... 覆盖；今天的 stm32h743 取默认值，行为零变化。 */
#ifndef SBRK_STACK_GUARD
#    define SBRK_STACK_GUARD (0x400U)
#endif

/* --------------------------------------------------------------------
 * _sbrk
 *   功能：把堆顶向上/向下移动 incr 字节，返回移动前的堆顶（新分配块的首地址）。
 *   参数：incr —— 增量（可为负，free 收缩堆时使用）。
 *   返回：成功返回旧堆顶；越界返回 (void *)-1（_sbrk_r 据此置 errno=ENOMEM）。
 *   实现要点：
 *     - 首次调用时惰性初始化 heap_end = &end（链接脚本符号，DTCM 底部）。
 *     - 上界：heap_end + incr 不得越过 __get_MSP() - SBRK_STACK_GUARD。
 *     - 下界：heap_end + incr 不得退到 &end 之下（防止 free 路径把堆顶算穿）。
 *     - 失败时**不**修改 heap_end，保证后续调用看到一致的堆顶。
 *     - 用 ptrdiff_t 而非 int：与 newlib _sbrk_r 的实参类型一致（ARM 上同为 32 位）。
 * -------------------------------------------------------------------- */
static char *s_heap_end;

extern char end; /* 链接脚本 PROVIDE(end = .) —— 堆起点 */

void *_sbrk(ptrdiff_t incr)
{
    char *prev_heap_end;
    char *limit;

    if (s_heap_end == NULL)
    {
        s_heap_end = &end;
    }

    prev_heap_end = s_heap_end;
    limit         = (char *)syscalls_current_msp() - SBRK_STACK_GUARD;

    if ((s_heap_end + incr > limit) || (s_heap_end + incr < &end))
    {
        return (void *)-1;
    }

    s_heap_end += incr;
    return (void *)prev_heap_end;
}

/* 已用堆字节数（堆顶 − end）。供 `stack` 诊断：堆与栈共用 DTCM，必须分开看。 */
DWORD syscalls_heap_used(void)
{
    if (s_heap_end == NULL)
    {
        return 0U;
    }
    return (DWORD)(s_heap_end - &end);
}
