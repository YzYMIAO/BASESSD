#ifndef NAND_DISPATCH_H
#define NAND_DISPATCH_H

#include "fc/fc_types.h"
#include "nand_config.h"
#include "nand_fc_interface.h"

#include <stdint.h>

/* One fixed per-LUN FIFO (depth NAND_DISPATCH_CHAIN_DEPTH) is attached to each LUN. */
#define NAND_DISPATCH_CHAIN_DEPTH (4U)

/*
 * 每个 LUN 一个待处理 FIFO，深度 = NAND_DISPATCH_CHAIN_DEPTH。
 * **每槽 = 一条命令**：NAND 侧看不到「链」，一条链 = 连续 N 槽。
 * FC 用 nand_dispatch_submit_chain() 把整链原子批量追加（空间不足则整体拒收），
 * poll 遍历每 LUN FIFO，先进先出，每轮每 LUN 推进一条、完成一条即回一份 completion。
 * 因此每条命令各有一份 completion，链中间命令的状态不再被吞掉。
 */

/*
 * Self-contained request struct. FC resolves all descriptor-chain data
 * (CW mask, per-plane row PAA, buffer pointers) at submit time.
 * nand_dispatch never calls back into FC after receiving this struct.
 *
 * ── 唯一索引空间：flat 绝对 CW 号 ──
 * n = plane * CW_NUM + cw（0 .. CW_TOTAL_MAX-1）。
 * cw_mask 的 bit n、cw_buffers[n]、completion.cw_status[n] / ecc_bit_count[n]
 * 全部用这一个编号，且与 FC 侧 fc_command_t.param.cw_mask、CQ 回传的
 * param.cw_mask 同一空间。链序、plane-major 密排槽位等中间坐标已全部取消。
 * 行地址不参与该空间：它是 per-plane 的，走 plane_paa[plane]。
 */
typedef struct
{
    uint16_t command_id;               /* FC command pool slot for completion correlation. */
    uint8_t  operation;                /* OP_* constant. */
    uint8_t  ch;                       /* Channel. */
    uint8_t  ce;                       /* Chip-enable. */
    uint8_t  lun;                      /* LUN. */
    uint32_t cw_mask;                  /* 选中的 CW 集合：bit n = plane*CW_NUM + cw。
                                        * 命令 plane 集合外的位恒 0；稀疏（可只选 CW1/CW3）。
                                        * 空掩码（无 desc 的退化命令）合法：无 CW 可访问。 */
    uint8_t plane_count;               /* Plane count. 实测语义：0 = 单 plane（FC 只对 multi-plane 分支赋值），
                                        * N = multi-plane（= command->config.multi_plane）。
                                        * ⚠ 原注释写 "1 = single" 与实现不符。
                                        * ⚠ 当前 NAND 层只有 nand_dispatch_scatter_page_buffer() 读它；
                                        *   multi-plane handler 一律写死 PLANE_NUM 遍历，见 nand_dispatch_tables.c。 */
    uint8_t plane;                     /* 本命令目标物理 plane 索引。
                                        * 单 plane：= paa_to_plane(command->paa)，该 plane 的掩码段非空。
                                        * multi-plane：恒 0 —— 多 plane uop 硬编码 row[0]/row[1]
                                        *   （onfi_1_0.c），plane 维从 0 起。 */
    uint8_t page_type : 2;             /* LP/MP/UP/TP — LDPC engine needs this for decode threshold. */
    uint8_t reserved  : 6;             /* Reserved for future data-path bits. */
    uint8_t feature_addr;              /* Feature register addr for SET/GET_FEATURES (column).
                                        * 内联数据不走本结构：FC 把 cw_buffers[plane*CW_NUM] 指向
                                        * 控制器侧暂存（g_fc_return_staging），见 fc.c。 */
    nand_paa_t plane_paa[PLANE_NUM];   /* 各 plane 的行地址承载 PAA（CW 位由 paa_to_nand_row 丢弃）。
                                        * FC 为命令 plane 集合内**每个** plane 填一条，即使该 plane
                                        * 一个 CW 都没选中（如只读 plane1）——多 plane op 的 uop 硬编码
                                        * row[0]/row[1]，空 plane 不填会退化成 block0/page0。
                                        * 未使用的格为确定性零值（结构整体 {0} 初始化）。 */
    uint8_t *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_mask 有效；NULL = 该 CW 无数据。 */
} nand_dispatch_request_t;

