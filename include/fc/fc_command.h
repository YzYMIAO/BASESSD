#ifndef FC_COMMAND_H
#define FC_COMMAND_H

#include "assertion.h"
#include "fc_types.h"
#include "nand_config.h" /* PLANE_NUM / CW_NUM / paa_to_plane */
#include "ssd_types.h"   /* DWORD（命令/描述符位域的 32 位字） */

#include <stdbool.h>
#include <stdint.h>

/*
 * fc_command.h — FC 命令 / Descriptor / CQ 结构体
 *
 * 资源模型：
 *   - Command（fc_command_t）      ：一条 NVMe/FTL 提交的命令，对应 SQ 槽位。
 *   - Descriptor（fc_descriptor_t）：一个 CW 的数据描述（PAA + buffer + ECC 状态），
 *                                    命令经 descriptor 链串联（next_id 链接）。
 *   - CQ 完成项（fc_command_status_t）：命令级聚合状态 + 命令级返回段（fc_param_t）。
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
 * FC 层只认 PAA 喂 NAND**。PBA 不再跨进 FC——它带 ch/ce，而 FC 的路由本就独立走
 * `config.ch/ce/lun`（见 fc_build_dispatch_request），两套编码并存只会制造静默算错地址的机会。
 * ⚠ 代价：PBA 与 LBA 同构时「`desc->paa` 与 `desc->meta` 用同一个 plane_cw 公式」的便利消失——
 * PAA 的 plane 与 cw **不相邻**（`[cw][page][plane][block]`），建链改用
 * `paa_with_plane()` / `paa_with_cw()`；`PLANE_CW_MASK` 技巧只在 LBA/PBA 空间有效。
 * 三者的低 3 位（PLANE_CW_MASK）仍是同一套编码 [plane:1][cw:2]。
 */

/* ECC 解码模式（配置写入 fc_cmd_conf_t.ldpc） */
typedef enum
{
    FC_ECC_MODE_BCH       = 0U, /* BCH encode/decode */
    FC_ECC_MODE_LDPC_HARD = 1U, /* LDPC hard-decision */
    FC_ECC_MODE_LDPC_SOFT = 2U, /* LDPC soft-decision */
    FC_ECC_MODE_LDPC_RAW  = 3U, /* LDPC raw (no decode) */
} fc_ecc_mode_e;

/* RAID 模式（配置写入 fc_cmd_conf_t.raid） */
typedef enum
{
    FC_RAID_MODE_NORMAL  = 0U, /* 无 RAID */
    FC_RAID_MODE_DECODE  = 1U, /* RAID 解码（ECC 失败时恢复） */
    FC_RAID_MODE_ENCODE  = 2U, /* RAID 编码（生成 parity） */
    FC_RAID_MODE_RECOVER = 3U, /* RAID 恢复（同 decode） */
    FC_RAID_MODE_CHECK   = 4U, /* RAID 校验（暂未实现） */
} fc_raid_mode_e;

/* NAND 页类型（TLC/QLC 多页编程时区分；SLC 只有 LP） */
typedef enum
{
    FC_PAGE_TYPE_LP = 0U, /* Lower Page (SLC page / TLC LP) */
    FC_PAGE_TYPE_MP = 1U, /* Middle Page (TLC MP) */
    FC_PAGE_TYPE_UP = 2U, /* Upper Page (TLC UP) */
    FC_PAGE_TYPE_TP = 3U, /* Triple Page (QLC TP) */
} fc_page_type_e;

/* CRC 校验开关（配置写入 fc_cmd_conf_t.crc） */
typedef enum
{
    FC_CRC_ENABLE  = 0U, /* CRC 校验使能 */
    FC_CRC_DISABLE = 1U, /* CRC 校验禁用 */
} fc_crc_mode_e;

/* 数据加扰开关（配置写入 fc_cmd_conf_t.random） */
typedef enum
{
    FC_RANDOM_ENABLE  = 0U, /* 数据加扰使能 */
    FC_RANDOM_DISABLE = 1U, /* 数据加扰禁用 */
} fc_random_mode_e;

/* 链式命令标记（配置写入 fc_cmd_conf_t.chain） */
typedef enum
{
    FC_CHAIN_END  = 0U, /* 链结束，单命令 */
    FC_CHAIN_MORE = 1U, /* 链中间，后续还有命令 */
} fc_chain_mode_e;

