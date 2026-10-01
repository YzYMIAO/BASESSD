/*
 * nand_debug.h
 *
 * 分层调试打印系统。每层独立编译开关，0=关，1=开。
 * 所有开关集中在此文件，改完重编译即可。零运行时开销（#if 编译时裁剪）。
 *
 * 使用方式：
 *   #define NAND_DBG_DISPATCH  1  // 查看 Dispatch Phase 机日志
 *   #include "nand_debug.h"
 *
 * 日志前缀：
 *   [FC]    FC 层 — submit/completion/busy
 *   [SCH]   调度器 — select/skip/chain
 *   [DSP]   Dispatch — Phase 1/2 状态机
 *   [ENG]   Engine — uop step 逐条执行
 *   [FMC]   FMC — PIO/WAIT_READY
 *   [INI]   Init — NAND 初始化流程
 */

#ifndef NAND_DEBUG_H
#define NAND_DEBUG_H

#include "common/ssd_log.h"

#include <stdint.h>

/* ======================================================================
 * Layer 开关（0=关, 1=开）
 *
 * 调试时把对应层改为 1，重编译即可。
 * 所有开关集中在文件头，改完重编译即生效（#if 编译时裁剪，零运行时开销）。
 * ====================================================================== */

#ifndef NAND_DBG_FC
#    define NAND_DBG_FC 0U /* FC 层：submit/completion/busy 跟踪 */
#endif

#ifndef NAND_DBG_SCHED
#    define NAND_DBG_SCHED 0U /* 调度器：select/skip/chain 仲裁 */
#endif

#ifndef NAND_DBG_DISPATCH
#    define NAND_DBG_DISPATCH 0U /* Dispatch：Phase 1/2 状态转换 */
#endif

#ifndef NAND_DBG_ENGINE
#    define NAND_DBG_ENGINE 0U /* Engine：uop step 逐条执行 */
#endif

#ifndef NAND_DBG_FMC
#    define NAND_DBG_FMC 0U /* FMC：PIO/WAIT_READY/RB# GPIO */
#endif

#ifndef NAND_DBG_DATA
#    define NAND_DBG_DATA 0U /* 数据内容（DATA_IN/OUT 首尾窗口） */
#endif

#ifndef NAND_DBG_TIMING
#    define NAND_DBG_TIMING 0U /* DWT 时间戳 / 等待超时计时 */
#endif

#ifndef NAND_DBG_RES
#    define NAND_DBG_RES 1U /* 资源统计 [RES]（测试中 cmd/desc/buf 使用量） */
#endif

/* ======================================================================
 * 宏封装
 *
 * 注意：开了的层用 SSD_LOGI 而非 SSD_LOGD，因为 main.c 默认
 * ssd_log_set_level(SSD_LOG_INFO) 会滤掉 DEBUG 级别。
 * 编译时 NAND_DBG_*=0 时完全裁剪，无运行时开销。
 *
 * 每层日志前缀（见文件头说明）：[FC]/[SCH]/[DSP]/[ENG]/[FMC]/[DAT]/[TIM]/[RES]。
 * ====================================================================== */

#if NAND_DBG_FC
#    define FC_LOGD(...) SSD_LOGI("[FC] " __VA_ARGS__)
#else
#    define FC_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_SCHED
#    define SCHED_LOGD(...) SSD_LOGI("[SCH] " __VA_ARGS__)
#else
#    define SCHED_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_DISPATCH
#    define DISP_LOGD(...) SSD_LOGI("[DSP] " __VA_ARGS__)
#else
#    define DISP_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_ENGINE
#    define ENG_LOGD(...) SSD_LOGI("[ENG] " __VA_ARGS__)
#else
#    define ENG_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_FMC
#    define FMC_LOGD(...) SSD_LOGI("[FMC] " __VA_ARGS__)
#else
#    define FMC_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_DATA
#    define DATA_LOGD(...) SSD_LOGI("[DAT] " __VA_ARGS__)
#else
#    define DATA_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_TIMING
#    define TIM_LOGD(...) SSD_LOGI("[TIM] " __VA_ARGS__)
#else
#    define TIM_LOGD(...) ((void)0U)
#endif

#if NAND_DBG_RES
#    define RES_LOGD(...) SSD_LOGI("[RES] " __VA_ARGS__)
#else
#    define RES_LOGD(...) ((void)0U)
#endif

#endif /* NAND_DEBUG_H */