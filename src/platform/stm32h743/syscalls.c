/*
 * syscalls.c — 裸机 libc 系统调用桩（当前只有 _sbrk）
 *
 * 职责：
 *   为 newlib 的堆分配提供 sbrk 后端。工具链自带的是 libgloss/libnosys 版本
 *   （sbrk.c），它只做 `heap_end += incr`，**不做任何上界检查**；本文件用同名
 *   强符号覆盖它，补上堆/栈相撞的保护。
 *
 * 调用链：
 *   malloc/calloc/free（libc_nano.a: nano-mallocr.o / nano-freer.o）
 *     → _sbrk_r（libc: reent/sbrkr.c，把返回值 -1 翻译成 errno=ENOMEM）
 *       → _sbrk（本文件）
 *   链接顺序决定覆盖：目标文件里的 _sbrk 优先于归档成员，libnosys 版本被丢弃。
 *
 * 关键约定（DTCM 内存布局，见 linker/stm32h743xx_flash.ld）：
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

#include "sys.h"

#include <stddef.h>

/* 堆顶与实时 MSP 之间必须留出的余量（字节）。
 *
 * 为什么需要：本函数返回后，调用方（malloc 内部）以及后续业务代码还会继续用栈。
 * 只判「不越过 MSP」是不够的——放行的那一刻可能已经贴着栈顶了。留 1 KB 让本
 * 函数自身的帧 + _sbrk_r/malloc 的帧 + 少量余量都有落脚点。
 * 为什么不更大：DTCM 只有 128 KB，堆与栈共享；测试期栈峰值约 55 KB，余量给太大
 * 会把可用堆压到影响正常分配。1 KB 与 _Min_Stack_Size 量级一致。 */
#define SBRK_STACK_GUARD (0x400U)

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
void *_sbrk(ptrdiff_t incr)
{
    extern char end; /* 链接脚本 PROVIDE(end = .) —— 堆起点 */

    static char *heap_end;
    char        *prev_heap_end;
    char        *limit;

    if (heap_end == NULL)
    {
        heap_end = &end;
    }

    prev_heap_end = heap_end;
    limit         = (char *)__get_MSP() - SBRK_STACK_GUARD;

    if ((heap_end + incr > limit) || (heap_end + incr < &end))
    {
        return (void *)-1;
    }

    heap_end += incr;
    return (void *)prev_heap_end;
}
