#ifndef NAND_ONFI_1_0_H
#define NAND_ONFI_1_0_H

#include "nand_protocol.h"

#include <stdint.h>

/*
 * onfi_1_0.h — ONFI 1.0 命令集定义 + 操作表 + 辅助宏
 *
 * 本头文件定义：
 *   - NAND 命令字节宏（NAND_CMD_* / NAND_STATUS_*）
 *   - OP_* 操作码（g_nand_operation_map 的下标）
 *   - nand_operation_map_t 结构体（命令属性 + UOP 序列 + 时序）
 *   - ONFI 参数页结构 / 解码声明
 *   - Phase V 辅助宏（ISSUE/WAIT/FETCH 拆分判定）
 *
 * 与 fc_nand_engine.h 的关系：OP_* 对应一条 UOP 序列（定义于 fc_onfi_1_0.c），
 * 由 FC 的 UOP 引擎解释执行；本头保留为 NAND 协议定义，供 FC/器件共同使用。
 */

/* ONFI 1.0 command cycles used by the initial command set. */
#define NAND_CMD_READ_PAGE_SETUP      0x00U
#define NAND_CMD_READ_PAGE_CONFIRM    0x30U
#define NAND_CMD_PROGRAM_PAGE_SETUP   0x80U
#define NAND_CMD_PROGRAM_PAGE_CONFIRM 0x10U
#define NAND_CMD_BLOCK_ERASE_SETUP    0x60U
#define NAND_CMD_BLOCK_ERASE_CONFIRM  0xD0U
#define NAND_CMD_READ_STATUS          0x70U
/* 前缀 ONFI_ 区分 HAL 的 NAND_CMD_RESET（两者数值相同、命名空间不同）。 */
#define NAND_ONFI_CMD_RESET                   0xFFU
#define NAND_CMD_READ_ID                      0x90U
#define NAND_CMD_READ_PARAMETER_PAGE          0xECU
#define NAND_CMD_READ_UNIQUE_ID               0xEDU
#define NAND_CMD_GET_FEATURES                 0xEEU
#define NAND_CMD_SET_FEATURES                 0xEFU
#define NAND_CMD_READ_STATUS_ENHANCED         0x78U
#define NAND_CMD_RANDOM_DATA_READ             0x05U
#define NAND_CMD_RANDOM_DATA_READ_MULTI_PLANE 0x06U
#define NAND_CMD_RANDOM_DATA_READ_END         0xE0U
#define NAND_CMD_RANDOM_DATA_INPUT            0x85U
#define NAND_CMD_READ_PAGE_CACHE_SEQ          0x31U
#define NAND_CMD_READ_PAGE_CACHE_LAST         0x3FU
#define NAND_CMD_PROGRAM_PAGE_CACHE           0x15U
#define NAND_CMD_INTERNAL_DATA_MOVE           0x35U
#define NAND_CMD_MULTI_PLANE_PROGRAM_SWITCH   0x11U /* Multi-Plane 编程切换 plane */
#define NAND_CMD_BLOCK_UNLOCK_LOW             0x23U
#define NAND_CMD_BLOCK_UNLOCK_HIGH            0x24U
#define NAND_CMD_BLOCK_LOCK                   0x2AU
#define NAND_CMD_BLOCK_LOCK_TIGHT             0x2CU
#define NAND_CMD_BLOCK_LOCK_STATUS            0x7AU

/* 90h 的地址段取值（手册 Figure 26 / Figure 27）：
 *   00h → 5 字节厂商/器件 ID（OP_READ_ID）
 *   20h → 4 字节 ONFI 签名 "ONFI"（OP_READ_ONFI_SIGNATURE） */
#define NAND_READ_ID_ADDR_ONFI_SIGNATURE 0x20U

/* 70h 状态字节位定义：bit6=READY、bit0=FAIL */
#define NAND_STATUS_READY 0x40U
#define NAND_STATUS_FAIL  0x01U
/* bit5 = ARDY：0 = 内部操作未完成（编程中），1 = 全部内部操作完成（cache mode）。
 * bit7 = WP：0 = protected，1 = not protected。 */