typedef struct
{
    uint16_t command_id;
    uint8_t  execution_status;
    uint8_t  nand_status;
    uint8_t  cw_status[CW_TOTAL_MAX];     /* [n]，n = plane*CW_NUM + cw（与请求掩码同空间）。 */
    uint8_t  ecc_bit_count[CW_TOTAL_MAX]; /* [n]，同上。 */
    uint16_t failed_descriptor_id;
} nand_dispatch_completion_t;

/* FC_STATUS_* / FC_CW_STATUS_* macros are needed only in the .c implementation. */
#ifndef FC_CW_STATUS_UNUSED
#    define FC_CW_STATUS_UNUSED (0U)
#endif

/* Initialize fixed Channel Dispatch Queues. This function does not initialize FMC hardware. */
nand_result_t nand_dispatch_init(void);

/* NAND/FMC initialisation succeeded; FC may submit media commands after this.
 * ⚠ 这不是 ONFI 的 "device ready"（R/B# / status bit 6，per-die 且瞬态）——
 *   那由引擎 WAIT_READY 现查。此闩锁表示"FMC 已由同步初始化路径移交给异步 dispatch"。 */
void nand_dispatch_set_armed(uint8_t armed);

/*
 * 查询异步路径放行闩锁（归一化 0/1）。
 * 供 NAND 驱动层的同步入口 nand_fc_execute 断言"仅初始化阶段可调"的契约：
 * 放行后必须走 nand_fc_start/step 异步路径，否则阻塞式执行会卡住调度器。
 */
uint8_t nand_dispatch_is_armed(void);

/* Submit one pre-resolved command to its LUN FIFO (count = 1). */
nand_result_t nand_dispatch_submit(const nand_dispatch_request_t *request);

/*
 * 原子提交整条命令链到目标 LUN 的 FIFO（追加 count 槽）。
 * requests[0..count-1] 必须在同一 ch/ce/lun。
 * 异步路径未放行（nand_dispatch_set_armed 之前）返回 INVALID_ARGUMENT。
 * LUN FIFO 剩余深度不足整链时返回 NOT_READY，无副作用。
 * 追加后链占据连续槽位、按 FIFO 顺序执行，中间不可被后续命令插入。
 * 每条命令各回一份 completion（由 poll 逐条返回）。
 */
nand_result_t nand_dispatch_submit_chain(const nand_dispatch_request_t *requests, uint8_t count);

/* 完成回调：poll 每产出一条完成就调用一次，调用方在回调里当场消费即可，无需缓存。 */
typedef void (*nand_dispatch_sink_t)(const nand_dispatch_completion_t *completion, void *ctx);

/*
 * 推进全部 (ch, ce, lun)：每个 LUN 每轮恰好 step 一次，完成经 sink 逐条交出。
 * 返回本轮产出的完成条数（0 = 无完成，上限 CH_NUM×CE_NUM×LUN_NUM）。
 * 不早退——公平性依赖"每个 LUN 每轮都被走到"（见 nand_dispatch.c 实现要点）。
 */
uint8_t nand_dispatch_poll(nand_dispatch_sink_t sink, void *ctx);

/*
 * 查询目标 LUN FIFO 的剩余槽数（0 .. NAND_DISPATCH_CHAIN_DEPTH）。
 * 供 FC 填充时判断能否放下整条链；ch/ce/lun 越界是调用者编程错误（断言）。
 */
uint8_t nand_dispatch_lun_free(uint8_t ch, uint8_t ce, uint8_t lun);

#endif /* NAND_DISPATCH_H */