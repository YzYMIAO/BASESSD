#ifndef FC_COMMAND_H
#define FC_COMMAND_H

#include "common/assertion.h"
#include "fc_types.h"

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
 */

/* ECC 解码模式（配置写入 fc_cmd_conf_t.ldpc） */
typedef enum
{
    FC_ECC_MODE_BCH       = 0U, /* BCH encode/decode */
    FC_ECC_MODE_LDPC_HARD = 1U, /* LDPC hard-decision */
    FC_ECC_MODE_LDPC_SOFT = 2U, /* LDPC soft-decision */
    FC_ECC_MODE_LDPC_RAW  = 3U, /* LDPC raw (no decode) */
} fc_ecc_mode_t;

/* RAID 模式（配置写入 fc_cmd_conf_t.raid） */
typedef enum
{
    FC_RAID_MODE_NORMAL  = 0U, /* 无 RAID */
    FC_RAID_MODE_DECODE  = 1U, /* RAID 解码（ECC 失败时恢复） */
    FC_RAID_MODE_ENCODE  = 2U, /* RAID 编码（生成 parity） */
    FC_RAID_MODE_RECOVER = 3U, /* RAID 恢复（同 decode） */
    FC_RAID_MODE_CHECK   = 4U, /* RAID 校验（暂未实现） */
} fc_raid_mode_t;

/* NAND 页类型（TLC/QLC 多页编程时区分；SLC 只有 LP） */
typedef enum
{
    FC_PAGE_TYPE_LP = 0U, /* Lower Page (SLC page / TLC LP) */
    FC_PAGE_TYPE_MP = 1U, /* Middle Page (TLC MP) */
    FC_PAGE_TYPE_UP = 2U, /* Upper Page (TLC UP) */
    FC_PAGE_TYPE_TP = 3U, /* Triple Page (QLC TP) */
} fc_page_type_t;

/* CRC 校验开关（配置写入 fc_cmd_conf_t.crc） */
typedef enum
{
    FC_CRC_ENABLE  = 0U, /* CRC 校验使能 */
    FC_CRC_DISABLE = 1U, /* CRC 校验禁用 */
} fc_crc_mode_t;

/* 数据加扰开关（配置写入 fc_cmd_conf_t.random） */
typedef enum
{
    FC_RANDOM_ENABLE  = 0U, /* 数据加扰使能 */
    FC_RANDOM_DISABLE = 1U, /* 数据加扰禁用 */
} fc_random_mode_t;

/* 链式命令标记（配置写入 fc_cmd_conf_t.chain） */
typedef enum
{
    FC_CHAIN_END  = 0U, /* 链结束，单命令 */
    FC_CHAIN_MORE = 1U, /* 链中间，后续还有命令 */
} fc_chain_mode_t;

/* 命令优先级（选择写入 SQ 还是 HSQ） */
typedef enum
{
    FC_PRIORITY_SQ  = 0U, /* 普通优先级（Submission Queue） */
    FC_PRIORITY_HSQ = 1U, /* 高优先级（High-priority SQ） */
} fc_priority_t;

/* 完成路由归属模块：FC 只按此分类路由 completion，不主动回调 */
typedef enum
{
    FC_COMMAND_MODULE_HOST_IO       = 0U,
    FC_COMMAND_MODULE_FTL           = 1U,
    FC_COMMAND_MODULE_GC            = 2U,
    FC_COMMAND_MODULE_READ_RECOVERY = 3U,
    FC_COMMAND_MODULE_METADATA      = 4U,
    FC_COMMAND_MODULE_BBT           = 5U,
    FC_COMMAND_MODULE_CHECKPOINT    = 6U,
    FC_COMMAND_MODULE_WEAR_LEVELING = 7U,
} fc_command_module_t;

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
#define FC_CW_STATUS_UNUSED     (0U)
#define FC_CW_STATUS_SUCCESS    (FC_CW_STATUS_READY)

/* 「失败位」的规范集合——上表六个错误位的并集（READY / PROTECTED / UNUSED 不算失败）。
 * 唯一读者：fc_complete_from_nand()（fc.c）用它挑出**首个失败 CW** 的 desc_id，
 * 以兑现 fc_command_status_t.desc_id 的契约（"first-failing desc on error"）。
 * 判「CW 是否失败」时一律用它，不要就地重列位——六位散落多处时漏一位不会有编译错误。 */
#define FC_CW_STATUS_ERROR_MASK                                                                  \
    (FC_CW_STATUS_NAND_FAIL | FC_CW_STATUS_TIMEOUT | FC_CW_STATUS_CRC_ERROR | FC_CW_STATUS_UNC | \
     FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_RAID_ERROR)