#define NAND_STATUS_ARDY 0x20U
#define NAND_STATUS_WP   0x80U
/* bit1 = FAILC：**上一页** cache program 的成败。15h 后当前页 FAIL 尚未反映阵列结果，
 * 必须同时看 FAILC；只查 FAIL 会把中间页失败静默当成成功。 */
#define NAND_STATUS_FAILC 0x02U

/* ═══ 1. 基础操作（RESET / 读 ID / 参数页 / 特性） ═══ */

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

/* OP_READ_ONFI_SIGNATURE
 * ----------------------------------------------------------------------------------------------
 * < 90 >-< 20 > ~~~~ < Dout1 >-< Dout2 >-< Dout3 >-< Dout4 >
 * ----------------------------------------------------------------------------------------------
 * 手册 Figure 27：同一条 90h，地址段给 **20h**（不是 Figure 26 的 00h）⇒ 只回 4 字节
 * `4Fh 4Eh 46h 49h` = "ONFI"，即 ONFI 签名，而非 5 字节厂商/器件 ID。
 *
 * ⚠ 与 OP_READ_PARAMETER_PAGE（ECh）的 "ONFI" **不是同一条路径**：EC 是独立命令、走 desc
 *   链、字节落 USER 缓冲；本 op 是 90h 命令、**无 desc 链**、4 字节经控制器侧暂存
 *   `g_fc_return_staging` 进 CQ 的 `param`（同 OP_GET_FEATURES / OP_READ_META）。
 *
 * 槽位 9 当前用于本操作；槽位 20 是保留空槽，不复用。
 */
#define OP_READ_ONFI_SIGNATURE (9U)

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

/* OP_RANDOM_DATA_READ_MULTI_PLANE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 06 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< E0 > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 * 06h = Random Data Read Two Plane：在已完成多 plane 读（数据已驻留各 plane 页缓冲）之后，
 * 按行地址的 plane 位选择目标 plane 的页缓冲，再从列地址取单个 CW。
 * 是 05h 的多 plane 版本，区别是**带 3 个行地址周期**（plane 位在行地址里，由 paa_to_nand_row() 编码），
 * 05h 只有列地址周期。与 OP_RANDOM_DATA_READ 配对使用：多 plane 读的逐 CW 取数走本 op。
 *
 * 前置约束：
 *   ① 06h 必须紧跟 TWO-PLANE PAGE READ（00h-00h-30h）之后，即本项目的
 *      OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT。
 *   ② two-plane 命令族（本 op / OP_PROGRAM_PAGE_MULTI_PLANE / OP_READ_PAGE_MULTI_PLANE_*）
 *      要求器件内部 on-die ECC 关闭（Micron datasheet Note 4）。本项目从不写 ECC feature
 *      寄存器（0x90），内部 ECC 保持默认关闭，故可用；若日后启用 on-die ECC，多 plane 路径
 *      整体失效。Note 4 出处待逐字核对，见 fc_nand_design.md §25.8 风险 2。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_RANDOM_DATA_READ_MULTI_PLANE (41U)

/* ═══ 2. 内部数据搬移（copy-back） ═══ */

/* 操作码 9 已复用为 OP_READ_ONFI_SIGNATURE（定义见 §1，与 OP_READ_ID 同族）——
 * 本操作码用于 READ_ONFI_SIGNATURE；随机数据输入由
 * OP_PROGRAM_FOR_INTERNAL_DATA_MOVE 覆盖。 */

/* 操作码 11 复用为 OP_READ_META（见 §3 页读 / Cache 读）。 */

/* OP_PROGRAM_FOR_INTERNAL_DATA_MOVE
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 85 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 10 >~~~~~< 70 >-< Status >
 *
 * ONFI 内部搬移编程（copy-back 目标页写入）：源页已由
 * OP_READ_FOR_INTERNAL_DATA_MOVE 读入 NAND 内部缓存，本操作把缓存内容直接编程到
 * 目标页，数据全程不过 host 总线。85h 提供目标列/行地址，10h 触发 tPROG。
 *
 * 无 DATA_IN 段：data_length = 0，has_data = 1（仍需 desc chain 提供 PAA 映射）。
 * 若要在搬移中改写页内数据，走普通读 + 普通写路径（GC 已如此），不在此 op 上加 DATA_IN。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_PROGRAM_FOR_INTERNAL_DATA_MOVE (10U)

/* ═══ 3. 页读 / Cache 读 ═══ */