/* buffer 归属（配置写入 fc_cmd_conf_t.buf_own）：本命令的数据 buffer **谁申请、谁释放**。
 *
 * bit1 = 申请方（0 = FC 申请，1 = 上层申请）；bit0 = 释放方（0 = 上层释放，1 = FC 释放）。
 * 「上层释放」= 上层调 fc_command_release() 归还（desc 链即命令的资源集，FC 一并归还链上
 * 所有未归还的 buffer）；「FC 释放」= FC 在命令完成时当场归还（并把 desc 的 buffer_id 抹成
 * INVALID，故之后 fc_command_release 不会重复归还）。
 *
 * **`00` 是默认值 = 既有生产行为**（FC 建链时申请、上层 release 时归还）：命令出池整体清零
 * ⇒ 未显式设置本字段的命令恒走 `00`，FTL 生产路径逐字不变。 */
typedef enum
{
    FC_BUF_OWN_FC_ALLOC_UPPER_FREE    = 0U, /* 00：FC 申请，上层释放——读路径，上层拿 buffer 并掌控生命周期（默认） */
    FC_BUF_OWN_FC_ALLOC_FC_FREE       = 1U, /* 01：FC 申请，FC 释放——读，只要命令状态 */
    FC_BUF_OWN_UPPER_ALLOC_UPPER_FREE = 2U, /* 10：上层申请，上层释放——上层把控数据场景 */
    FC_BUF_OWN_UPPER_ALLOC_FC_FREE    = 3U, /* 11：上层申请，FC 释放——写命令等，只要命令状态 */
} fc_buf_own_e;

#define FC_BUF_OWN_ALLOC_BY_UPPER (0x2U) /* bit1：1 = 上层申请（上层自行 fc_buffer_alloc 并传入） */
#define FC_BUF_OWN_FREE_BY_FC     (0x1U) /* bit0：1 = FC 在完成时归还 */

/* 命令优先级（选择写入 SQ 还是 HSQ） */
typedef enum
{
    FC_PRIORITY_SQ  = 0U, /* 普通优先级（Submission Queue） */
    FC_PRIORITY_HSQ = 1U, /* 高优先级（High-priority SQ） */
} fc_priority_e;

/* ⚠ 本头文件**不声明**"完成路由归属模块"枚举。
 *
 * 它是 FTL 的概念（哪个功能拥有这条命令），不该由 FC 层声明。FC 只认
 * `fc_command_t.op_tag` 这个**不透明 7 位整数**：不解释、不分支，完成时原样拷进
 * `fc_command_status_t.op_tag`。语义（= FTL 的 `ftl_op_id_e`）由 FTL 侧定义，
 * 见 include/ftl/ftl_op.h。 */

/*
| 状态 | 汇总规则 |
|---|---|
| `NAND_FAIL` | 任意 CW 置位即置位 |
| `TIMEOUT` | 任意 CW 置位即置位 |
| `CRC_ERROR` | 任意 CW 置位即置位 |
| `UNC` | 任意 CW 置位即置位 |
| `MATA_ERROR` | 任意 CW 置位即置位 |
| `RAID_ERROR` | 任意 CW 置位即置位 |
| `READY` | 建议所有有效 CW 都 Ready 后才算命令 Ready |
| `PROTECTED` | 任意 CW 或目标块受保护即置位 |
| `BLANK` | 任意 CW 置位即置位；**不是错误位**——表示该 CW 物理上是擦除页 |
*/
/* 每 CW 状态位：命令级聚合状态 = 各 CW 状态按上表规则 OR 合并 */
#define FC_CW_STATUS_NAND_FAIL  (1U << 0)
#define FC_CW_STATUS_TIMEOUT    (1U << 1)
#define FC_CW_STATUS_CRC_ERROR  (1U << 2)
#define FC_CW_STATUS_UNC        (1U << 3)
#define FC_CW_STATUS_MATA_ERROR (1U << 4)
#define FC_CW_STATUS_RAID_ERROR (1U << 5)
#define FC_CW_STATUS_READY      (1U << 6)
#define FC_CW_STATUS_PROTECTED  (1U << 7)
/* 擦除页（erased page）：整条 CW（META+CRC+USER+ECC）全 0xFF。
 * 全 0xFF 不是合法 BCH 码字 ⇒ 解码必然报 UNC；若不单独识别，GC 读到半写块的空洞页
 * 就会把整块当坏块退役（RISK-GC-11）。**不属于 FC_CW_STATUS_ERROR_MASK**：
 * 「页从未编程」是介质的合法状态，不是读错误。 */
