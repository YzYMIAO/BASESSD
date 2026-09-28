#ifndef PLATFORM_STM32H743_ONFI_1_0_H
#define PLATFORM_STM32H743_ONFI_1_0_H

#include "nand_engine.h"

#include <stdint.h>

/* ONFI 1.0 command cycles used by the initial command set. */
#define NAND_CMD_READ_PAGE_SETUP            0x00U
#define NAND_CMD_READ_PAGE_CONFIRM          0x30U
#define NAND_CMD_PROGRAM_PAGE_SETUP         0x80U
#define NAND_CMD_PROGRAM_PAGE_CONFIRM       0x10U
#define NAND_CMD_BLOCK_ERASE_SETUP          0x60U
#define NAND_CMD_BLOCK_ERASE_CONFIRM        0xD0U
#define NAND_CMD_READ_STATUS                0x70U
#define NAND_ONFI_CMD_RESET                 0xFFU
#define NAND_CMD_READ_ID                    0x90U
#define NAND_CMD_READ_PARAMETER_PAGE        0xECU
#define NAND_CMD_READ_UNIQUE_ID             0xEDU
#define NAND_CMD_GET_FEATURES               0xEEU
#define NAND_CMD_SET_FEATURES               0xEFU
#define NAND_CMD_READ_STATUS_ENHANCED       0x78U
#define NAND_CMD_RANDOM_DATA_READ           0x05U
#define NAND_CMD_RANDOM_DATA_READ_TWO_PLANE 0x06U
#define NAND_CMD_RANDOM_DATA_READ_END       0xE0U
#define NAND_CMD_RANDOM_DATA_INPUT          0x85U
#define NAND_CMD_READ_MODE                  0x00U
#define NAND_CMD_READ_PAGE_CACHE_SEQ        0x31U
#define NAND_CMD_READ_PAGE_CACHE_LAST       0x3FU
#define NAND_CMD_PROGRAM_PAGE_CACHE         0x15U
#define NAND_CMD_INTERNAL_DATA_MOVE         0x35U
#define NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH 0x11U /* Multi-Plane 编程切换 plane */
#define NAND_CMD_BLOCK_UNLOCK_LOW           0x23U
#define NAND_CMD_BLOCK_UNLOCK_HIGH          0x24U
#define NAND_CMD_BLOCK_LOCK                 0x2AU
#define NAND_CMD_BLOCK_LOCK_TIGHT           0x2CU
#define NAND_CMD_BLOCK_LOCK_STATUS          0x7AU

#define NAND_STATUS_READY 0x40U
#define NAND_STATUS_FAIL  0x01U

/* RESET OPERATIONS */
/* OP_RESET
 * ----------------------------------------------------------------------------------------------
 * < FF > ~~~~ R/B# ~~~~ < 70 >-< Status >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_RESET (0U)

/* OP_READ_ID
 * ----------------------------------------------------------------------------------------------
 * < 90 >-< A1 > ~~~~ < Dout1 >-< Dout2 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_ID (1U)

/* OP_READ_PARAMETER_PAGE
 * ----------------------------------------------------------------------------------------------
 * < EC >-< A1 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PARAMETER_PAGE (2U)

/* OP_READ_UNIQUE_ID
 * ----------------------------------------------------------------------------------------------
 * < ED >-< A1 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_UNIQUE_ID (3U)

/* FEATURE OPERATIONS */
/* OP_GET_FEATURES
 * ----------------------------------------------------------------------------------------------
 * < EE >-< A1 > ~~~~ < Dout1 >-< Dout2 >-< Dout3 >-< Dout4 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_GET_FEATURES (4U)

/* OP_SET_FEATURES
 * ----------------------------------------------------------------------------------------------
 * < EF >-< A1 >-< Din1 >-< Din2 >-< Din3 >-< Din4 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_SET_FEATURES (5U)

/* STATUS OPERATIONS */
/* OP_READ_STATUS
 * ----------------------------------------------------------------------------------------------
 * < 70 > ~~~~ < Status >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_STATUS (6U)

/* OP_READ_STATUS_ENHANCED
 * ----------------------------------------------------------------------------------------------
 * < 78 >-< L1 >-< L2 >-< L3 > ~~~~ < Status >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_STATUS_ENHANCED (7U)

/* COLUMN ADDRESS OPERATIONS */
/* OP_RANDOM_DATA_READ
 * ----------------------------------------------------------------------------------------------
 * < 05 >-< C1 >-< C2 >-< E0 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_RANDOM_DATA_READ (8U)

/* 操作码 9 已删除（原 OP_RANDOM_DATA_INPUT），
 * 功能由 OP_PROGRAM_FOR_INTERNAL_DATA_MOVE 完整覆盖。 */

/* 操作码 11 已删除（原 OP_READ_MODE），
 * 功能由 OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT 内含相同语义，无独立用途。 */

