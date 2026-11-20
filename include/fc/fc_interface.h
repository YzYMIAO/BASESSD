#ifndef FC_INTERFACE_H
#define FC_INTERFACE_H

/*
 * fc_interface.h — FC 层唯一公共接口
 *
 * 本文件集中定义 FC 公共契约：
 *   ① FC 基础类型 / 状态码 / 资源 ID
 *   ② 命令、描述符、CQ 结构与 CW 位图工具
 *   ③ 资源池 / 队列深度 / CW 布局常量
 *   ④ 对外 API 与 main 启动编排入口
 *
 * 私有实现头在 src/fc/，外部消费者不得 include。
 *
 * FC = 硬件 FCC 的软件替身：只提供槽位/描述符/队列/数据路径，不含 FTL 策略。
 */

#include "assertion.h"
#include "nand_config.h"   /* PLANE_NUM / CW_NUM / CW_SIZE / paa_to_plane */
#include "nand_protocol.h" /* nand_result_e（FC 启动编排返回类型） */
#include "ssd_types.h"     /* BYTE/WORD/DWORD/QWORD */
#include "stats.h"         /* SSD_STATS_ENABLE（统计专用声明按档裁剪） */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * FC 层基础类型
 *
 * FC（Flash Controller）层负责：命令池/Descriptor 链/缓冲池管理、
 * SQ/CQ 调度、ECC/CRC/RAID 数据路径。本段是 FC 层的最小依赖集
 * （状态码 + 资源 ID + 资源统计）。
 */

/* FC 层状态码：0 = 成功，其余为具名错误/资源码（全部非负） */
typedef enum FC_STATUS_E
{
    FC_STATUS_OK               = 0U,
    FC_STATUS_INVALID_ARGUMENT = 1U,
    FC_STATUS_QUEUE_FULL       = 2U,
    FC_STATUS_QUEUE_EMPTY      = 3U,
    FC_STATUS_NOT_READY        = 4U,
    FC_STATUS_IO_ERROR         = 5U,
    FC_STATUS_NOT_SUPPORTED    = 6U,
    FC_STATUS_RESOURCE_EMPTY   = 7U,
} fc_status_e;

/* 固定 Pool 的 16 位资源 ID；0xFFFF 表示无效句柄。 */
typedef WORD fc_cmd_id_t;
typedef WORD fc_desc_id_t;
typedef WORD fc_buf_id_t;

#define FC_CMD_ID_INVALID  ((fc_cmd_id_t)0xFFFFU)
#define FC_DESC_ID_INVALID ((fc_desc_id_t)0xFFFFU)
#define FC_BUF_ID_INVALID  ((fc_buf_id_t)0xFFFFU)

/* 资源使用统计（测试/监控用） */
typedef struct FC_RESOURCE_STATISTICS_S
{
    WORD sq_used;         /* SQ 当前占用条数 */
    WORD hsq_used;        /* HSQ 当前占用条数 */
    WORD cq_used;         /* CQ 当前占用条数 */
    WORD command_used;    /* 命令池已分配槽位数 */
    WORD descriptor_used; /* 描述符池已分配条目数 */
    WORD buffer_used;     /* buffer 池已占用块数 */
    WORD command_high;    /* 命令池历史最高占用（高水位，诊断/调参用） */
    WORD descriptor_high; /* 描述符池历史最高占用 */
    WORD buffer_high;     /* buffer 池历史最高占用 */
} fc_resource_stats_s;

/* 位域/位移宽度契约：这些量改变时必须在同一提交里改位宽，否则会静默截断。
 * - fc_cmd_cfg_s.cw_count  : 2 bit
 * - fc_desc_s.cw_index: 2 bit
 * - fc_cmd_cfg_s.multi_plane: 3 bit
 * - ch/ce/lun               : ch:2 / ce:2 / lun:4
 * - fc_cmd_cfg_s.operation : 8 bit
 * - flat CW 位图            : 1UL << n，n 必须 < 32
 */
_Static_assert(CW_NUM <= 4U, "fc_cmd_cfg_s.cw_count / fc_desc_s.cw_index are 2 bits");
_Static_assert(PLANE_NUM <= 7U, "fc_cmd_cfg_s.multi_plane is 3 bits");
_Static_assert(CW_TOTAL_MAX < 32U, "flat CW bitmap must fit a DWORD shift");

/*
 * FC 命令 / Descriptor / CQ 结构体
 *
 * 资源模型：
 *   - Command（fc_cmd_s）      ：一条 NVMe/FTL 提交的命令，对应 SQ 槽位。
 *   - Descriptor（fc_desc_s）：一个 CW 的数据描述（PAA + buffer + ECC 状态），
 *                                    命令经 descriptor 链串联（next_id 链接）。
 *   - CQ 完成项（fc_cmd_status_s）：命令级聚合状态 + 命令级返回段（fc_param_u）。
 *
 * 三池深度见本文件（FC_COMMAND_POOL_DEPTH / FC_DESCRIPTOR_POOL_DEPTH /
 * FC_BUFFER_POOL_DEPTH）。命令/描述符用 16 位 Pool ID 互相关联，
 * 0xFFFF = 无效句柄。
 *
 * ── 三套地址（权威定义与位序表见 nand/nand_config.h）──
 *
 *   LBA  逻辑地址      [CH][CE][LUN][BLOCK][PAGE][PLANE][CW]  FTL 用
 *   PBA  系统物理地址  与 LBA **逐位同构**                       **FTL 内部**用（含 ch/ce）
 *   PAA  芯片内地址    [CW][PAGE][PLANE][BLOCK][LUN]（BA6/PA 规则）FC → NAND 传它
 *
 * 分界：**FTL 交 LBA + PAA（PBA 在 FTL 侧 `pba_to_paa()` 一次转出），
 * FC 层只认 PAA 喂 NAND**。PBA 不跨进 FC——它带 ch/ce，而 FC 的路由本就独立走
 * `cmd->ch/ce/lun`（见 fc_build_dispatch_request），两套编码并存只会制造静默算错地址的机会。
 * ⚠ 代价：PBA 与 LBA 同构时「`desc->paa` 与 `desc->meta` 用同一个 plane_cw 公式」的便利消失——
 * PAA 的 plane 与 cw **不相邻**（`[cw][page][plane][block]`），建链改用
 * `paa_with_plane()` / `paa_with_cw()`；`PLANE_CW_MASK` 技巧只在 LBA/PBA 空间有效。
 * 三者的低 3 位（PLANE_CW_MASK）仍是同一套编码 [plane:1][cw:2]。
 */

/* ── ① 配置枚举 ── */

/* ECC 解码模式（配置写入 fc_cmd_cfg_s.ldpc） */
typedef enum FC_ECC_MODE_E
{
    FC_ECC_MODE_NORMAL    = 0U, /* 正常纠错；当前编译引擎为 BCH */
    FC_ECC_MODE_LDPC_HARD = 1U, /* LDPC hard-decision；未完整支持 */
    FC_ECC_MODE_LDPC_SOFT = 2U, /* LDPC soft-decision；未完整支持 */
    FC_ECC_MODE_LDPC_RAW  = 3U, /* raw，跳过 ECC 解码 */
} fc_ecc_mode_e;