#define FC_CW_STATUS_BLANK   (1U << 8)
#define FC_CW_STATUS_UNUSED  (0U)
#define FC_CW_STATUS_SUCCESS (FC_CW_STATUS_READY)

/* 「失败位」的规范集合——上表六个错误位的并集（READY / PROTECTED / UNUSED 不算失败）。
 * 它是失败判定的**唯一定义**：fc_complete_from_nand()（fc.c）用它挑出**首个失败 CW**
 * 的 desc_id（兑现 fc_command_status_t.desc_id 的 "first-failing desc on error"），
 * 其余消费方一律经 `fc_cw_status_has_error()`。不要就地重列位——六位散落多处时
 * 漏一位不会有编译错误。 */
#define FC_CW_STATUS_ERROR_MASK (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR | FC_CW_STATUS_UNC | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)

/**
 * @brief 命令级聚合状态是否含任一错误位。
 * 读路径的成败判据 = 本函数（单看 NAND_FAIL 会把 UNC / MATA_ERROR 当成功，
 * 页内容不可信却照常搬出，见 ftl_sync_read_page 的注释）。
 */
static inline bool fc_cw_status_has_error(DWORD status)
{
    return (status & FC_CW_STATUS_ERROR_MASK) != 0U;
}

/**
 * @brief 该 CW 是否被判定为擦除页（未编程）。与 has_error 正交：
 * 擦除页既不表示读成功，也不表示读失败——页上没有任何数据。
 */
static inline bool fc_cw_status_is_blank(DWORD status)
{
    return (status & FC_CW_STATUS_BLANK) != 0U;
}

/**
 * @brief 把 FC_CW_STATUS_* 位解码成短字符串（日志用）
 *
 * 输出例："READY|UNC"、"NAND|TIMEOUT"；未知位以 '?' 表示。
 * @param status 完成状态（FC_CW_STATUS_* 位或）
 * @param buf    输出缓冲；非空
 * @param buflen 缓冲长度
 * @return buf（便于直接放进 LOG 实参）
 */
const char *fc_cw_status_str(DWORD status, char *buf, DWORD buflen);

/* Command 与 Descriptor 共用 32 位操作配置（位域布局见下） */
typedef struct fc_cmd_conf_t
{
    DWORD operation : 8;   /* ONFI operation macro (OP_*). */
    DWORD ch        : 2;   /* NAND channel number. */
    DWORD ce        : 2;   /* NAND chip-enable number. */
    DWORD lun       : 2;   /* NAND LUN number. */
    DWORD cw        : 2;   /* Command: 每-plane CW 数减一。**FTL 建链不读它**
                            *          （链长只由建链形参 cw_bitmap 的置位数决定）；当前读者只剩
                            *          FC 内 RAID stripe 的 cw_count 与测试，写者保留；
                            * Descriptor: 本 desc 在 plane 内是第几个 CW（0..CW_NUM-1）。 */
    DWORD crc         : 1; /* FC_CRC_ENABLE / FC_CRC_DISABLE. */
    DWORD ldpc        : 2; /* fc_ecc_mode_e: BCH/LDPC_HARD/LDPC_SOFT/LDPC_RAW. */
    DWORD raid        : 3; /* fc_raid_mode_e: NORMAL/DECODE/ENCODE/RECOVER/CHECK. */
    DWORD random      : 1; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE. */
    DWORD chain       : 1; /* FC_CHAIN_END / FC_CHAIN_MORE. */
    DWORD page_type   : 2; /* fc_page_type_e: LP/MP/UP/TP. */
    DWORD multi_plane : 3; /* 0=single; N=N-plane。总 desc 数以 desc 链长度为真值，不由本字段推导。 */
    DWORD buf_own     : 2; /* fc_buf_own_e：本命令数据 buffer 的归属（bit1=申请方 0=FC 1=上层；
                            *                 bit0=释放方 0=上层 1=FC）。默认 0 = 既有行为。 */
    DWORD reserved : 1;    /* Reserved for future configuration. */
} fc_cmd_conf_t;

/*
 * 单 CW 描述符：物理地址 + 配置 + 缓冲 ID + 链 ID + 状态 + ECC 信息。
 * 一条命令的 desc 链 = 本次要访问的 CW 逐条排列（plane-major），next_id 串联；
 * 0xFFFF 终止。**链长度即真值**：uniform 命令 = multi_plane × 每-plane CW 数，
 * flat 掩码命令 = 掩码置位数。凡需要"这条命令有几个 CW / 每 plane 几个"一律走链。
 */