/* OP_PROGRAM_FOR_INTERNAL_DATA_MOVE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 85 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< Din(1B) >-< 10 >~~~~~< 70 >-< Status >
 *
 * ONFI 内部搬移的"替换写入"模式：85h + 地址 + 1 字节数据后发 10h 确认，
 * 将单字节写入页缓冲中指定位置，然后写入 NAND。不同于纯 copy-back
 * （OP_PROGRAM_FOR_INTERNAL_MOVE），它允许在搬移过程中替换 1 字节数据
 * （如 ECC 纠正后的单字节修正），然后 10h 触发 tPROG 写入 NAND。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_PROGRAM_FOR_INTERNAL_DATA_MOVE (10U)

/* READ OPERATIONS */

/* OP_NORMAL_READ_PAGE_WITH_DATA_OUT
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 >~~~~~< Dout1 >-< Dout2 >-< Doutn >-< 70 >-< Status >
 * 标准单 Plane 读取，30h 后 tR 等待，数据加载完成后立即输出整页 PAGE_SIZE 字节。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_NORMAL_READ_PAGE_WITH_DATA_OUT (12U)

/* OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 >~~~~~< 70 >-< Status >
 * 单 Plane 读取，30h 后 tR 将数据加载到页缓冲，不输出到总线。
 * 数据留在 NAND 内部，后续通过 RANDOM_DATA_READ (05h-E0h) 分 CW 读取。
 * 适用于配合 05h-E0h 随机读取或需要先加载再处理其他操作的场景。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT (33U)

/* OP_READ_PAGE_CACHE_SEQ
 * ----------------------------------------------------------------------------------------------
 * < 31 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_SEQ (13U)

/* OP_READ_PAGE_CACHE_RANDOM
 * ----------------------------------------------------------------------------------------------
 * < 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 31 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_RANDOM (14U)

/* OP_READ_PAGE_CACHE_LAST
 * ----------------------------------------------------------------------------------------------
 * < 3F > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_LAST (15U)

/* OP_READ_PAGE_CACHE_START
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 >~~~~~< 70 >-< Status >
 * Cache 读管线启动：00h-地址-30h 将页加载到 NAND 内部页缓冲，不输出到总线。
 * 数据由后续 READ_PAGE_CACHE_SEQ（31h）被动输出。
 * 语义上等价于 WITHOUT_DATA_OUT，但专用于 Cache 读序列的首条命令。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_START (34U)

/* PROGRAM OPERATIONS */
/* OP_NORMAL_PROGRAM_PAGE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 80 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< Din1 >-< Din2 >-< Dinn >-< 10 >~~~~~< 70 >-< Status >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_NORMAL_PROGRAM_PAGE (16U)

/* OP_PROGRAM_PAGE_CACHE
 * ----------------------------------------------------------------------------------------------
 * < 80 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< Din1 >-< Dinn >-< 15 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_PROGRAM_PAGE_CACHE (17U)

/* ERASE OPERATIONS */
/* OP_ERASE_BLOCK
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 60 >-< R1 >-< R2 >-< R3 >-< D0 >~~~~~< 70 >-< Status >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_ERASE_BLOCK (18U)

/* INTERNAL DATA MOVE OPERATIONS */
/* OP_READ_FOR_INTERNAL_DATA_MOVE
 * ----------------------------------------------------------------------------------------------
 * < 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 35 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_FOR_INTERNAL_DATA_MOVE (19U)

/* OP_PROGRAM_FOR_INTERNAL_MOVE
 * ----------------------------------------------------------------------------------------------
 * < 85 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 10 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_PROGRAM_FOR_INTERNAL_MOVE (20U)

/* BLOCK LOCK OPERATIONS */
/* OP_BLOCK_UNLOCK_LOW
 * ----------------------------------------------------------------------------------------------
 * < 23 >-< R1 >-< R2 >-< R3 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_BLOCK_UNLOCK_LOW (21U)

/* OP_BLOCK_UNLOCK_HIGH
 * ----------------------------------------------------------------------------------------------
 * < 24 >-< R1 >-< R2 >-< R3 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_BLOCK_UNLOCK_HIGH (22U)

/* OP_BLOCK_LOCK
 * ----------------------------------------------------------------------------------------------
 * < 2A >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_BLOCK_LOCK (23U)

/* OP_BLOCK_LOCK_TIGHT
 * ----------------------------------------------------------------------------------------------
 * < 2C >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_BLOCK_LOCK_TIGHT (24U)

/* OP_BLOCK_LOCK_STATUS
 * ----------------------------------------------------------------------------------------------
 * < 7A >-< R1 >-< R2 >-< R3 > ~~~~ < Status >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_BLOCK_LOCK_STATUS (25U)

/* OTP OPERATIONS */
/* OP_OTP_DATA_LOCK
 * ----------------------------------------------------------------------------------------------
 * < 80 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 10 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_OTP_DATA_LOCK (26U)

/* OP_OTP_DATA_PROGRAM
 * ----------------------------------------------------------------------------------------------
 * < 80 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< Din1 >-< Dinn >-< 10 >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_OTP_DATA_PROGRAM (27U)

/* OP_OTP_DATA_READ
 * ----------------------------------------------------------------------------------------------
 * < 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_OTP_DATA_READ (28U)

/* MULTI-PLANE READ OPERATIONS */
/* OP_READ_PAGE_MULTI_PLANE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1_P0 >-< R2_P0 >-< R3_P0 >-< 00 >-< C1 >-< C2 >-< R1_P1 >-< R2_P1 >-< R3_P1 >-< 30 >~~~~~< Dout1..n(双页) >-< 70 >-< Status >
 *       |--- Plane 0 地址周期 (5 字节, 含 plane bit = 0) ---|
 *                                                                |--- Plane 1 地址周期 (5 字节, 含 plane bit = 1) ---|
 * 两个 Plane 共用相同的列地址 (C1/C2)，仅行地址中的 plane bit 不同。
 * 30h 后 R/B# 等待 tR，一次性输出 PAGE_SIZE * PLANE_NUM 字节。
 * ----------------------------------------------------------------------------------------------
 */

