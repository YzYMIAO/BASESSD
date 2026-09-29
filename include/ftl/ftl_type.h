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
 * 企业级 SSD 固件的 FTL 内部采用简洁的两值枚举：
 *   FTL_OK    =  0  操作成功
 *   FTL_ERROR = -1  操作失败（通用，不细分错误码）
 */
typedef enum
{
    FTL_OK    = 0,  /* 成功 */
    FTL_ERROR = -1, /* 通用失败 */
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