/* RAID 模式（配置写入 fc_cmd_cfg_s.raid） */
typedef enum FC_RAID_MODE_E
{
    FC_RAID_MODE_NORMAL  = 0U, /* 无 RAID */
    FC_RAID_MODE_DECODE  = 1U, /* RAID 解码（ECC 失败时恢复） */
    FC_RAID_MODE_ENCODE  = 2U, /* RAID 编码（生成 parity） */
    FC_RAID_MODE_RECOVER = 3U, /* RAID 恢复（同 decode） */
    FC_RAID_MODE_CHECK   = 4U, /* RAID 校验（暂未实现） */
} fc_raid_mode_e;

/* NAND 页类型（TLC/QLC 多页编程时区分；SLC 只有 LP） */
typedef enum FC_PAGE_TYPE_E
{
    FC_PAGE_TYPE_LP = 0U, /* Lower Page (SLC page / TLC LP) */
    FC_PAGE_TYPE_MP = 1U, /* Middle Page (TLC MP) */
    FC_PAGE_TYPE_UP = 2U, /* Upper Page (TLC UP) */
    FC_PAGE_TYPE_TP = 3U, /* Triple Page (QLC TP) */
} fc_page_type_e;

/* CRC 校验开关（配置写入 fc_cmd_cfg_s.crc） */
typedef enum FC_CRC_MODE_E
{
    FC_CRC_ENABLE  = 0U, /* CRC 校验使能 */
    FC_CRC_DISABLE = 1U, /* CRC 校验禁用 */
} fc_crc_mode_e;

/* 数据加扰开关（配置写入 fc_cmd_cfg_s.random） */
typedef enum FC_RANDOM_MODE_E
{
    FC_RANDOM_ENABLE  = 0U, /* 数据加扰使能 */
    FC_RANDOM_DISABLE = 1U, /* 数据加扰禁用 */
} fc_random_mode_e;

/* 链式命令标记（配置写入 fc_cmd_cfg_s.chain） */
typedef enum FC_CHAIN_MODE_E
{
    FC_CHAIN_END  = 0U, /* 链结束，单命令 */
    FC_CHAIN_MORE = 1U, /* 链中间，后续还有命令 */
} fc_chain_mode_e;

/* buffer 归属（配置写入 fc_cmd_cfg_s.buf_own）：本命令的数据 buffer **谁申请、谁释放**。
 *
 * bit1 = 申请方（0 = FC 申请，1 = 上层申请）；bit0 = 释放方（0 = 上层释放，1 = FC 释放）。
 * 「上层释放」= 上层调 fc_command_release() 归还（desc 链即命令的资源集，FC 一并归还链上
 * 所有未归还的 buffer）；「FC 释放」= FC 在命令完成时当场归还（并把 desc 的 buffer_id 抹成
 * INVALID，故之后 fc_command_release 不会重复归还）。
 *
 * **`00` 是默认值 = 既有生产行为**（FC 建链时申请、上层 release 时归还）：命令出池整体清零
 * ⇒ 未显式设置本字段的命令恒走 `00`，即 FC 申请 / 上层随命令释放。 */
typedef enum FC_BUFFER_OWN_E
{
    FC_BUF_OWN_FC_ALLOC_UPPER_FREE    = 0U, /* 00：FC 申请，上层释放——读路径，上层拿 buffer 并掌控生命周期（默认） */
    FC_BUF_OWN_FC_ALLOC_FC_FREE       = 1U, /* 01：FC 申请，FC 释放——读，只要命令状态 */
    FC_BUF_OWN_UPPER_ALLOC_UPPER_FREE = 2U, /* 10：上层申请，上层释放——上层把控数据场景 */
    FC_BUF_OWN_UPPER_ALLOC_FC_FREE    = 3U, /* 11：上层申请，FC 释放——写命令等，只要命令状态 */
} fc_buf_own_e;

#define FC_BUF_OWN_ALLOC_BY_UPPER (0x2U) /* bit1：1 = 上层申请（上层自行 fc_buffer_alloc 并传入） */
#define FC_BUF_OWN_FREE_BY_FC     (0x1U) /* bit0：1 = FC 在完成时归还 */

/* ⚠ 本头文件**不声明**“完成路由归属模块”枚举，命令对象也不携带 FTL 的完成
 * 服务函数选择器。它是 FTL 的概念（哪个功能拥有这条命令），由 FTL 的 request
 * 表按 cmd_id 保存（见 include/ftl/ftl_op.h）。
 * CQE（`fc_cmd_status_s`）只带 cmd_id / status；FTL 完成派发时用
 * cmd_id 取回自己的 request 表项。 */

/* ── ② 状态位与判定 ── */

/* 状态有两套坐标：
 *   · descriptor 状态（FC_CW_STATUS_*，per-CW）：数据路径结果 + NAND per-CW FAIL；
 *   · command 状态（FC_CMD_STATUS_*，命令级）：NAND 前/后两次读状态的位 + 数据路径聚合。
 * 两者不共用位号；命令级的数据路径只保留 DATA_ERROR/META 两个聚合位，具体类别看 desc。
 *
 * NAND 70h 状态位（Table 18）：bit0 FAIL（后一次读状态，当前操作）、
 * bit1 FAILC（后一次读状态，上一页 cache program）、bit5 ARDY、bit6 RDY、bit7 WP。
 * 这些位在 command 状态里保持原义，**不是“命令成功”**：
 * 命令成功 = FAIL/FAILC/TIMEOUT/DATA_ERROR/META 全 0；RDY/AREADY/WP 是器件状态。
 */

/* ── descriptor 状态（per-CW） ── */
#define FC_CW_STATUS_UNUSED (0U)
#define FC_CW_STATUS_FAIL   (1U << 0) /* 70h FAIL（后一次读状态）：该 CW 的 NAND 操作失败 */
/* bit1/2 保留 */
#define FC_CW_STATUS_RAID_ERROR (1U << 3)
#define FC_CW_STATUS_META_ERROR (1U << 4)
#define FC_CW_STATUS_CRC_ERROR  (1U << 5)
#define FC_CW_STATUS_UNC        (1U << 6)
/* 空页（erased page）：ECC 解码失败 **且** 整条 CW（META+CRC+USER+ECC，528B）的
 * 1 比特占比 ≥ 99%（判据与阈值推导见 fc_data_path.c 的 fc_cw_is_blank）。
 * 擦除页不是合法 BCH 码字 ⇒ 解码必然报 UNC；若不单独识别，GC 读到半写块的空洞页
 * 就会把整块当坏块退役。**不属于 FC_CW_STATUS_ERROR_MASK**：
 * 「页从未编程」是介质的合法状态，不是读错误。
 * ⚠ 判据只在**解码路径**成立：RAW 读（LDPC_RAW）跳过 ECC，不产生本状态。 */
#define FC_CW_STATUS_EMPTY (1U << 7)

/* 「失败位」的规范集合：descriptor 的五个错误位。
 * 判「CW 是否失败」时一律经 `fc_cw_status_has_error()` 或本集合。 */