/* MULTI-PLANE READ (WITHOUT DATA OUT) */
/* OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1_P0 >-< R2_P0 >-< R3_P0 >-< 00 >-< C1 >-< C2 >-< R1_P1 >-< R2_P1 >-< R3_P1 >-< 30 >~~~~~< 70 >-< Status >
 * 30h 后 tR 将两个 Plane 数据加载到各 Plane 页缓冲中，但不输出到总线。
 * 数据留在 NAND 内部，后续通过 RANDOM_DATA_READ (05h-E0h) 逐 CW 读取。
 * 适用于需要先完成其他操作再回读数据的场景。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT (29U)

/* MULTI-PLANE READ (WITH DATA OUT) */
/* OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1_P0 >-< R2_P0 >-< R3_P0 >-< 00 >-< C1 >-< C2 >-< R1_P1 >-< R2_P1 >-< R3_P1 >-< 30 >~~~~~< Dout1..n(双页) >-< 70 >-< Status >
 * 30h 后 R/B# 等待 tR，一次性输出 PAGE_SIZE * PLANE_NUM 字节。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT (30U)

/* MULTI-PLANE PROGRAM OPERATIONS */
/* OP_PROGRAM_PAGE_MULTI_PLANE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 80 >-< C1 >-< C2 >-< R1_P0 >-< R2_P0 >-< R3_P0 >-< Din_P0(2112B) >-< 11 >-< C1 >-< C2 >-< R1_P1 >-< R2_P1 >-< R3_P1 >-< Din_P1(2112B) >-< 10 >~~~~~< 70 >-< Status >
 *       |--- Plane 0 地址周期 (5 字节) -----|               |--- Plane 1 地址周期 (5 字节) -----|
 * 80h 载入 Plane 0 数据和地址 → 11h 切换 Plane → 载入 Plane 1 数据和地址 → 10h 确认烧录。
 * 11h 是 ONFI 标准的 Multi-Plane 切换命令，告诉 NAND 现在载入第二个 Plane。
 * tPROG 后两个 Plane 同时烧录。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_PROGRAM_PAGE_MULTI_PLANE (31U)

/* MULTI-PLANE ERASE OPERATIONS */
/* OP_ERASE_BLOCK_MULTI_PLANE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 60 >-< R1_P0 >-< R2_P0 >-< R3_P0 >-< 60 >-< R1_P1 >-< R2_P1 >-< R3_P1 >-< D0 >~~~~~< 70 >-< Status >
 *       |--- Plane 0 行地址 (3 字节, 无列地址) ---|
 *                                                      |--- Plane 1 行地址 (3 字节, 无列地址) ---|
 * 两个 Plane 共用同一 Block 号，仅 plane bit (BA6) 不同。
 * D0h 后 tBERS，两个 Plane 同时擦除。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_ERASE_BLOCK_MULTI_PLANE (32U)

/* ── Phase V: MULTI-LUN ISSUE/WAIT/FETCH 拆分 OP 码 ── */

/* OP_READ_PAGE_LOAD:
 * 70h轮询→00h→C1C2R1R2R3→30h→tWB→END
 * 不含 WAIT_READY(tR)，发完立即返回，LUN 进入 tR 期。
 * 必须由 OP_READ_PAGE_FETCH 完成 WAIT_READY + DATA_OUT + 状态检查。 */
#define OP_READ_PAGE_LOAD (35U)

