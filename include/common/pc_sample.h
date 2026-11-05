#ifndef PC_SAMPLE_H
#define PC_SAMPLE_H

#include "ssd_types.h"

#include <stdbool.h>

/*
 * pc_sample — 运行期 PC 采样器（死循环 / 跑飞定位）。
 *
 * SysTick（1 ms）采样**被中断上下文**的 PC，写入 .noinit 环形缓冲。复位后
 * `pc_sample_init` 把上一轮的环快照到 prev，CLI `pc` 打印 top-N；再用**同一镜像**的
 * `debug.map` / `arm-none-eabi-addr2line` 反查函数与行号。
 *
 * 与 fault 机制的分工：fault 只在 NMI/HardFault/… 产生现场；本模块覆盖**没有异常**
 * 的死循环 / 忙等。`fault` 的 96 词栈扫描给候选返回地址，本模块给「PC 停在哪」。
 *
 * ⚠ 限制：
 *   · 中断被关（PRIMASK）或死循环在更高优先级 ISR 里 ⇒ SysTick 采不到；
 *     环停在关中断前的地址。
 *   · 死循环就在 SysTick 自身 ⇒ 同样采不到。
 *   · .noinit 只跨软复位 / 看门狗复位保留，**POR（拔电）会丢**。
 *   · 必须用产生该次运行的镜像的 ELF/map 反查（同值在不同镜像落不同符号）。
 *
 * 常态开销：每 1 ms 一次写环 + 自增；纳秒级，可忽略。
 */

/* 环深：1 ms × rate 采样；rate=1 时约 256 ms 历史。2 的幂。 */
#define PC_SAMPLE_RING (256U)

/* 采样间隔上限（tick 数）：rate=1000 ⇒ 窗口约 256 s。 */
#define PC_SAMPLE_RATE_MAX (1000U)

/*
 * 上电第一条调用：在 SysTick 启动前把上一轮的活跃环快照到 prev 并重置本轮。
 * 必须早于任何 ISR 采样。
 */
void pc_sample_init(void);

/* SysTick 的 C 入口调用：记录被中断上下文的 PC（只写活跃环）。 */
void pc_sample_on_tick(DWORD pc);

/* 清空上一轮快照与本轮活跃环（CLI `pc clear`）。可在 tick 并发下调用。 */
void pc_sample_clear(void);

/* 上一轮快照是否有效（复位前有采样）。 */
bool pc_sample_prev_valid(void);

/*
 * 取有效条目数（0..PC_SAMPLE_RING）。
 *
 * @param previous true = 上一轮快照；false = 本轮活跃环
 */
DWORD pc_sample_count(bool previous);

/*
 * 取一条 PC（0 <= index < PC_SAMPLE_RING；**物理下标**，不保证时间顺序）。
 *
 * 上一轮快照在 `pc_sample_init` 时已按 oldest→newest 重排，物理序即时间序；
 * 本轮活跃环需要用 `pc_sample_head` 计算环形起点。
 *
 * @param previous true = 上一轮快照；false = 本轮活跃环
 * @param index    环下标
 * @return 采样到的 PC
 */
DWORD pc_sample_at(bool previous, DWORD index);

/* 本轮活跃环的下一个写位置（时间序起点：total >= 环深时为 head，否则为 0）。 */
DWORD pc_sample_head(void);

/*
 * 取采样间隔（单位：SysTick tick，1 tick = 1 ms）。
 *
 * @param previous true = 上一轮使用的 rate；false = 当前 rate
 */
DWORD pc_sample_rate(bool previous);

/*
 * 设置本轮采样间隔（1..PC_SAMPLE_RATE_MAX）；值持久化在 .noinit，复位后沿用。
 *
 * @param rate tick 数
 * @return true = 已设置；false = 越界（未改变）
 */
bool pc_sample_rate_set(DWORD rate);

#endif /* PC_SAMPLE_H */