#define FC_CW_STATUS_ERROR_MASK (FC_CW_STATUS_FAIL | FC_CW_STATUS_RAID_ERROR | FC_CW_STATUS_META_ERROR | FC_CW_STATUS_CRC_ERROR | FC_CW_STATUS_UNC)

/* ── command 状态（命令级） ── */
#define FC_CMD_STATUS_NAND_FAIL  (1U << 0) /* 70h FAIL（后一次读状态） */
#define FC_CMD_STATUS_FAIL_CACHE (1U << 1) /* 70h FAILC（后一次读状态，上一页 cache program） */
#define FC_CMD_STATUS_TIMEOUT    (1U << 2) /* 忙等超时（前一次读状态仍 busy） */
#define FC_CMD_STATUS_DATA_ERROR (1U << 3) /* 任一 CW：UNC/CRC/RAID */
#define FC_CMD_STATUS_META_ERROR (1U << 4) /* 任一 CW：META（BBT 无 desc 也用它） */
#define FC_CMD_STATUS_AREADY     (1U << 5) /* 70h ARDY（前一次读状态：阵列/内部操作状态） */
#define FC_CMD_STATUS_RDY        (1U << 6) /* 70h RDY（前一次读状态：cache 是否可接收新数据） */
#define FC_CMD_STATUS_WP         (1U << 7) /* 70h WP（0=protected，1=not protected） */

/* 命令级失败位：不含 RDY/AREADY/WP —— 它们是器件状态，不是命令成败。 */
#define FC_CMD_STATUS_ERROR_MASK (FC_CMD_STATUS_NAND_FAIL | FC_CMD_STATUS_FAIL_CACHE | FC_CMD_STATUS_TIMEOUT | FC_CMD_STATUS_DATA_ERROR | FC_CMD_STATUS_META_ERROR)

/**
 * @brief descriptor 状态是否含任一错误位。
 *
 * @param status descriptor 状态（FC_CW_STATUS_* 位或）
 *
 * @return true  含至少一个错误位；false 不含。
 *
 * @note 读路径的成败判据 = 本函数（单看 FAIL 会把 UNC / META 当成功）。
 */
static inline BOOL fc_cw_status_has_error(DWORD status)
{
    return (status & FC_CW_STATUS_ERROR_MASK) != 0U;
}

/**
 * @brief 单个 CW 状态是否为不可纠 ECC（UNC）
 *
 * @param status FC_CW_STATUS_* 位或
 * @return true = UNC
 */
static inline BOOL fc_cw_status_has_unc(DWORD status)
{
    return (status & FC_CW_STATUS_UNC) != 0U;
}

/**
 * @brief 该 CW 是否被判定为空页（从未编程）。
 *
 * @param status descriptor 状态（FC_CW_STATUS_* 位或）
 *
 * @return true  空页；false 非空页。
 *
 * @note 与 fc_cw_status_has_error 正交：空页既不表示读成功，也不表示读失败。
 */
static inline BOOL fc_cw_status_is_empty(DWORD status)
{
    return (status & FC_CW_STATUS_EMPTY) != 0U;
}

/**
 * @brief descriptor 是否读成功（无错误、非空页）。
 *
 * @param status descriptor 状态（FC_CW_STATUS_* 位或）
 *
 * @return true = 成功（状态为 0）；false = 有错误或空页。
 */
static inline BOOL fc_cw_status_is_success(DWORD status)
{
    return status == FC_CW_STATUS_UNUSED;
}

/**
 * @brief 命令级状态是否含任一错误位。
 *
 * @param status command 状态（FC_CMD_STATUS_* 位或）
 *
 * @return true  命令失败；false 命令成功（RDY/AREADY/WP 不影响成败）。
 */
static inline BOOL fc_cmd_status_has_error(DWORD status)
{
    return (status & FC_CMD_STATUS_ERROR_MASK) != 0U;
}

/**
 * @brief 命令是否成功（错误位全 0）。
 *
 * @param status command 状态（FC_CMD_STATUS_* 位或）
 *
 * @return true = 成功；false = 失败。
 */
static inline BOOL fc_cmd_status_is_success(DWORD status)
{
    return (status & FC_CMD_STATUS_ERROR_MASK) == 0U;
}

/* ── ③ 命令 / 描述符 / CQ 结构 ── */

/* 命令级操作配置：operation 与数据路径开关统一放在这里。 */
typedef struct FC_COMMAND_CONFIG_S
{
    DWORD operation   : 8; /* ONFI operation macro (OP_*). */
    DWORD cw_count    : 2; /* 每-plane CW 数减一；仅 RAID stripe 使用。 */
    DWORD crc         : 1; /* FC_CRC_ENABLE / FC_CRC_DISABLE. */
    DWORD ldpc        : 2; /* fc_ecc_mode_e. */
    DWORD raid        : 3; /* fc_raid_mode_e. */
    DWORD random      : 1; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE. */
    DWORD chain       : 1; /* FC_CHAIN_END / FC_CHAIN_MORE. */
    DWORD page_type   : 2; /* fc_page_type_e. */
    DWORD multi_plane : 3; /* 0 = 单 plane；N = N-plane。 */
    DWORD buf_own     : 2; /* fc_buf_own_e：buffer 申请方/释放方。 */
    DWORD reserved    : 7; /* 位域补齐，必须为 0。 */
} fc_cmd_cfg_s;

_Static_assert(sizeof(fc_cmd_cfg_s) == 4U, "fc_cmd_cfg_s size changed");
_Static_assert(CH_NUM <= 4U, "ch is 2 bits");
_Static_assert(CE_NUM <= 4U, "ce is 2 bits");
_Static_assert(LUN_NUM <= 16U, "lun is 4 bits");

/* 4 字节内联参数 / 返回段：byte 视图用于命令参数，word 视图用于整体读写。
 * Cortex-M7 小端：parameter0 是最低字节。 */
typedef union FC_PARAMETER_U
{
    struct
    {
        BYTE parameter0;
        BYTE parameter1;
        BYTE parameter2;
        BYTE parameter3;
    };
    DWORD value;
} fc_param_u;

_Static_assert(sizeof(fc_param_u) == 4U, "fc_param_u size changed");

/* 单 CW 描述符：物理地址 + 命令配置副本 + buffer/链 ID + 状态。
 * 命令级配置副本保留给命令泛化/错误恢复回填。 */
typedef struct FC_DESCRIPTOR_S
{
    DWORD        paa;    /* 本 CW 的 PAA。 */
    fc_cmd_cfg_s config; /* 命令级配置副本；错误恢复上下文。 */
    DWORD        buffer_id : 16;
    DWORD        next_id   : 16; /* 0xFFFF = 链尾。 */
    DWORD        meta;           /* 本 CW 的扇区 LBA。 */

    DWORD cw_index : 2;
    DWORD status   : 8; /* FC_CW_STATUS_* bits. */
    DWORD ecc_bits : 8;
    DWORD reserved : 14; /* 位域补齐，必须为 0。 */
} fc_desc_s;

_Static_assert(sizeof(fc_desc_s) == 20U, "fc_desc_s size changed");

/* command->meta 的「本命令无已知 LBA」哨兵。0 是合法 LBA，不能当哨兵。 */
#define FC_META_UNKNOWN (0xFFFFFFFFU)