/* Command 与 Descriptor 共用 32 位操作配置（位域布局见下） */
typedef struct fc_cmd_conf_t
{
    DWORD operation : 8;   /* ONFI operation macro (OP_*). */
    DWORD ch        : 2;   /* NAND channel number. */
    DWORD ce        : 2;   /* NAND chip-enable number. */
    DWORD lun       : 2;   /* NAND LUN number. */
    DWORD cw        : 2;   /* Command: 每-plane CW 数减一。**v23 起 FTL 建链不再读它**
                            *          （链长只由 param.cw_mask 决定，0 = 本命令 plane 段内
                            *          全部 CW）；当前读者只剩 FC 内 RAID stripe 的 cw_count
                            *          与测试，写者保留；
                            * Descriptor: 本 desc 在 plane 内是第几个 CW（0..CW_NUM-1）。 */
    DWORD crc         : 1; /* FC_CRC_ENABLE / FC_CRC_DISABLE. */
    DWORD ldpc        : 2; /* fc_ecc_mode_t: BCH/LDPC_HARD/LDPC_SOFT/LDPC_RAW. */
    DWORD raid        : 3; /* fc_raid_mode_t: NORMAL/DECODE/ENCODE/RECOVER/CHECK. */
    DWORD random      : 1; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE. */
    DWORD chain       : 1; /* FC_CHAIN_END / FC_CHAIN_MORE. */
    DWORD page_type   : 2; /* fc_page_type_t: LP/MP/UP/TP. */
    DWORD multi_plane : 3; /* 0=single; N=N-plane。总 desc 数以 desc 链长度为真值，不由本字段推导。 */
    DWORD reserved    : 3; /* Reserved for future configuration. */
} fc_cmd_conf_t;

/*
 * 单 CW 描述符：物理地址 + 配置 + 缓冲 ID + 链 ID + 状态 + ECC 信息。
 * 一条命令的 desc 链 = 本次要访问的 CW 逐条排列（plane-major），next_id 串联；
 * 0xFFFF 终止。**链长度即真值**：uniform 命令 = multi_plane × 每-plane CW 数，
 * flat 掩码命令 = 掩码置位数。凡需要"这条命令有几个 CW / 每 plane 几个"一律走链。
 */
struct fc_descriptor_t
{
    DWORD         paa;            /* Physical NAND address; valid for this Descriptor's CW. */
    fc_cmd_conf_t config;         /* Copy of Command configuration; config.cw is this CW index. */
    DWORD         buffer_id : 16; /* Buffer Pool ID; 0xFFFF means invalid. */
    DWORD         next_id   : 16; /* Next Descriptor ID; 0xFFFF terminates the chain. */
    DWORD         meta;           /* CW META 值：写时从 command->meta 转换填入，读后存放 NAND 读回的 META 值。 */

    /* 注意：meta 字段的内容由 lba_to_cw_meta() 从 command->meta + cw_index 转换得到；
     * command->meta 保持不变，始终是 Host LBA（当前语义）。 */
    DWORD status   : 8; /* CW status, using FC_CW_STATUS_* bits. */
    DWORD ecc_bits : 8; /* ECC corrected bit count from last completion; 0 if no error. */
};

/*
 * 4 字节命令参数 / 返回段，SQ 与 CQ 共用。按 operation 解释：
 *   parameter0..3 / bytes[4]：SET/GET_FEATURES 的内联数据（SQ）/ 读回的寄存器值（CQ）
 *   cw_mask                ：flat CW 掩码，bit n = plane * CW_NUM + cw
 *                            SQ = 请求集（0 = **本命令 plane 段内的全部 CW**）；
 *                            CQ = 失败集（UNC/CRC_ERROR，0 = 无失败）
 * 小端（Cortex-M7）下 cw_mask 的 bit0 == parameter0 的 bit0。
 * 同一时刻只按一种解释使用——读命令不用 parameter0..3，FEATURES 命令不用 cw_mask。
 *
 * ⚠ SQ 侧 cw_mask（请求集）的**读者是 FTL，不是 FC**（v13）：FC 只认 desc 链，
 *   「访问哪些 CW」由 ftl_cmd_configure_descriptors() / ftl_cmd_setup_ppa_pair()
 *   读本字段后逐条 fc_command_configure_descriptor 展开（v23 起两个入口共用同一套
 *   展开规则，见 ftl_cmd_resolve_cw_mask）。
 *   FC 内没有写入者，也没有读者——fc_command_validate 不再校验它。
 *
 * ⚠ 别与 NAND 层的 request->cw_mask 混淆：两者共用同一套 flat 编号，但**值不相等**。
 *   本字段是**意图**（输入，0 = 未指定 → 展开），FC 按链/按操作重新铸出 request->cw_mask
 *   后交给 NAND，那里的 0 是**合法空集**（无 CW 可访问），不是"全部"。
 */
