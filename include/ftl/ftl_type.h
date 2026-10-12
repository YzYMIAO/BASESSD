#ifndef FTL_TYPE_H
#define FTL_TYPE_H

#include <stdint.h>

/*
 * FTL 层基础地址类型。
 *
 * 地址体系（行业标准）：
 *
 *   LBA (Host)  →  PBA (FTL full physical)  →  PAA (per-chip address)
 *   cmd->meta        L2P 表存的值               nand_paa_t
 *   逻辑地址         [CH][CE][PAA]             [LUN][BLOCK][PLANE][PAGE][CW]
 *
 * lba_t — Host 逻辑地址（uint32_t）。
 *   当前容量 512MiB ÷ 512B = 1,048,576 个 LBA，uint32_t 足够。
 *   作为 cmd->meta 传入 FC 用于 META 校验。
 */
typedef uint32_t lba_t;

/*
 * ftl_status_t — FTL 内部操作通用返回状态
 *
 *   FTL_OK    =  0  操作成功
 *   FTL_ERROR = -1  永久失败（配置错误 / 无可用空间）——重试无意义
 *   FTL_AGAIN = -2  瞬态背压（资源暂空）——调用者应**排队重试**
 *
 * FTL_AGAIN 的存在是为了把「资源暂空」与「真失败」分开：FC 命令/desc/buffer 池、
 * SQ、pending 表都可能在某一瞬间满（如 16 个多 plane 子操作同时在飞），但下一轮
 * ftl_service 抽干完成后即恢复。大请求被 FTL 拆成多个子操作时，若把这种瞬态当致命，
 * 请求就会在第 N 个子操作处整体失败——这正是本设计要消除的（见 ftl.c ftl_drain_group）。
 */
typedef enum
{
    FTL_OK    = 0,  /* 成功 */
    FTL_ERROR = -1, /* 永久失败（通用，不细分错误码） */
    FTL_AGAIN = -2, /* 瞬态背压（资源暂空，可重试） */
} ftl_status_t;

/*
 * ftl_topology_t — FTL 拓扑描述
 *
 * 描述本 FTL 实例管理的芯片路由。
 *   ch/ce/lun — 本 FTL 绑定的 NAND 芯片。
 *               元数据（Journal/BBT/Checkpoint/Anchor）存放在此芯片。
 *               用户数据由 allocator 基于此拓扑做负载均衡路由。
 *
 * ftl_init 内部初始化，不暴露给 controller 层。
 * 各子模块在初始化时接收一份副本。
 */
typedef struct
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
} ftl_topology_t;

#endif /* FTL_TYPE_H */