/* 命令上下文。身份/路由在前，配置/地址/参数在后。 */
typedef struct FC_COMMAND_S
{
    /* identity */
    DWORD cmd_id  : 16; /* command_pool 槽位号；FREE 状态仍保留。 */
    DWORD desc_id : 16; /* 首个 descriptor ID；0xFFFF = 无。 */

    /* control / route */
    DWORD high_priority : 1;
    DWORD feature_addr  : 8;
    DWORD ch            : 2;
    DWORD ce            : 2;
    DWORD lun           : 4;
    DWORD reserved      : 15; /* 位域补齐，必须为 0。 */

    fc_cmd_cfg_s config;
    DWORD        paa;
    DWORD        meta;
    fc_param_u   param;
} fc_cmd_s;

_Static_assert(sizeof(fc_cmd_s) == 24U, "fc_cmd_s size changed");

/* CQ 完成项：命令身份 + 命令级状态 + 返回段。 */
typedef struct FC_COMMAND_STATUS_S
{
    DWORD      cmd_id   : 16;
    DWORD      status   : 8;
    DWORD      reserved : 8; /* 位域补齐，必须为 0。 */
    fc_param_u param;
} fc_cmd_status_s;

_Static_assert(sizeof(fc_cmd_status_s) == 8U, "fc_cmd_status_s size changed");
_Static_assert(offsetof(fc_cmd_status_s, param) == 4U, "param offset changed");

/* ── ④ 结构内联工具 / 校验 ── */

/**
 * @brief plane/cw → flat 绝对 CW 号（plane*CW_NUM + cw）。
 *
 * @param plane 物理 plane 号。
 * @param cw    plane 内 CW 号。
 * @return flat CW 号（0 .. CW_TOTAL_MAX-1）。
 */
static inline BYTE fc_cw_flat(BYTE plane, BYTE cw)
{
    return (BYTE)((plane * CW_NUM) + cw);
}

/**
 * @brief 给出命令能触及的 plane 段 [*plane_first, *plane_last)。
 *
 * @param cmd     命令。
 * @param plane_first 输出：plane 段起点（调用者保证非空）。
 * @param plane_last  输出：plane 段终点（调用者保证非空）。
 *
 * @return 段内 plane 数（= multi_plane，单 plane 时为 1）。
 *
 * @note 实现要点：- multi-plane：从 plane 0 起，覆盖 plane_count 个 plane；
 * - 单 plane：**只覆盖 paa_to_plane(command->paa) 那一个**——掩码是绝对编号，
 * plane 1 的单 plane 命令落在 0xF0 段而不是 0x0F 段。
 *
 * 放在头文件（唯一一份定义）：这是 fc_cmd_s 的结构属性——FC 侧用它做
 * 建链入口（fc_pool.c 的 fc_command_configure_descriptor）的越段校验，
 * FTL 侧用它做位图展开。两份实现会让改 plane 语义时漏改。
 * ⚠ 上界写死 PLANE_NUM：desc PAA 的 plane 位与请求的 plane_paa[PLANE_NUM]
 * 都按 PLANE_NUM 定尺。改 plane 语义时这里与 fc_nand_dispatch.c 的
 * multi-plane handler 要一起动。
 */
static inline BYTE fc_command_plane_span(const fc_cmd_s *cmd, BYTE *plane_first, BYTE *plane_last)
{
    const BYTE plane_count = (cmd->config.multi_plane > 0U) ? cmd->config.multi_plane : 1U;

    ASSERT(plane_count <= PLANE_NUM);

    *plane_first = (plane_count == 1U) ? paa_to_plane(cmd->paa) : 0U;
    *plane_last  = (*plane_first + plane_count);
    return plane_count;
}

/**
 * @brief 本命令 plane 段内**全部** CW 的 flat 位图——「整段访问」的显式写法。
 *
 * @param cmd 命令（须已设 config.multi_plane 与 paa，本函数按它们定段）。
 *
 * @return 段内每个 CW 一位置 1 的 flat 位图（bit n = plane*CW_NUM + cw）；
 *         plane 1 单 plane ⇒ 0xF0，两 plane ⇒ 0xFF。
 *
 * @note 用途：产出 **ftl_cmd_configure_descriptors(cmd, cw_bitmap) 的实参**——FTL 的整段路径
 * （pba_to_cmd / _pair_cw）一律先调本函数算出位图，再作为形参交给建链入口。
 * ⚠ 必须在 paa 与 multi_plane **都写好之后**调用——它读的就是这两个字段。
 */
static inline DWORD fc_command_plane_span_cw_bitmap(const fc_cmd_s *cmd)
{
    BYTE plane_first;
    BYTE plane_last;

    (void)fc_command_plane_span(cmd, &plane_first, &plane_last);
    return ((1U << ((plane_last - plane_first) * CW_NUM)) - 1U) << fc_cw_flat(plane_first, 0U);
}

/**
 * @brief 把 FC_CW_STATUS_* 位解码成短字符串（日志用）
 *
 * @param status descriptor 状态（FC_CW_STATUS_* 位或）
 * @param buf    输出缓冲（可传 NULL）
 * @param buflen 缓冲容量（含结尾 NUL）
 *
 * @return buf  便于直接放进 LOG 实参
 *         ""   buf 为 NULL 或 buflen 为 0
 *
 * @note 输出例："UNC"、"CRC|META"、"EMPTY"；未知位以 '?' 表示。
 * 输出按 buflen 截断，不越界。
 */
const char *fc_cw_status_str(DWORD status, char *buf, DWORD buflen);

/**
 * @brief 把 FC_CMD_STATUS_* 位解码成短字符串（日志用）
 *
 * @param status command 状态（FC_CMD_STATUS_* 位或）
 * @param buf    输出缓冲（可传 NULL）
 * @param buflen 缓冲容量（含结尾 NUL）
 *
 * @return buf  便于直接放进 LOG 实参
 *         ""   buf 为 NULL 或 buflen 为 0
 *
 * @note 输出例："NAND|TIMEOUT"、"FAILC"、"DATA"、"RDY"；未知位以 '?' 表示。
 * 输出按 buflen 截断，不越界。
 */
const char *fc_cmd_status_str(DWORD status, char *buf, DWORD buflen);

/**
 * @brief 校验命令配置合法性。
 *
 * @param cmd FC 命令。
 *
 * @note 实现要点（整函数转 ASSERT）：
 *   - 前置违反——operation ≥ OP_COUNT / 数据操作无 desc 链——全是**调用者 bug**：
 *     `operation` 全仓都是 FTL 的硬编码常量（见 ftl_fc_interface.c 的
 *     sync_page_prepare 实参），**从不是 host 输入**；release 下断言裁空，由调用链保证。
 *     `cmd` 指针非空同样由调用者保证（本函数不重复守）。
 *   - has_data 覆盖数据输出型操作（READ_STATUS/READ_ID 等），它们也需要 buffer；
 *     data_input_cycles>0 只是其子集。
 *   - **不校验 CW 集合**（集合不在命令里）：它是 FTL 的建链输入，FC 只认 desc 链
 *     （链长度即真值）。越段检查随枚举逻辑一并上移 FTL，见
 *     ftl_cmd_configure_descriptors()。本函数只确认「有数据的命令必须已有链」。
 */