struct fc_descriptor_t
{
    DWORD paa;                    /* 本 CW 的**芯片内地址（PAA）**；建链时由 row_paa + plane/cw 造出
                                   * （`paa_with_cw(paa_with_plane(row_paa, plane), cw)`），
                                   * 直接喂 NAND 请求。与 command->paa 同空间（都是 PAA）。 */
    fc_cmd_conf_t config;         /* Copy of Command configuration; config.cw is this CW index. */
    DWORD         buffer_id : 16; /* Buffer Pool ID; 0xFFFF means invalid. */
    DWORD         next_id   : 16; /* Next Descriptor ID; 0xFFFF terminates the chain. */
    DWORD         meta;           /* 本 CW 的**扇区 LBA**：建链时由 command->meta 的高位 +
                                   * 本 desc 的 plane/cw 造出（见 fc_command_configure_descriptor）；
                                   * 加扰种子取自它，读后存放 NAND 读回的 META 值。 */

    /* 注意：meta 的高位来自 command->meta（本命令的扇区 LBA），低 3 位
     * [plane][cw] 由本 desc 决定——两者同一套编码（PLANE_CW_MASK）。 */
    DWORD status   : 9; /* CW status, using FC_CW_STATUS_* bits. */
    DWORD ecc_bits : 8; /* ECC corrected bit count from last completion; 0 if no error. */
};

/*
 * 4 字节命令参数 / 返回段，SQ 与 CQ 共用。只有一种解释：
 *   parameter0..3 / bytes[4]：SET_FEATURES 的内联数据（SQ）/ GET_FEATURES 读回的寄存器值（CQ）
 * 小端（Cortex-M7）下 parameter0 是最低字节。
 *
 * 本联合体**不设** `DWORD cw_bitmap` 视图（flat CW 位图，bit n = plane*CW_NUM + cw）：
 * 请求集与失败集都是**派生量**，不常驻命令结构体：
 *   · 请求集：读者只有 FTL，且它是**建链的输入** ⇒ 由 ftl_cmd_configure_descriptors()
 *     的形参承载。
 *   · 失败集：读者只有上层，上层遍历 desc 链 OR 各 desc->status 即可 ⇒
 *     FC 不代为聚合（见 fc_complete_from_nand）。
 *
 * ⚠ 别与 NAND 层的 request->cw_bitmap 混淆：那是 **NAND 真正访问的集合**，
 *   由 FC 按 desc 链重新铸出（fc_build_request_data_planes）；本联合体与它无关。
 */
typedef union
{
    struct
    {
        BYTE parameter0; /* Command parameter byte 0. */
        BYTE parameter1; /* Command parameter byte 1. */
        BYTE parameter2; /* Command parameter byte 2. */
        BYTE parameter3; /* Command parameter byte 3. */
    };
    BYTE bytes[4]; /* 内联数据的字节视图。 */
} fc_param_t;

/*
 * SQ 命令：被 FC 持有直至完成。desc_id = 首个 Descriptor ID（0xFFFF = 无描述符）。
 *
 * **本结构刻意不携带自己的命令池 ID**：ID 由「指针 − command_pool 基址」导出
 * （fc_command_to_id / fc_command_id_to_command 互为逆）。缓存的副本会在
 * fc_command_release 把结构体整体清零时失效，派生值不会；而指针本来就在调用者手里，
 * 再存一份只会带来"两份真值不一致"的机会。
 *
 * ⚠ SQ 在 configure 完成后即不可变：命令配置完就算完成，运行期返回值一律走 CQ。
 */

/* command->meta 的「本命令无已知 LBA」哨兵。
 *
 * 数据命令的 meta 是本命令的**扇区 LBA**；FC 侧 Pass 4 用它交叉校验读回的页内 META
 * （读到了别的页 ⇒ MATA_ERROR）。但 GC / 掉电重建**不知道** LBA——正是要从页内 META
 * 读回来，故它们把 meta 置成本哨兵，FC 跳过「读错页」那一项校验（META 自洽性仍校验）。
 * 不用 0 当哨兵：0 是合法的扇区 LBA（页 0 / plane 0 / CW 0）。 */