/* READ OPERATIONS */

/* OP_READ_META（复用槽位 11）
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 >~~~~~< Dout1..Dout4 >-< 70 >-< Status >
 * 只读回读**指定页、指定 CW 的前 4 字节 META**（CW 布局 [META:4][CRC:4][USER:512][PARITY:8]，
 * META 在加扰区之前、明文）。用于扫描坏块建表（读页失败/工厂坏块标记）。
 *
 * 列地址 = CW 号 × CW_SIZE：CW 由 command->paa 的 cw 字段选（PAA 原生含 cw →
 * plane_paa[] → paa_to_cw()），行地址取自同一 PAA。**无 desc 链、无 buffer 占用**
 * （has_data = 0）；4 字节落控制器侧暂存，完成时拷进 CQ 的 fc_param_t param。
 * uop 序列与 OP_NORMAL_READ_PAGE_WITH_DATA_OUT 逐字节相同（复用其数组），
 * 差别仅在输出长度 = 4（request->data_length）。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_META (11U)

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
 * 「随机跳页」入口（ONFI：Read Page Cache Random）。
 * ⚠ 语义：**地址页载入 data register（预取），当场输出的是 cache 寄存器里的「上一页」**
 *   —— 输出页 ≠ 地址页，地址页要等**下一次 31h** 才出；且必须在 00h-30h 或 00h-31h
 *   之后下发（die 地址须与前一命令一致）⇒ 它是「带显式下一页地址的 31h」，
 *   **不能当独立入口单发**。列地址被器件忽略（uop 送 0）。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_RANDOM (14U)

/* OP_READ_PAGE_CACHE_LAST
 * ----------------------------------------------------------------------------------------------
 * < 3F > ~~~~ < Dout1 >-< Doutn >
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_LAST (15U)

/* OP_READ_PAGE_CACHE_START_SEQ
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 >~~~~~< 31 >~~~~~< Dout1 >-< Doutn >-< 70 >-< Status >
 * Cache 读管线启动 + 第一页输出：00h-addr-30h 把地址页读入 cache register，
 * 随后 31h 输出该页并把下一页读入 data register。
 * 单 plane 语义：输出页 == 地址页；后续页用 31h，末页用 3Fh。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_PAGE_CACHE_START_SEQ (34U)

/* ── 器件扩展：two-plane cache（ONFI 1.0 无 multi-plane；MT29F4G08 datasheet 有）── */

/* OP_TWO_PLANE_PROGRAM_CACHE（Figure 74 中间页）
 * < 80 >-< addr/data(P0) >-< 11 >-< 80 >-< addr/data(P1) >-< 15 >
 * 中间页只等 tCBSY；末页必须用 OP_TWO_PLANE_PROGRAM_CACHE_END 的 10h 收尾
 * （普通 op31 只查 FAIL，会漏掉上一页 FAILC）。 */
#define OP_TWO_PLANE_PROGRAM_CACHE (44U)

/* OP_TWO_PLANE_PROGRAM_CACHE_END（Figure 74 末页）
 * 先看上一页 FAILC，再 80h-11h-80h-10h 收尾；状态检查同时覆盖 FAIL 与 FAILC。
 * 普通 op31 不能替代：它只查 FAIL，会把上一页 cache program 的失败漏掉。 */
#define OP_TWO_PLANE_PROGRAM_CACHE_END (48U)

/* OP_TWO_PLANE_PROGRAM_CACHE_FIRST（批次首页）
 * 与 op44 相同，但只查 FAIL：首页之前没有本批的 cache 页，FAILC 可能残留上一批/别的
 * cache 操作结果，查它会把首页误判失败。从第二页起才用 op44/op48 查 FAILC。 */
