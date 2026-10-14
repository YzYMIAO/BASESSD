#ifndef COMMON_STACK_GUARD_H
#define COMMON_STACK_GUARD_H

/*
 * stack_guard.h — 栈哨兵：开机给空闲栈填 pattern，运行期周期校验。
 *
 * 解决的问题：本仓 DTCM（128 KB）同时装堆和栈，**中间没有守卫区**——堆从
 * `end`（0x200000f8）向**上**长，栈从 `_estack`（0x20020000）向**下**长。
 * 一旦某次写越界踩进空闲栈区，故障现场只剩一个「PC/LR 落在数据区」的异常帧，
 * 事后只能靠符号表反推（见 docs/design/fault_capture.md 与 v0.71 那条未闭合记录）。
 * 本模块把「踩到哪」变成**当场可读**的数字。
 *
 * 机制：
 *   ① stack_guard_init()  —— 把 [&end, 当时 SP) 整段填 pattern，记下 paint_hi；
 *   ② stack_guard_check() —— superloop 每轮调一次：
 *        · MSP 越界（不在 [paint_lo, _estack)）⇒ 报 ALARM_MSP；
 *        · 从上次已知的最低点**继续向下**找第一个仍为 pattern 的字 ⇒ 得到 low_mark
 *          （单调下移，故整轮累计代价是 O(区域大小)，摊还到每次调用是 O(1)）；
 *        · low_mark 触底且底部那个字不再是 pattern ⇒ 报 ALARM_OVERFLOW。
 *   ③ stack_guard_report()—— 打印 paint 区间 / 栈峰值 / 余量 / 告警（CLI `stack`）。
 *
 * ⚠ 语义边界（别把它当 `-fstack-protector` 用）：
 *   · 它只能看见**空闲区**被踩。踩的是「已用栈里的某个保存寄存器」（越界写向上）时
 *     本模块**看不到**——那种要 -fstack-protector-strong（见 src/common/stack_guard.c 尾注）。
 *   · low_mark 是「栈峰值」的前提是**堆没被用过**：堆从 &end 向上长，堆的写入同样会把
 *     pattern 抹掉，此时 low_mark 反映的是「堆顶」而不是「栈底」。正常固件路径不用堆，
 *     只有 `test` 夹具会 malloc；`stack` 命令会把 &end 一并打出来便于判断。
 *
 * 生命周期：init 必须在 main() 里**较早**调用（越早，覆盖的深调用越多）；
 *           check/report 任意上下文可调（只读 MSP + 只读内存）。
 */

#include <stdint.h>

/* 告警位（stack_guard_report 打印，可组合）。 */
#define STACK_GUARD_ALARM_OVERFLOW 0x1U /* 空闲区底部被踩：栈（或堆）触底 */
#define STACK_GUARD_ALARM_MSP      0x2U /* MSP 落在 [paint_lo, _estack) 之外 */

/* 给空闲栈填 pattern 并开始守卫。重复调用等于重新起算（会重填一次）。 */
void stack_guard_init(void);

/* 每轮 superloop 调一次：MSP 越界检查 + 增量向下扫描。无告警时开销 ~10 条指令。 */
void stack_guard_check(void);

/* 打印 paint 区间 / 栈峰值 / 余量 / 告警计数（CLI `stack`）。 */
void stack_guard_report(void);

/* 当前告警位（0 = 无）。供上层自检用。 */
uint32_t stack_guard_alarm(void);

#endif /* COMMON_STACK_GUARD_H */
