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
 *
 * 所有 FTL 内部「操作函数」（init/flush/create/save/load/read/write 等）
 * 统一返回此类型。谓词函数（check_trigger/is_emergency/needs_gc 等）
 * 和计数函数（gc_migrate_sb/wl_relocate 等）保持 int。
 *
 * 与 ssd_status_t 的边界：
 *   FTL 对外 API（ftl_read/ftl_write/ftl_trim）返回 ssd_status_t
 *   FTL 内部模块间调用返回 ftl_status_t
 *   在 ftl_read/write 包装函数中：FTL_ERROR → SSD_STATUS_IO_ERROR
 */
typedef enum
{
    FTL_OK    = 0,  /* 成功 */
    FTL_ERROR = -1, /* 通用失败 */
} ftl_status_t;

#endif /* FTL_TYPE_H */