#define FC_META_UNKNOWN (0xFFFFFFFFU)

struct fc_command_t
{
    DWORD desc_id       : 16; /* First Descriptor ID; 0xFFFF means no Descriptor. */
    DWORD high_priority : 1;  /* 0=SQ, 1=HSQ. */
    DWORD op_tag        : 7;  /* 不透明 7 位完成标签（= FTL 的 ftl_op_id_e，见 ftl_op.h）。
                               * FC **不解释、不分支**，只在完成时原样拷进
                               * fc_command_status_t.op_tag（fc.c 的 fc_complete_from_nand）。
                               * 上层据它查"完成服务函数数组"派发 ⇒ ID 真的在命令上。
                               * 本字段不落介质（落介质的只有 meta/paa）⇒ 编号可自由重排。 */
    DWORD feature_addr : 8;   /* SET/GET_FEATURES register address, sent via COLUMN_0. */
    DWORD meta;               /* META 值（本命令的**扇区 LBA**），写入各 CW buffer[0..3]。
                               * 每条 desc 取其高位 + 自己的 plane/cw 得本 CW 的 meta，
                               * 见 fc_command_configure_descriptor 的 plane_cw 公式。 */
    DWORD paa;                /* 本命令的**芯片内地址（PAA）**，由 FTL 在边界处
                               * `pba_to_paa()` 转好后交来。
                               * 数据命令的逐 CW 行地址仍走 desc 链（Policy B：两 plane 可不同块）；
                               * 本字段供单 plane / 擦除 / Feature / READ_META 等无逐 CW 地址的
                               * 命令取行地址，以及 fc_command_plane_span 定 plane 段。
                               * ⚠ 路由（ch/ce/lun）**不在 PAA 里**——一律走 config。 */
    fc_cmd_conf_t config;     /* Operation, NAND location, CW and data-path configuration. */
    fc_param_t    param;      /* 仅 FEATURES 命令使用：SET_FEATURES 的内联数据 / GET_FEATURES 的读回值
                               * （param.bytes）。「本命令访问哪些 CW」**不在命令里**——
                               * 它由 FTL 在建链时作形参交给 ftl_cmd_configure_descriptors()。 */
};

/* CQ 完成项：命令级聚合状态 + 命令级返回段。
 * Per-CW 细节（status/ecc_bits）只留在 descriptor 链中，聚合 ECC 用 fc_get_cmd_ecc_max()；
 * 聚合 status 用 FC_CW_STATUS_* 位（READY=0x40、NAND_FAIL=0x01、TIMEOUT=0x02…）。
 *
 * desc_id 的取值（fc_complete_from_nand 组装）：
 *   初值 = NAND 层透传的 failed_descriptor_id（无失败时即 FC_DESCRIPTOR_ID_INVALID）。
 *   ⚠ 该透传值实为 **flat CW 编号 n**（见 nand_dispatch_tables.c 的 completion_per_cw），
 *     与 desc id 不同空间——只对"哪个 CW 失败"这一含义有效，勿当 desc 下标用。
 *   Page Read 且带 desc 链时，再按链序覆盖为**首个失败 CW 的 descriptor id**，
 *   失败判据是 FC_CW_STATUS_ERROR_MASK 全集（不只 UNC）；全链无失败位则保持初值。
 * ⚠ 目前 FTL 侧尚无读者；要定位失败 CW，由上层遍历 desc 链 OR 各 desc->status；
 *   FC 不代为聚合失败位图。 */
struct fc_command_status_t
{
    DWORD command_id : 16;    /* Completed Command Pool ID. */
    DWORD desc_id    : 16;    /* 首个失败 CW 的 descriptor id；0xFFFF = 无失败。详见上方说明。 */
    DWORD op_tag     : 7;     /* Copy of command->op_tag —— 上层**不必反查命令**即可派发
                               * （g_op_callbacks[completion.op_tag]）。 */
    DWORD      status   : 9;  /* Aggregated command-level status; FC_CW_STATUS_* bits ORed from all CWs. */
    DWORD      reserved : 16; /* Reserved. */
    fc_param_t param;         /* 命令级返回：仅 GET_FEATURES = 寄存器值 param.bytes；其余命令恒 0
                               * （失败 CW 的定位走 desc 链，不在此聚合）。 */
};

