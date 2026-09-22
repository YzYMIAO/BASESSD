#ifndef PLATFORM_STM32H743_NAND_RB_INTERRUPT_H
#define PLATFORM_STM32H743_NAND_RB_INTERRUPT_H

#include <stdint.h>

/*
 * NAND R/B# 中断驱动。
 *
 * 在第一次 WAIT_READY uop 时配置 PD6 (R/B#) 上升沿 EXTI 中断，
 * 设置等待标志后返回 NOT_READY。后续 step() 调用检查标志，
 * 已置位则返回 READY，超时则返回 TIMEOUT。
 *
 * 使用示例（nand_fmc_wait_ready 中的行为）：
 *   if (!nand_rb_interrupt_armed()) {
 *       nand_rb_interrupt_arm(timeout_us);  // 配置 EXTI，清除标志
 *       return NOT_READY;
 *   }
 *   if (nand_rb_interrupt_triggered()) {
 *       nand_rb_interrupt_disarm();         // 禁用 EXTI
 *       return READY;
 *   }
 *   if (nand_rb_interrupt_timed_out()) {
 *       nand_rb_interrupt_disarm();
 *       return TIMEOUT;
 *   }
 *   return NOT_READY;
 */

/* 初始化 R/B# EXTI 中断（在 nand_init 阶段调用一次） */
void nand_rb_interrupt_init(void);

/* 注册 WAIT_READY 等待：配置上升沿中断，清零标志，记录超时 */
void nand_rb_interrupt_arm(uint32_t timeout_us);

/* 取消等待：禁用 EXTI 中断 */
void nand_rb_interrupt_disarm(void);

/* 是否已 arm 且等待中 */
uint8_t nand_rb_interrupt_armed(void);

/* EXTI 上升沿是否已触发 */
uint8_t nand_rb_interrupt_triggered(void);

/* 是否超时（arm 时记录的 timeout_us 已耗尽） */
uint8_t nand_rb_interrupt_timed_out(void);

/* EXTI6 中断处理函数 — 由 EXTI6_IRQHandler 调用 */
void nand_rb_interrupt_handler(void);

#endif /* PLATFORM_STM32H743_NAND_RB_INTERRUPT_H */