#define OP_TWO_PLANE_PROGRAM_CACHE_FIRST (49U)

/* OP_READ_PAGE_CACHE_START_SEQ_MP（Two-Plane Read Cache 首 pair）
 * 00h-addr(P0) + 00h-addr(P1) + 30h → 31h → 06h P0 E0h → Dout P0
 *                                      → 06h P1 E0h → Dout P1
 * 输出 page0 pair，data register 预取 page1 pair。 */
#define OP_READ_PAGE_CACHE_START_SEQ_MP (45U)

/* OP_READ_PAGE_CACHE_SEQ_MP（Two-Plane Read Cache 顺序 pair）
 * 31h → 06h P0 E0h → Dout P0 → 06h P1 E0h → Dout P1 */
#define OP_READ_PAGE_CACHE_SEQ_MP (46U)

/* OP_READ_PAGE_CACHE_LAST_MP（Two-Plane Read Cache 末 pair）
 * 3Fh → 06h P0 E0h → Dout P0 → 06h P1 E0h → Dout P1 */
#define OP_READ_PAGE_CACHE_LAST_MP (47U)

/* ═══ 4. 页写 / Cache 写 / 擦除 / 锁 ═══ */

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

/* 操作码 20 保留为空槽，不复用；内部数据搬移统一用
 * OP_PROGRAM_FOR_INTERNAL_DATA_MOVE。 */

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
 * ⚠ 状态字节**不走数据缓冲**：引擎直读 DQ 总线送进 completion->nand_status，FC 侧再拷进
 * CQ 的 fc_param_t param.bytes[0]。故 op 表 has_data = 0 —— 它不搬 host 数据，
 * 无需 desc 链。位定义：I/O2 = Lock#（0 = 锁定，1 = 未锁）、I/O1 = LT#、I/O0 = LT（1 = 紧锁）。
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

/* ═══ 5. 多 Plane 操作 ═══ */

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
 * 数据留在 NAND 内部，后续通过 RANDOM_DATA_READ_MULTI_PLANE (06h-E0h，行地址带 plane 位) 逐 CW 读取。
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

/* ═══ 6. Phase V: ISSUE/WAIT/FETCH 拆分 ═══ */

/* ── Phase V: MULTI-LUN ISSUE/WAIT/FETCH 拆分 OP 码 ── */

/* OP_READ_PAGE_LOAD:
 * 70h轮询→00h→C1C2R1R2R3→30h→tWB→END
 * 不含 WAIT_READY(tR)，发完立即返回，LUN 进入 tR 期。
 * 由 OP_READ_PAGE_WAIT 完成等待 + 状态检查；数据留在页缓冲，
 * 随后由 fc_nand_dispatch 的 CW 链逐 CW 用 05h/06h 取回。 */
#define OP_READ_PAGE_LOAD (35U)

/* OP_READ_PAGE_WAIT:
 * WAIT_READY(tR)→70h轮询→CHECK_FAIL→END
 * 无数据传输。与 OP_READ_PAGE_LOAD / OP_READ_PAGE_MULTI_PLANE_LOAD 配对：
 * 完成即代表页已加载，逐 CW 取数由 fc_nand_dispatch_task_cw_chain 接续。 */
#define OP_READ_PAGE_WAIT (36U)

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

/* OP_READ_PAGE_MULTI_PLANE_LOAD（槽位 43）：
 * 70h轮询→(00h+C1C2R1R2R3)[plane0]→(00h+C1C2R1R2R3)[plane1]→30h→tWB→END
 * 与 OP_READ_PAGE_LOAD 同构，只是发两组 plane 地址；由 OP_READ_PAGE_WAIT 收尾。
 * 数据留在页缓冲，逐 CW 取数仍用 06h（RANDOM_DATA_READ_MULTI_PLANE）。 */
#define OP_READ_PAGE_MULTI_PLANE_LOAD (43U)