/**
 * @brief 给出命令能触及的 plane 段 [*plane_first, *plane_last)。
 *
 * @param command 命令；plane_first/plane_last —— 输出（调用者保证非空）。
 *
 * @return 段内 plane 数（= multi_plane，单 plane 时为 1）。
 *
 * @note 实现要点：- multi-plane：从 plane 0 起，覆盖 plane_count 个 plane；
 * - 单 plane：**只覆盖 paa_to_plane(command->paa) 那一个**——掩码是绝对编号，
 * plane 1 的单 plane 命令落在 0xF0 段而不是 0x0F 段。
 *
 * 放在头文件（唯一一份定义）：这是 fc_command_t 的结构属性——FC 侧用它做
 * 建链入口（fc_interface.c 的 fc_command_configure_descriptor）的越段校验，
 * FTL 侧用它做位图展开。两份实现会让改 plane 语义时漏改。
 * ⚠ 上界写死 PLANE_NUM：desc PAA 的 plane 位与请求的 plane_paa[PLANE_NUM]
 * 都按 PLANE_NUM 定尺。改 plane 语义时这里与 nand_dispatch_tables.c 的
 * multi-plane handler 要一起动。
 */
static inline BYTE fc_command_plane_span(const fc_command_t *command, BYTE *plane_first, BYTE *plane_last)
{
    const BYTE plane_count = (command->config.multi_plane > 0U) ? command->config.multi_plane : 1U;

    ASSERT(plane_count <= PLANE_NUM);

    *plane_first = (plane_count == 1U) ? paa_to_plane(command->paa) : 0U;
    *plane_last  = (*plane_first + plane_count);
    return plane_count;
}

/**
 * @brief 本命令 plane 段内**全部** CW 的 flat 位图——「整段访问」的显式写法。
 *
 * @param command 命令（须已设 config.multi_plane 与 paa，本函数按它们定段）。
 *
 * @return 段内每个 CW 一位置 1（plane 1 单 plane ⇒ 0xF0，两 plane ⇒ 0xFF）。用途：产出 **ftl_cmd_configure_descriptors(cmd, cw_bitmap) 的实参**——FTL 的整段路径（pba_to_cmd / _pair_cw）一律先调本函数算出位图，再作为形参交给建链入口。⚠ 必须在 paa 与 multi_plane **都写好之后**调用——它读的就是这两个字段。
 */
static inline DWORD fc_command_plane_span_cw_bitmap(const fc_command_t *command)
{
    BYTE plane_first;
    BYTE plane_last;

    (void)fc_command_plane_span(command, &plane_first, &plane_last);
    return ((1U << ((plane_last - plane_first) * CW_NUM)) - 1U) << (plane_first * CW_NUM);
}

/**
 * @brief 校验命令配置合法性。
 *
 * @param command FC 命令。
 *
 * @return 恒 FC_STATUS_OK。实现要点（整函数转 ASSERT）：- 三处前置违反——command 为空 / operation ≥ OP_COUNT / 数据操作无 desc 链——全是**调用者 bug**：`operation` 全仓都是 FTL 的硬编码常量（见 ftl_fc_interface.c 的 sync_page_prepare 实参），**从不是 host 输入**，属判据 2，故一律 ASSERT（配置错误绝不发到硬件，§十三 定调）。- 本函数退化为「恒 OK」的校验钩子：debug 下跑内层断言；release 下调用点 fc_submit 的 ASSERT(... == OK) 整条裁空，无运行时开销。- has_data 覆盖数据输出型操作（READ_STATUS/READ_ID 等），它们也需要 buffer；data_input_cycles>0 只是其子集。- **不校验 CW 集合**（集合不在命令里）：它是 FTL 的建链输入， FC 只认 desc 链（链长度即真值）。越段检查随枚举逻辑一并上移 FTL，见 ftl_cmd_configure_descriptors()。本函数只确认「有数据的命令必须已有链」。
 */
fc_status_e fc_command_validate(const fc_command_t *command);
/**
 * @brief 判断 flat CW 掩码在**每个 plane 上的 CW 集合是否相同**。
 *
 * @param mask flat 掩码（bit n = plane*CW_NUM + cw）。
 *
 * @return 1 = 对称（mask == 0 视为对称）；0 = 不对称。用途：RAID / stripe 的 XOR 按 CW 配对，要求两 plane 同集合（见 fc_data_path_generate_stripe_parity 的前置断言）。
 */
BYTE fc_cw_bitmap_is_plane_symmetric(DWORD mask);

#endif /* FC_COMMAND_H */