void fc_command_validate(const fc_cmd_s *cmd);

/**
 * @brief 判断 flat CW 掩码在**每个 plane 上的 CW 集合是否相同**。
 *
 * @param mask flat 掩码（bit n = plane*CW_NUM + cw）。
 *
 * @return true = 对称（mask == 0 视为对称）；false = 不对称
 *
 * @note 用途：RAID / stripe 的 XOR 按 CW 配对，要求两 plane 同集合
 * （见 fc_data_path_generate_stripe_parity 的前置断言）。
 */
BOOL fc_cw_bitmap_is_plane_symmetric(DWORD mask);

/*
 * FC 对外接口声明（FTL / 主循环 / 测试 使用）
 *
 *   fc_interface.h   ：唯一公共头（基础类型 + 命令结构 + API + 布局常量）
 *   fc.c / fc_pool.c ：实现
 *   私有头           ：src/fc/，FTL 不可见
 *
 * 对外接口分组：
 *   ① 初始化            fc_init
 *   ② 命令生命周期       fc_command_alloc / configure_descriptor[_with_buffer] /
 *                        descriptor_release_head / descriptor_chain_release_all /
 *                        fc_submit / fc_submit_batch / fc_poll_completion[_s] /
 *                        fc_command_release
 *   ③ buffer 访问       fc_buffer_alloc / release / get_user_data /
 *                        fc_buffer_get_base / fc_buffer_get_meta
 *   ④ 数据查询          fc_get_cmd_data_ptr / fc_get_cmd_cw_bitmap /
 *                        fc_get_cmd_ecc_max / fc_get_cmd_all_cw_empty
 *   ⑤ 查询工具          fc_command_id_to_command / fc_descriptor_id_to_descriptor /
 *                        fc_get_resource_stats / fc_debug_state_get
 *   ⑥ 服务入口          fc_service
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数；句柄取 cmd->cmd_id → 逐条
 *   fc_command_configure_descriptor(cmd_id, ...) 组装 desc 链
 *   （FTL 侧「按掩码一次性建链」见 ftl_cmd_configure_descriptors()）
 *   → fc_submit(cmd_id) → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release(cmd_id) 回收。
 *
 * 边界：FC = 硬件 FCC 的软件替身。**不含策略**——「访问哪些 CW」由 FTL 决定，
 *   「数据搬运」由 FTL 做（fc_command_configure_descriptor + 自己的拷贝），FC 只提供槽位/描述符/队列/数据路径。
 */

/* ── 功能特性开关 ── */

/*
 * Stripe RAID 功能使能。
 *   0 = 禁用（默认）。RAID 相关代码不编译，fc_cmd_cfg_s.config.raid 固定为 NORMAL。
 *   1 = 启用。跨 Plane RAID XOR parity generation + recovery。
 *
 * 当前未启用（功能代码已完成但未验证，待硬件就绪后开启）。
 */
#ifndef CONFIG_FC_RAID
#    define CONFIG_FC_RAID (0U)
#endif

/* 固定资源池深度 */
#define FC_COMMAND_POOL_DEPTH    (64U)
#define FC_DESCRIPTOR_POOL_DEPTH (256U)
#define FC_BUFFER_POOL_DEPTH     (128U)
#define FC_BUFFER_POOL_CW_SIZE   (CW_SIZE) /* 528B = 每 CW 缓冲大小（= NAND 侧 CW_SIZE） */

/* 命令池 CRIT 预留：L2P region 加载（GC / 写确认的前台依赖）独占 2 槽。
 * 其余后台（CP/GC/journal/metadata）与 host 只能用到 DEPTH - CRIT；
 * host 路径另有独立预算限流（不在本层）。
 * desc/buffer 同理：L2P region 页（2 KiB = 4 个 512B CW）需要 4 组，预留 8 组。 */
#define FC_CMD_RESERVE_CRIT      (2U)
#define FC_CMD_RESERVE_CRIT_DESC (8U)
#define FC_CMD_RESERVE_CRIT_BUFS (8U)

/* 命令分配类：决定当前池占用上限；默认 BG。 */
typedef enum FC_CMD_CLASS_E
{
    FC_CMD_CLASS_BG = 0, /* 后台元数据 / host：上限 DEPTH - CRIT */
    FC_CMD_CLASS_CRIT,   /* L2P load：上限 DEPTH */
    FC_CMD_CLASS_NUM     /* 边界哨兵 */
} fc_cmd_class_e;

/*
 * CW 布局: [META:4][CRC:4][USER:512][PARITY:8] = 528B
 * 对外接口只暴露 USER 数据区，META/CRC/PARITY 由 FC 内部管理。
 */
#define FC_BUFFER_USER_OFFSET (8U)   /* USER 区域在 CW 中的偏移 */
#define FC_BUFFER_USER_SIZE   (512U) /* 暴露给 FTL 的用户数据大小 */
#define FC_BUFFER_DATA_SIZE   (520U) /* META+CRC+USER = 加扰/ECC 处理的数据区大小 */

/* CW 内 META/CRC/USER 区域偏移（FTL 诊断与 FC 数据路径共用） */
#define FC_CRC_META_SIZE   (4U)
#define FC_CRC_FIELD_SIZE  (4U)
#define FC_CRC_USER_SIZE   (512U)
#define FC_CRC_USER_OFFSET (FC_CRC_META_SIZE + FC_CRC_FIELD_SIZE) /* = 8 */

/* 队列深度 */
#define FC_SQ_DEPTH  (64U)
#define FC_HSQ_DEPTH (16U)
#define FC_CQ_DEPTH  (64U)