/* OP_READ_COLUMN（槽位 42 —— 新增；0..41 已用，槽位 20 是墓碑不得复用）
 * ----------------------------------------------------------------------------------------------
 * < 70 >-< Status >-< 00 >-< C1 >-< C2 >-< R1 >-< R2 >-< R3 >-< 30 >~~~~~< Dout1..Dout4 >-< 70 >-< Status >
 * 按**显式列地址**读一页的 4 个字节。用途：读 spare 区——出厂坏块标记在**列 2048**
 * （spare 首字节，x8 为 00h），任何 cw 值都够不到那里。
 *
 * 与 OP_READ_META 的唯一区别：**列地址不走 paa 的 cw 字段，改由命令级 param 的
 * 字节 0/1（小端）传入**（cw 只有 2 位 ⇒ 列上限 1584，够不到 spare 区）。
 * 行地址仍取自 command->paa。**无 desc 链、无 buffer 占用**（has_data = 0）；
 * 4 字节落控制器侧暂存，完成时拷进 CQ 的 fc_param_t param。
 * uop 序列与 OP_NORMAL_READ_PAGE_WITH_DATA_OUT 逐字节相同（复用其数组），
 * 差别仅在列地址来源与输出长度 = 4。
 * ----------------------------------------------------------------------------------------------
 */
#define OP_READ_COLUMN (42U)

#define OP_COUNT (50U) /* 0..49 inclusive */

/* ── Phase V: OP 类型枚举 ── */
#define OP_TYPE_ATOMIC (0U) /* 原子 OP 码 */
#define OP_TYPE_ISSUE  (1U) /* ISSUE 段——发完立即返回 */
#define OP_TYPE_WAIT   (2U) /* WAIT 段——等完成，无数据 */
#define OP_TYPE_FETCH  (3U) /* FETCH 段——等完成 + 读数据 */

/* ═══ 7. 操作表结构 + Phase V 辅助宏 ═══ */

typedef struct
{
    BYTE                    operation;                     /* OP_* 码（== 数组下标） */
    const char             *name;                          /* 可读名（日志用） */
    BYTE                    command_1;                     /* 命令值 1（如读页 00h / 编程 80h；无则 0） */
    BYTE                    command_2;                     /* 命令值 2（如读页 30h / 编程 10h；无则 0） */
    BYTE                    address_cycles;                /* 地址周期数（列+行；多 plane 为两倍） */
    BYTE                    data_input_cycles;             /* DATA_IN 段数（写类操作） */
    BYTE                    valid_while_selected_lun_busy; /* 本 LUN busy 时可否下发 */
    BYTE                    valid_while_other_lun_busy;    /* 其他 LUN busy 时可否下发（多 LUN 准入） */
    BYTE                    has_data;                      /* 是否涉及用户数据传输（需 descriptor+buffer） */
    WORD                    data_length;                   /* 数据传输字节数 */
    BYTE                    op_type : 2;                   /* OP_TYPE_ATOMIC / ISSUE / WAIT / FETCH */
    BYTE                    pair_op;                       /* 配对 OP 码：ISSUE→WAIT|FETCH，反之亦然 */
    WORD                    min_busy_us;                   /* LUN 内部操作最小等待时间（tR/tPROG/tBERS typ 带余量） */
    const nand_uop_t       *uops;                          /* UOP 序列（onfi_1_0.c 定义） */
    nand_operation_timing_t timing;                        /* 操作时序参数（twb/trst/twh/busy_timeout） */
} nand_operation_map_t;

/* OP_* 可直接作为下标访问，不需要命令表扫描或操作码解析。 */
extern const nand_operation_map_t g_nand_operation_map[OP_COUNT];

/* 判断 operation 是否涉及用户数据传输（即是否需要 descriptor + buffer）。
 * NO_DATA 类操作（RESET、LOCK 等）和 ERASE 返回 0。
 * 推荐直接读 g_nand_operation_map[op].has_data。 */
#define nand_operation_has_data(op) ((op) < OP_COUNT ? g_nand_operation_map[(op)].has_data : 0U)

/* 返回 operation 的数据传输字节数。NO_DATA 类操作返回 0。
 * 推荐直接读 g_nand_operation_map[op].data_length。 */
#define nand_operation_data_length(op) ((op) < OP_COUNT ? g_nand_operation_map[(op)].data_length : 0U)

