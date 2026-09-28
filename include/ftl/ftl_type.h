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
 *
 * pba_t — 完整物理地址（uint32_t）。
 *   L2P 映射的目标值，包含拓扑路由（CH/CE）+ 芯片内地址（PAA）。
 *   FTL 从 PBA 拆出：
 *     cmd->config.ch   = pba_to_ch(pba)
 *     cmd->config.ce   = pba_to_ce(pba)
 *     cmd->paa         = pba_to_paa(pba)  →  nand_paa_t（含 LUN/Block/Plane/Page/CW）
 */
typedef uint32_t lba_t;
typedef uint32_t pba_t;

#endif /* FTL_TYPE_H */