typedef union
{
    struct
    {
        uint8_t parameter0; /* Command parameter byte 0. */
        uint8_t parameter1; /* Command parameter byte 1. */
        uint8_t parameter2; /* Command parameter byte 2. */
        uint8_t parameter3; /* Command parameter byte 3. */
    };
    uint8_t  bytes[4]; /* 内联数据的字节视图。 */
    uint32_t cw_mask;  /* flat CW 掩码：bit n = plane * CW_NUM + cw（绝对编号，非每-plane 低 4 位）。 */
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
struct fc_command_t
{
    DWORD         desc_id       : 16; /* First Descriptor ID; 0xFFFF means no Descriptor. */
    DWORD         high_priority : 1;  /* 0=SQ, 1=HSQ. */
    DWORD         module        : 4;  /* Completion route owner; FC does not invoke callbacks. */
    DWORD         feature_addr  : 8;  /* SET/GET_FEATURES register address, sent via COLUMN_0. */
    DWORD         reserved1     : 3;  /* Reserved for future Command control. */
    DWORD         meta;               /* META 值，写入 NAND CW buffer[0..3]；当前为 LBA，未来可扩展。 */
    DWORD         paa;                /* Operation-dependent target; data commands use Descriptor PAA values. */
    fc_cmd_conf_t config;             /* Operation, NAND location, CW and data-path configuration. */
    fc_param_t    param;              /* 读：请求掩码 param.cw_mask（v13 起读者是 FTL，FC 不解释）；
                                       * FEATURES：内联数据 param.bytes。 */
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
 * ⚠ 目前 FTL 侧尚无读者；失败 CW 更精确的定位应走 param.cw_mask（flat 掩码）。 */
struct fc_command_status_t
{
    DWORD      command_id : 16; /* Completed Command Pool ID. */
    DWORD      desc_id    : 16; /* 首个失败 CW 的 descriptor id；0xFFFF = 无失败。详见上方说明。 */
    DWORD      module     : 4;  /* Copy of command->module for direct routing without ID lookup. */
    DWORD      status     : 8;  /* Aggregated command-level status; FC_CW_STATUS_* bits ORed from all CWs. */
    DWORD      reserved   : 20; /* Reserved. */
    fc_param_t param;           /* 命令级返回：读 = 失败掩码 param.cw_mask（bit n = plane*CW_NUM + cw）；
                                 * GET_FEATURES = 寄存器值 param.bytes；其余命令恒 0。 */
};

/* --------------------------------------------------------------------
 * fc_command_plane_span
 *   功能：给出命令能触及的 plane 段 [*plane_first, *plane_last)。
 *   参数：command —— 命令；plane_first/plane_last —— 输出（调用者保证非空）。
 *   返回：段内 plane 数（= multi_plane，单 plane 时为 1）。
 *   实现要点：
 *     - multi-plane：从 plane 0 起，覆盖 plane_count 个 plane；
 *     - 单 plane：**只覆盖 paa_to_plane(command->paa) 那一个**——掩码是绝对编号，
 *       plane 1 的单 plane 命令落在 0xF0 段而不是 0x0F 段。
 *
 *   放在头文件（唯一一份定义）：这是 fc_command_t 的结构属性——FC 侧用它做
 *   建链入口（fc_interface.c 的 fc_command_configure_descriptor）的越段校验，
 *   FTL 侧用它做掩码展开。两份实现会让改 plane 语义时漏改。
 *   ⚠ 上界写死 PLANE_NUM：desc PAA 的 plane 位与请求的 plane_paa[PLANE_NUM]
 *     都按 PLANE_NUM 定尺。改 plane 语义时这里与 nand_dispatch_tables.c 的
 *     multi-plane handler 要一起动。
 * -------------------------------------------------------------------- */
static inline uint8_t fc_command_plane_span(const fc_command_t *command, uint8_t *plane_first, uint8_t *plane_last)
{
    const uint8_t plane_count = (command->config.multi_plane > 0U) ? (uint8_t)command->config.multi_plane : 1U;

    ASSERT(plane_count <= PLANE_NUM);

    *plane_first = (plane_count == 1U) ? (uint8_t)paa_to_plane(command->paa) : 0U;
    *plane_last  = (uint8_t)(*plane_first + plane_count);
    return plane_count;
}

fc_status_t fc_command_validate(const fc_command_t *command);
uint8_t     fc_cw_mask_is_plane_symmetric(uint32_t mask);

#endif /* FC_COMMAND_H */