/* ── Phase V 多 LUN 辅助宏 ──
 * op_type/pair_op 判定（见 nand_operation_map_t）：
 *   is_issue        ：该 OP 是否 ISSUE 段（发完即返回，LUN 进入 busy）
 *   is_wait_or_fetch：该 OP 是否 WAIT/FETCH 段（等完成）
 *   is_splittable   ：原子 OP 且存在配对 OP（可拆分为 ISSUE+WAIT/FETCH）
 *   to_issue / get_pair：取配对 OP 码（原子 OP 取自身） */
#define nand_operation_is_issue(op)         ((op) < OP_COUNT ? (g_nand_operation_map[(op)].op_type == OP_TYPE_ISSUE) : 0U)
#define nand_operation_is_wait_or_fetch(op) ((op) < OP_COUNT ? ((g_nand_operation_map[(op)].op_type == OP_TYPE_WAIT) || (g_nand_operation_map[(op)].op_type == OP_TYPE_FETCH)) : 0U)
#define nand_operation_is_splittable(op)    ((op) < OP_COUNT ? ((g_nand_operation_map[(op)].op_type == OP_TYPE_ATOMIC) && (g_nand_operation_map[(op)].pair_op != 0U)) : 0U)
#define nand_operation_to_issue(op)         ((op) < OP_COUNT ? g_nand_operation_map[(op)].pair_op : (op))
#define nand_operation_get_pair(op)         ((op) < OP_COUNT ? g_nand_operation_map[(op)].pair_op : (op))

/* ═══ 8. ONFI 参数页（Parameter Page，手册 Table 11） ═══
 *
 * 256 字节结构，由 ECh（OP_READ_PARAMETER_PAGE）读出；多字节字段一律**小端**。
 * 手册 Table 11 的 bytes 256–511 / 512–767 是同一份内容的副本（续页），本层只解第一份。
 *
 * 只解「校验 / 开机打印」用得上的字段；保留段与 vendor-specific 段（141–253）不解。
 * 偏移集中在此，避免解码函数里散落魔数。
 */
#define NAND_ONFI_PARAM_PAGE_SIZE (256U)
#define NAND_ONFI_PARAM_MODEL_LEN (20U) /* bytes 44–63 的型号串长度（不含结尾 NUL） */

/* 字段偏移（手册 Table 11；注释里的区间为闭区间）。 */
#define NAND_ONFI_PARAM_OFF_SIGNATURE               0U   /* 0–3    "ONFI" = 4F 4E 46 49 */
#define NAND_ONFI_PARAM_OFF_REVISION                4U   /* 4–5    小端 */
#define NAND_ONFI_PARAM_OFF_FEATURES                6U   /* 6–7    小端 */
#define NAND_ONFI_PARAM_OFF_MODEL                   44U  /* 44–63  型号串（ASCII） */
#define NAND_ONFI_PARAM_OFF_MANUFACTURER_ID         64U  /* 64     厂商 ID（Micron = 2Ch） */
#define NAND_ONFI_PARAM_OFF_DATA_BYTES_PER_PAGE     80U  /* 80–83  小端 */
#define NAND_ONFI_PARAM_OFF_SPARE_BYTES_PER_PAGE    84U  /* 84–85  小端 */
#define NAND_ONFI_PARAM_OFF_DATA_BYTES_PER_PARTIAL  86U  /* 86–89  小端 */
#define NAND_ONFI_PARAM_OFF_SPARE_BYTES_PER_PARTIAL 90U  /* 90–91  小端 */
#define NAND_ONFI_PARAM_OFF_PAGES_PER_BLOCK         92U  /* 92–95  小端 */
#define NAND_ONFI_PARAM_OFF_BLOCKS_PER_UNIT         96U  /* 96–99  小端 */
#define NAND_ONFI_PARAM_OFF_LOGICAL_UNITS           100U /* 100    小端 */
/* 101：bits 7:4 = 列地址周期数，bits 3:0 = 行地址周期数（ONFI Table 11；
 * 原文未随仓备份，见 docs/reference/README.md §E1）。0x23 = C2/R3。
 * nand_init 用它与本仓 UOP 表实际发出的 C/R 周期数比对。 */