/* OP_READ_PAGE_FETCH:
 * WAIT_READY(tR)→DATA_OUT→70h轮询→CHECK_FAIL→END
 * 与 OP_READ_PAGE_LOAD 配对。 */
#define OP_READ_PAGE_FETCH (36U)

/* OP_PROGRAM_PAGE_ISSUE:
 * 70h轮询→80h→C1C2R1R2R3→DATA_IN→10h→tWB→END
 * 不含 WAIT_READY(tPROG)，发完立即返回，LUN 进入 tPROG 期。
 * 必须由 OP_PROGRAM_PAGE_WAIT 完成等待 + 状态检查。 */
#define OP_PROGRAM_PAGE_ISSUE (37U)

/* OP_PROGRAM_PAGE_WAIT:
 * WAIT_READY(tPROG)→70h轮询→CHECK_FAIL→END
 * 无数据传输。与 OP_PROGRAM_PAGE_ISSUE 配对。 */
#define OP_PROGRAM_PAGE_WAIT (38U)

/* OP_ERASE_BLOCK_ISSUE:
 * 70h轮询→60h→R1R2R3→D0h→tWB→END
 * 不含 WAIT_READY(tBERS)，发完立即返回，LUN 进入 tBERS 期。
 * 必须由 OP_ERASE_BLOCK_WAIT 完成等待 + 状态检查。 */
#define OP_ERASE_BLOCK_ISSUE (39U)

/* OP_ERASE_BLOCK_WAIT:
 * WAIT_READY(tBERS)→70h轮询→CHECK_FAIL→END
 * 无数据传输。与 OP_ERASE_BLOCK_ISSUE 配对。 */
#define OP_ERASE_BLOCK_WAIT (40U)

#define OP_COUNT (41U) /* 0..40 inclusive */

/* ── Phase V: OP 类型枚举 ── */
#define OP_TYPE_ATOMIC (0U) /* 原原子 OP 码 */
#define OP_TYPE_ISSUE  (1U) /* ISSUE 段——发完立即返回 */
#define OP_TYPE_WAIT   (2U) /* WAIT 段——等完成，无数据 */
#define OP_TYPE_FETCH  (3U) /* FETCH 段——等完成 + 读数据 */

typedef struct
{
    uint8_t                 operation;
    const char             *name;
    uint8_t                 command_1;
    uint8_t                 command_2;
    uint8_t                 address_cycles;
    uint8_t                 data_input_cycles;
    uint8_t                 valid_while_selected_lun_busy;
    uint8_t                 valid_while_other_lun_busy;
    uint8_t                 has_data;
    uint16_t                data_length;
    uint8_t                 op_type : 2; /* OP_TYPE_ATOMIC / ISSUE / WAIT / FETCH */
    uint8_t                 pair_op;     /* 配对 OP 码：ISSUE→WAIT|FETCH，反之亦然 */
    uint16_t                min_busy_us; /* LUN 内部操作最小等待时间（tR/tPROG/tBERS typ） */
    const nand_uop_t       *uops;
    nand_operation_timing_t timing;
} nand_operation_map_t;

/* OP_* 可直接作为下标访问，不需要命令表扫描或操作码解析。 */
extern const nand_operation_map_t g_nand_operation_map[OP_COUNT];

/* 判断 operation 是否涉及用户数据传输（即是否需要 descriptor + buffer）。
 * NO_DATA 类操作（RESET、LOCK、READ_MODE 等）和 ERASE 返回 0。
 * 推荐直接读 g_nand_operation_map[op].has_data。 */
#define nand_operation_has_data(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].has_data : 0U)

/* 返回 operation 的数据传输字节数。NO_DATA 类操作返回 0。
 * 推荐直接读 g_nand_operation_map[op].data_length。 */
#define nand_operation_data_length(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].data_length : 0U)

/* ── Phase V 多 LUN 辅助宏 ── */
#define nand_operation_is_issue(op) \
    ((op) < OP_COUNT ? (g_nand_operation_map[(op)].op_type == OP_TYPE_ISSUE) : 0U)
#define nand_operation_is_wait_or_fetch(op)                                     \
    ((op) < OP_COUNT ? ((g_nand_operation_map[(op)].op_type == OP_TYPE_WAIT) || \
                        (g_nand_operation_map[(op)].op_type == OP_TYPE_FETCH))  \
                     : 0U)
#define nand_operation_is_splittable(op)                                          \
    ((op) < OP_COUNT ? ((g_nand_operation_map[(op)].op_type == OP_TYPE_ATOMIC) && \
                        (g_nand_operation_map[(op)].pair_op != 0U))               \
                     : 0U)
#define nand_operation_to_issue(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].pair_op : (op))
#define nand_operation_get_pair(op) \
    ((op) < OP_COUNT ? g_nand_operation_map[(op)].pair_op : (op))

#endif /* PLATFORM_STM32H743_ONFI_1_0_H */