#ifdef __cplusplus
extern "C"
{
#endif

    /* fc_resource_stats_s 是公共资源统计类型 */

    /* ═══════ 启动编排（main 专用） ═══════ */
    /**
     * @brief 初始化 NAND 芯片并装配 FC 的 per-chip 后端。
     *
     * @note 只允许在 fc_init() 之后、放行命令队列之前调用。
     */
    nand_result_e fc_nand_init(void);

    /**
     * @brief 全部初始化完成后切换到高速时序。
     *
     * @param fmc_hz FMC 时钟频率（Hz）。
     * @return 成功返回 NAND_RESULT_OK；失败保持低速时序。
     */
    nand_result_e fc_nand_enable_high_speed(DWORD fmc_hz);

    /** @brief 放行 NAND 命令队列（原 fc_nand_engine_set_armed(1)）。 */
    void fc_boot_arm(void);

    /* ═══════ ① 初始化 ═══════ */
    /**
     * @brief 初始化所有资源池和队列（幂等，可重复调用以重置）。
     *
     * @note 调用前须确保 FC 已 quiesce；本函数会清空在途命令/完成并 disarm NAND 引擎，
     *       运行中调用会丢失未完成命令。初始化没有失败路径（CRC 自检失败直接
     *       ASSERT），故不返回状态码。
     */
    void fc_init(void);

    /* ═══════ ② 命令生命周期 ═══════ */
    /**
     * @brief 申请一个命令槽位（满池额度；测试 / RAID parity 入口）。
     *
     * 命令句柄 = 返回对象的 `cmd_id` 字段。生产 FTL 用 fc_command_alloc_class()
     * 区分 BG / CRIT 额度。
     *
     * @return 命令指针（供调用者填 config/route/paa）；池空时返回 NULL。
     */
    fc_cmd_s *fc_command_alloc(void);

    /**
     * @brief 按分配类申请命令槽位；类决定当前池占用上限。
     *
     * @param cls 分配类（FC_CMD_CLASS_*）
     * @return 命令指针；该类额度用完时返回 NULL
     */
    fc_cmd_s *fc_command_alloc_class(fc_cmd_class_e cls);

    /**
     * @brief 逐条追加一个 (plane, cw) 到命令的 desc 链尾（分配 desc + buffer、填 PAA/config/meta）。
     * **建链的唯一入口**：按掩码一次性建链的策略层在 FTL（见
     * ftl_cmd_configure_descriptors()），本函数即机制体。
     * 上层自行组装 CW 集合（例如只读离散 CW）：
     * 按自己算出的集合循环调用即可，
     * **链序即数据布局顺序**（FTL 侧拷贝按此顺序遍历链，见 ftl_cmd_read_data）。
     * row_paa = 该 plane 的行地址（**PAA**：page/block/lun，plane/cw 位由实参决定），
     * 其 plane 位必须等于 plane。本入口收 PAA——FC 层内没有 PBA，
     * 调用方（FTL）自行 `pba_to_paa()` 后传入。
     * 由此 multi-plane 命令的**两个 plane 可以指向不同 block**（Policy B：每 plane 独立分配块 +
     * 坏块替换）——每个 plane 传自己的 row_paa 即可；同一 plane 的所有 CW 必须传同一 row_paa。
     * plane 必须落在本命令的 plane 段内（单 plane = paa_to_plane(command->paa) 那一个；
     * multi-plane = [0, multi_plane)）。
     * 前置：命令处于 ALLOCATED；plane 落在本命令 plane 段内；CW 号 < CW_NUM；
     * row_paa 的 plane 位 == plane。命令 ID 越界、命令不在 ALLOCATED：
     * debug 停机，release 返回 FC_STATUS_INVALID_ARGUMENT；plane/CW/row_paa 坐标
     * 只在 debug 检查，提交边界 fc_pool_chain_validate 兜底。配置错误绝不发到硬件。
     *
     * @param cmd_id 目标命令 ID；须已 ALLOCATED。
     * @param plane   CW 所在 plane；须落在本命令的 plane 段内。
     * @param cw      CW 号；须 < CW_NUM。
     * @param row_paa 该 plane 的行地址（PAA）；其 plane 位必须等于 plane。
     *
     * @return FC_STATUS_OK             建链成功。
     *         FC_STATUS_RESOURCE_EMPTY desc 或 buffer 池空——只有「申请不到」才走返回码（模仿硬件行为）。
     *         FC_STATUS_INVALID_ARGUMENT 命令 ID 越界 / 命令不在 ALLOCATED
     *                                  （debug ASSERT；release 判据 9 闸门）。
     *                                  **失败时链上可能已有 desc（含本条）**：本层不回滚，「已分配的 desc 全部在链上」是不变量，调用方沿链释放即完整回收（见下两个函数）。
     */
    fc_status_e fc_command_configure_descriptor(fc_cmd_id_t cmd_id, BYTE plane, BYTE cw, DWORD row_paa);

    /**
     * @brief 建链入口（**上层申请 buffer**）——与上一个的唯一差别是 buffer 从哪来。
     * 仅当 command->config.buf_own 的申请位 = 上层申请（FC_BUF_OWN_ALLOC_BY_UPPER，即 10/11）
     * 时合法：调用者须先 `fc_buffer_alloc()` 拿到 buffer_id 再传进来，FC 建链时**不申请**。
     * buf_own = FC 申请（00/01）时必须传 FC_BUF_ID_INVALID（否则 release 返回 INVALID_ARGUMENT）。
     * 其余前置与失败语义与 fc_command_configure_descriptor 一致。
     *
     * @param cmd_id    目标命令 ID；须已 ALLOCATED。
     * @param plane     CW 所在 plane；须落在本命令的 plane 段内。
     * @param cw        CW 号；须 < CW_NUM。
     * @param row_paa   该 plane 的行地址（PAA）；其 plane 位必须等于 plane。
     * @param buffer_id 上层已申请的 buffer ID；buf_own 为 FC 申请时须传 FC_BUF_ID_INVALID。
     *
     * @return FC_STATUS_OK             建链成功。
     *         FC_STATUS_RESOURCE_EMPTY desc 池空，或 FC 申请 buffer 时 buffer 池空。
     *         FC_STATUS_INVALID_ARGUMENT 命令 ID 越界 / 命令不在 ALLOCATED /
     *                                    上层 buffer ID 越界或归属不符
     *                                    （debug ASSERT；release 判据 9 闸门）。
     */
    fc_status_e fc_command_configure_descriptor_with_buffer(fc_cmd_id_t cmd_id, BYTE plane, BYTE cw, DWORD row_paa, fc_buf_id_t buffer_id);

    /**
     * @brief 释放链头那一个 desc（含其 buffer），返回新链头；空链（desc_id 越界）原样返回。
     * 只能传**链头**：释放链中间节点会让前驱的 next_id 悬空。
     * command->desc_id 是位域取不了地址，故按值传入、按值取回。
     * 用法：command->desc_id = fc_descriptor_release_head(command->desc_id);
     *
     * @param desc_id 当前链头描述符 ID。
     *
     * @return 释放后的新链头；空链（越界）时原样返回 desc_id。
     */
    fc_desc_id_t fc_descriptor_release_head(fc_desc_id_t desc_id);

    /**
     * @brief 沿链释放**这条命令**的全部 desc + buffer，返回 FC_DESC_ID_INVALID（恒为空链头）。
     * "all" 指**本命令的 desc 链**（= 该命令已申请的全部资源），**不是**清空 desc/buffer 池，
     * 也不触及别的命令——链本身就是这条命令的资源集合（唯一真值）。
     * 建链中途失败的回滚入口——释放后命令只剩命令槽位，由 fc_command_release 收尾。
     * 用法：command->desc_id = fc_descriptor_chain_release_all(command->desc_id);
     *
     * @param head 当前链头 ID。
     *
     * @return 恒为 FC_DESC_ID_INVALID（空链头）。
     */
    fc_desc_id_t fc_descriptor_chain_release_all(fc_desc_id_t head);

    /**
     * @brief 提交到 SQ 或 HSQ（根据 high_priority），入队即返回。
     * 前置：指针在命令池内；状态 ALLOCATED；validate 通过；desc 链完整
     * （归属 / plane 段 / CW 唯一 / buffer 占用 / 行一致 / multi-plane 覆盖）。
     * 违反时 debug ASSERT；release 返回 FC_STATUS_INVALID_ARGUMENT（判据 9 闸门）。
     *
     * @param cmd_id 待提交命令 ID；须在池内且状态 ALLOCATED。
     *
     * @return FC_STATUS_OK         入队成功。
     *         FC_STATUS_QUEUE_FULL 目标队列满（资源，模仿硬件行为）。
     *         FC_STATUS_INVALID_ARGUMENT ID/状态/链校验失败（release）。
     */
    fc_status_e fc_submit(fc_cmd_id_t cmd_id);

    /**
     * @brief 批量提交：n 条命令要么全进 SQ/HSQ，要么一条都不进。
     * 供 two-plane cache program 批次使用——不允许“前几条已提交、末条失败”留下
     * 未终结的 cache sequence。前置与 fc_submit 相同。
     *
     * 链契约：批内只要含 `FC_CHAIN_MORE` 成员，全批必须同优先级（进同一条队列），
     * 否则链会被拆到 SQ/HSQ 两段（序列错乱）。末条**允许** `FC_CHAIN_MORE`——
     * 这是流式序列的中间窗口；FTL 的序列状态负责在 run_end 发 `FC_CHAIN_END`，
     * `chain_owner` 跨窗口保留。
     *
     * @param cmd_ids 命令 ID 数组，长度为 n；批内 ID 须**互不相同**。
     * @param n       命令条数；须 > 0。
     *
     * @return FC_STATUS_OK               全部入队。
     *         FC_STATUS_QUEUE_FULL       SQ 或 HSQ 余量不足（一条都不入队）。
     *         FC_STATUS_INVALID_ARGUMENT 批内含 chain 成员但优先级不一致；
     *                                    批内有重复命令 ID；或某条命令的 ID/
     *                                    状态/链校验失败（release）。
     */
    fc_status_e fc_submit_batch(const fc_cmd_id_t cmd_ids[], DWORD n);

    /**
     * @brief 调度执行（上层 while(1) 轮询调用）。
     *
     * @return FC_STATUS_OK          本轮有完成被消费，或有命令被下发。
     *         FC_STATUS_QUEUE_EMPTY 无完成且 SQ/HSQ 均为空。
     *         FC_STATUS_NOT_READY   无完成，队列非空但当前无 LUN 可下发（背压）。
     *         FC_STATUS_IO_ERROR    CQ 满、时间源不可用或命令下发失败。
     */
    fc_status_e fc_service(void);

    /**
     * @brief 从 CQ 取一条完成项。
     *
     * @param completion 完成项出参；须非空。
     *
     * @return FC_STATUS_OK          取到一条完成项。
     *         FC_STATUS_QUEUE_EMPTY CQ 为空。
     */
    fc_status_e fc_poll_completion(fc_cmd_status_s *completion);

    /**
     * @brief 从 CQ 批量取完成项：最多取 max_count 条，实际条数写入 *count。
     * **上限由调用方给**（FC 不写死任何批量常量）——常见取值是 fc_nand_dispatch_poll
     * 的推进上限 CH_NUM×CE_NUM×LUN_NUM，与单轮调度对齐；也可是任意 K。
     * 前置：completions 与 count 非空（ASSERT）；max_count 须 > 0
     * （debug ASSERT + release FC_STATUS_INVALID_ARGUMENT）。
     * max_count 给多大都不会越界——循环被 CQ 占用数（≤ FC_CQ_DEPTH）天然封顶。
     *
     * @param completions 完成项输出数组；须非空。
     * @param max_count   最多取多少条；须 > 0。
     * @param count       实际取到的条数（出参）；须非空。
     *
     * @return FC_STATUS_OK             取到 ≥1 条。
     *         FC_STATUS_QUEUE_EMPTY    一条没取到（CQ 为空）。
     *         FC_STATUS_INVALID_ARGUMENT max_count == 0（release 拒绝）。
     */
    fc_status_e fc_poll_completions(fc_cmd_status_s *completions, WORD max_count, WORD *count);

    /**
     * @brief 释放命令及关联 Descriptor 链 + 各 CW buffer（FREE 幂等）。
     *
     * ID 越界、或命令仍在飞（QUEUED/ACTIVE）：debug ASSERT；release 由池闸门
     * 拒绝，不释放任何资源。生命周期由调用链保证（未提交 / 完成项已取走）。
     *
     * @param cmd_id 待释放命令 ID；须为池内槽位。
     */
    void fc_command_release(fc_cmd_id_t cmd_id);

    /* ═══════ ③ buffer 访问 ═══════ */
    /**
     * @brief 申请 Buffer（返回 ID），失败返回 FC_BUF_ID_INVALID。
     * ⚠️ 手动分配接口：常规路径由 fc_command_configure_descriptor() 为每个 CW 自动分配 desc + buffer。
     *
     * @return buffer ID；buffer 池空时返回 FC_BUF_ID_INVALID。
     */
    fc_buf_id_t fc_buffer_alloc(void);

    /**
     * @brief 释放上层预申请的 Buffer（尚未挂到命令上）。
     *
     * 越界 / 未占用 / 仍归属某条命令：debug ASSERT；release 由池闸门拒绝。
     * 已挂链 buffer 必须随命令释放（fc_command_release），不能在这里释放。
     *
     * @param buffer_id 待释放的 buffer ID。
     */
    void fc_buffer_release(fc_buf_id_t buffer_id);

    /**
     * @brief ID→指针，返回 USER 数据区首地址（跳过 META+CRC 共 8B）。
     *
     * @param buffer_id 已占用的 buffer ID。
     *
     * @return USER 数据区首地址（CW 基址 + FC_BUFFER_USER_OFFSET，恒非空）。
     */
    BYTE *fc_buffer_get_user_data(fc_buf_id_t buffer_id);

    /**
     * @brief FC 内部使用：返回 CW 基址（META 开头，528B 完整 CW）。
     * 数据路径（加扰/ECC/RAID）使用此接口。
     *
     * @param buffer_id 已占用的 buffer ID。
     *
     * @return CW 基址指针（恒非空）。
     */
    BYTE *fc_buffer_get_base(fc_buf_id_t buffer_id);

    /**
     * @brief 返回 CW 中 META 字段指针（4 字节，位于 CW 基址偏移 0）。
     * META 包含 FAA/PAA 信息，由 FC 在 program 前自动填写。
     * FTL/Test 在 read 完成后可通过此接口验证物理地址一致性。
     * cw_index 是 **CW 号**（按 paa_to_cw(desc->paa) 匹配），不是链走位。
     * ⚠ 仅适用于**单 plane**命令：multi-plane 命令同一 CW 号在两个 plane 各有一条
     *   desc，本接口无法用它唯一定位 ⇒ release 返回 NULL（debug ASSERT）；
     *   multi-plane 的逐 CW 读取请沿 desc 链遍历。
     *
     * @param cmd_id 命令 ID；须为池内槽位。
     * @param cw_index CW 号。
     *
     * @return META 字段指针（4 字节）；链上无该 CW、该 desc 没有 buffer，
     *         或命令为 multi-plane 时返回 NULL。
     */
    BYTE *fc_buffer_get_meta(fc_cmd_id_t cmd_id, BYTE cw_index);

    /* ═══════ ④ 数据查询 ═══════ */
    /**
     * @brief 获取命令中 **CW 号 == cw_index** 的 CW 的 buffer 指针（读操作后使用）。
     * 不在链上返回 NULL。
     * ⚠ 仅适用于**单 plane**命令：multi-plane 命令同一 CW 号在两个 plane 各有一条
     *   desc，本接口无法用它唯一定位 ⇒ release 返回 NULL（debug ASSERT）；
     *   multi-plane 的逐 CW 读取请沿 desc 链遍历（如 ftl_cmd_read_data）。
     *
     * @param cmd_id 命令 ID；须为池内槽位且 desc 链非空。
     * @param cw_index CW 号。
     *
     * @return 该 CW 的 USER 数据指针；不在链上、该 desc 无 buffer，
     *         或命令为 multi-plane 时返回 NULL。
     */
    BYTE *fc_get_cmd_data_ptr(fc_cmd_id_t cmd_id, BYTE cw_index);

    /**
     * @brief 从 desc 链重建命令的 flat CW 掩码（bit n = plane*CW_NUM + cw；无 desc 时 0）。
     * **链是 CW 集合的唯一真值**——FC 内没有掩码输入，需要"掩码形状"的
     * 判定（如 RAID 的 plane 对称性）一律走这里按链重建。
     *
     * @param cmd_id 命令 ID；须为池内槽位。
     *
     * @return flat CW 掩码；无 desc 时为 0。
     */
    DWORD fc_get_cmd_cw_bitmap(fc_cmd_id_t cmd_id);

    /**
     * @brief 命令级聚合 ECC：desc 链上所有 CW 的 ecc_bits 最大值。
     * 须在 fc_command_release() 之前调用（desc 释放后失效）。
     *
     * @param cmd_id 命令 ID；须为池内槽位。
     *
     * @return desc 链上所有 CW 的 ecc_bits 最大值；无 desc 时为 0。
     */
    BYTE fc_get_cmd_ecc_max(fc_cmd_id_t cmd_id);

    /**
     * @brief 命令 desc 链上是否存在 UNC（不可纠 ECC）CW
     *
     * 须在 fc_command_release 之前调用。
     *
     * @param cmd_id 命令 ID
     * @return true = 至少一个 CW 为 UNC
     */
    BOOL fc_get_cmd_has_unc(fc_cmd_id_t cmd_id);

    /**
     * @brief 命令读到的所有 CW 是否都是**擦除页**（desc 链上每个 CW 都是 FC_CW_STATUS_EMPTY）。
     *
     * 这是「这一页编程过没有」的**唯一权威判据**：空/非空的依据（ECC 是否成码字、
     * parity 是否写成）只有 FC 看得见，FTL 只拿得到 USER 区。
     * 须在 fc_command_release() 之前调用（desc 释放后失效）。
     *
     * @param cmd_id 命令 ID；须为池内槽位，且是读命令（写命令的 desc 状态不表示介质内容）。
     *
     * @return true  = 链上每个 CW 都判为擦除页
     *         false = 至少一个 CW 不是（含解码成功 / UNC / 读失败），
     *                 或 desc 链为空（没有 CW 就没有证据）
     */
    BOOL fc_get_cmd_all_cw_empty(fc_cmd_id_t cmd_id);

    /* ═══════ ⑤ 查询工具 ═══════ */
    /**
     * @brief ID→指针转换。前置条件：cmd_id 为 fc_command_alloc() 分配出的池内槽位号。
     *
     * @param cmd_id 池内命令槽位号。
     *
     * @return 命令指针（恒非空）。
     */
    fc_cmd_s *fc_command_id_to_command(fc_cmd_id_t cmd_id);

    /**
     * @brief ID→指针转换。前置条件：descriptor_id 为命令 desc 链中的池内 ID。
     *
     * @param descriptor_id 池内描述符 ID。
     *
     * @return 描述符指针（恒非空）。
     */
    fc_desc_s *fc_descriptor_id_to_descriptor(fc_desc_id_t descriptor_id);

    /* ── 诊断：队列头 / 链锁快照（只读，不改状态）──
     * 排障用：用例/运维在「FTL 回不到 idle」时打印它，看是哪个队列的头卡住、
     * 以及该 LUN 的链锁归谁（chain_owner）。生产路径不调用。 */
    typedef struct FC_DEBUG_STATE_S
    {
        WORD  sq_used;
        WORD  hsq_used;
        BYTE  chain_owner; /* 0=NONE / 1=SQ / 2=HSQ */
        WORD  sq_head_id;
        WORD  sq_head_chain;
        BYTE  sq_head_op;
        WORD  hsq_head_id;
        WORD  hsq_head_chain;
        BYTE  hsq_head_op;
        BYTE  lun_used;       /* NAND per-LUN FIFO 已用槽 */
        BYTE  lun_phase;      /* NAND per-LUN FSM 相位（LUN_PHASE_*） */
        DWORD fill_calls;     /* FC 填充轮次累计调用次数 */
        DWORD fill_pops;      /* 填充轮次累计从队列 pop 的命令数 */
        BYTE  sq_head_state;  /* SQ 头命令的状态：0=FREE 1=ALLOCATED 2=QUEUED 3=ACTIVE 4=COMPLETED */
        WORD  sched_sq_count; /* 调度器实际扫描的 SQ 的 count（应与 sq_used 相同） */
        BYTE  sched_sq_bound; /* 1 = scheduler->sq/hsq 就绑在 controller->sq/hsq 上 */
    } fc_debug_state_s;

    /**
     * @brief 取队列头 / 链锁快照（只读）。
     *
     * @param out 出参；须非空。
     */
    void fc_debug_state_get(fc_debug_state_s *out);

#if SSD_STATS_ENABLE
    /**
     * @brief FC 侧进展签名（主循环空转判据）。
     *
     * = 已消费完成数 + 有派发的填充轮数。只用于比较两次采样是否变化；
     * u32 回绕不影响相等比较。
     *
     * @return 进展签名
     */
    DWORD fc_stats_progress(void);
#endif

    /**
     * @brief 资源使用统计（测试/监控用）。
     *
     * used 字段来自分配/释放闸门的 O(1) 计数；high 字段是历史最高占用（高水位）。
     * debug 档会额外扫描全池校验计数与真值一致，release 档只读计数器。
     *
     * @note **不得用于断言/故障路径**：debug 扫描会 ASSERT，计数不一致时会二次
     *       断言并让终端递归（见 assertion_platform_context）。
     *
     * @param stats 统计结果出参；须非空。
     */
    void fc_get_resource_stats(fc_resource_stats_s *stats);

    /** @brief 开机横幅：打印 FC 固定资源池/队列深度、通道拓扑与数据通路配置（全编译期常量）。 */
    void fc_boot_report(void);

#ifdef __cplusplus
}
#endif

#if defined(FC_NAND_TEST_ENABLE)
/**
 * @brief 测试专用：对 ALLOCATED 命令喂完成，验证完成侧 ACTIVE 守卫（SCH-11）。
 *
 * @return fc_complete_from_nand 的返回码
 */
fc_status_e fc_test_complete_nonactive(void);

#endif /* FC_NAND_TEST_ENABLE */

#endif /* FC_INTERFACE_H */