#define NAND_ONFI_PARAM_OFF_ADDRESS_CYCLES       101U /* 101    [C:4][R:4] cycle counts */
#define NAND_ONFI_PARAM_OFF_BITS_PER_CELL        102U /* 102    小端 */
#define NAND_ONFI_PARAM_OFF_PROGRAMS_PER_PAGE    110U /* 110    小端 */
#define NAND_ONFI_PARAM_OFF_ECC_BITS             112U /* 112    小端 */
#define NAND_ONFI_PARAM_OFF_TIMING_MODE_SUPPORT  129U /* 129–130 位图：bit n = 支持 Mode n */
#define NAND_ONFI_PARAM_OFF_PROGRAM_CACHE_TIMING 131U /* 131–132 小端 */
#define NAND_ONFI_PARAM_OFF_TPROG_MAX            133U /* 133–134 小端 */
#define NAND_ONFI_PARAM_OFF_TBERS_MAX            135U /* 135–136 小端 */
#define NAND_ONFI_PARAM_OFF_TR_MAX               137U /* 137–138 小端 */
#define NAND_ONFI_PARAM_OFF_TCCS_MIN             139U /* 139–140 小端 */

/* 小端读取（参数页多字节字段一律小端；全仓无现成 LE helper）。 */
static inline WORD nand_onfi_le16(const BYTE *p)
{
    return (p[0] | (p[1] << 8U));
}

static inline DWORD nand_onfi_le32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8U) | ((DWORD)p[2] << 16U) | ((DWORD)p[3] << 24U);
}

/* ONFI 参数页解码结果（只含校验 / 打印用得到的字段）。 */
typedef struct
{
    BYTE  signature[4];                          /* "ONFI" = 4F 4E 46 49 */
    WORD  revision;                              /* 版本号（小端原值） */
    WORD  features;                              /* 支持的特性位 */
    char  model[NAND_ONFI_PARAM_MODEL_LEN + 1U]; /* 型号串（去尾部空格 + 补 NUL） */
    BYTE  manufacturer_id;                       /* 厂商 ID（Micron = 2Ch） */
    DWORD data_bytes_per_page;
    WORD  spare_bytes_per_page;
    DWORD data_bytes_per_partial_page;
    WORD  spare_bytes_per_partial_page;
    DWORD pages_per_block;
    DWORD blocks_per_unit;
    BYTE  logical_units;
    BYTE  address_cycles; /* 字节原值（Table 11 的 23h） */
    BYTE  column_cycles;  /* bits 7:4：列地址周期数 */
    BYTE  row_cycles;     /* bits 3:0：行地址周期数 */
    BYTE  bits_per_cell;
    BYTE  programs_per_page;
    BYTE  ecc_bits;
    WORD  timing_mode_support; /* 位图：bit n = 支持 Mode n */
    WORD  program_cache_timing;
    WORD  tprog_max; /* 以下四项：量纲见手册 Table 11，仓库内未查证 ⇒ 只存原值、不打单位 */
    WORD  tbers_max;
    WORD  tr_max;
    WORD  tccs_min;
} nand_onfi_param_info_t;

/*
 * nand_onfi_parameter_page_decode
 *   功能：把 ECh 读到的 256 字节参数页解成 nand_onfi_param_info_t。
 *   参数：page —— 至少 NAND_ONFI_PARAM_PAGE_SIZE 字节的原始页；
 *         info —— 输出（**无论签名是否匹配都会填满**，供诊断打印）。
 *   返回：1 = 签名是 "ONFI"；0 = 签名不符。
 *   实现要点：纯函数——零 HAL / 零日志 / 零硬件 ⇒ 可纯 RAM 单测。
 *         model 按 20 字节拷贝并补结尾 NUL（尾部空格填充**不裁剪**）。
 */
BYTE nand_onfi_parameter_page_decode(const BYTE page[NAND_ONFI_PARAM_PAGE_SIZE], nand_onfi_param_info_t *info);

#endif /* NAND_ONFI_1_0_H */
