#ifndef FC_COMMAND_H
#define FC_COMMAND_H

#include "assertion.h"
#include "fc_types.h"
#include "nand_config.h" /* PLANE_NUM / CW_NUM / paa_to_plane */
#include "ssd_types.h"   /* DWORD（命令/描述符位域的 32 位字） */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * fc_command.h — FC 命令 / Descriptor / CQ 结构体
 *
 * 资源模型：
 *   - Command（fc_cmd_s）      ：一条 NVMe/FTL 提交的命令，对应 SQ 槽位。
 *   - Descriptor（fc_desc_s）：一个 CW 的数据描述（PAA + buffer + ECC 状态），
 *                                    命令经 descriptor 链串联（next_id 链接）。
 *   - CQ 完成项（fc_cmd_status_s）：命令级聚合状态 + 命令级返回段（fc_param_u）。
 *
 * 三池深度见 fc.h（FC_COMMAND_POOL_DEPTH / FC_DESCRIPTOR_POOL_DEPTH /
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
 * 就会把整块当坏块退役（RISK-GC-11）。**不属于 FC_CW_STATUS_ERROR_MASK**：
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

#endif /* FC_COMMAND_H */
