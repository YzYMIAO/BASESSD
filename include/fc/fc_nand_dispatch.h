#ifndef FC_NAND_DISPATCH_H
#define FC_NAND_DISPATCH_H

#include "fc_nand_engine.h"
#include "nand_config.h"

#include <stdint.h>

/* One fixed per-LUN FIFO (depth NAND_DISPATCH_CHAIN_DEPTH) is attached to each LUN.
 * 深度定义在 nand_config.h（FTL cache batch 也按它派生批次上限）。 */

/*
 * 每个 LUN 一个待处理 FIFO，深度 = NAND_DISPATCH_CHAIN_DEPTH。
 * **每槽 = 一条命令**：NAND 侧看不到「链」，一条链 = 连续 N 槽。
 * FC 用 fc_nand_dispatch_submit_chain() 把整链原子批量追加（空间不足则整体拒收），
 * poll 遍历每 LUN FIFO，先进先出，每轮每 LUN 推进一条、完成一条即回一份 completion。
 * 因此每条命令各有一份 completion，链中间命令的状态独立上报。
 */

/*
 * Self-contained request struct. FC resolves all descriptor-chain data
 * (CW mask, per-plane row PAA, buffer pointers) at submit time.
 * nand_dispatch never calls back into FC after receiving this struct.
 *
 * ── 唯一索引空间：flat 绝对 CW 号 ──
 * n = plane * CW_NUM + cw（0 .. CW_TOTAL_MAX-1）。
 * cw_bitmap 的 bit n、cw_buffers[n]、completion.cw_status[n] / ecc_bit_count[n]
 * 全部用这一个编号（FC 侧的 desc 链序也按同一公式展开）。链序、plane-major
 * 密排槽位等中间坐标已全部取消。
 * 行地址不参与该空间：它是 per-plane 的，走 plane_paa[plane]。
 */
typedef struct FC_NAND_DISPATCH_REQUEST_S
{
    WORD  command_id;               /* FC command pool slot for completion correlation. */
    BYTE  operation;                /* OP_* constant. */
    BYTE  ch;                       /* Channel. */
    BYTE  ce;                       /* Chip-enable. */
    BYTE  lun;                      /* LUN. */
    DWORD cw_bitmap;                /* 选中的 CW 集合：bit n = plane*CW_NUM + cw。
                                     * 命令 plane 集合外的位恒 0；稀疏（可只选 CW1/CW3）。
                                     * **字面位图**：0 = 空集（无 desc 的退化命令，合法；
                                     * 擦除即空集，CW0 位保持 0）。 */
    BYTE plane_count;               /* 0 = 单 plane；N = multi-plane（= command->config.multi_plane）。
                                     * 当前只有 scatter 读它；multi-plane handler 按 PLANE_NUM 遍历。 */
    BYTE plane;                     /* 本命令目标物理 plane 索引。
                                     * 单 plane：= paa_to_plane(command->paa)，该 plane 的掩码段非空。
                                     * multi-plane：恒 0 —— 多 plane uop 硬编码 row[0]/row[1]
                                     *   （onfi_1_0.c），plane 维从 0 起。 */
    BYTE page_type : 2;             /* LP/MP/UP/TP — LDPC engine needs this for decode threshold. */
    BYTE reserved  : 6;             /* Reserved for future data-path bits. */
    BYTE feature_addr;              /* Feature register addr for SET/GET_FEATURES (column).
                                     * 内联数据不走本结构：FC 把 cw_buffers[plane*CW_NUM] 指向
                                     * 控制器侧暂存（g_fc_return_staging），见 fc.c。 */
    WORD column;                    /* **显式列地址**：只有 OP_READ_COLUMN 读它（列来自命令级
                                     * param 字节 0/1）。其余 op 的列地址各有来源（paa 的 cw
                                     * 字段 / feature_addr / 固定常量），本字段恒为 0。
                                     * ⚠ feature_addr 是 8 位、装不下 spare 区列号（2048 需 12 位），
                                     *   故不能复用；两者语义不同，别合并。 */
    DWORD plane_paa[PLANE_NUM];     /* 各 plane 的行地址承载 PAA（CW 位由 paa_to_nand_row 丢弃）。
                                     * FC 为命令 plane 集合内**每个** plane 填一条，即使该 plane
                                     * 一个 CW 都没选中（如只读 plane1）——多 plane op 的 uop 硬编码
                                     * row[0]/row[1]，空 plane 不填会退化成 block0/page0。
                                     * 未使用的格为确定性零值（结构整体 {0} 初始化）。 */
    BYTE *cw_buffers[CW_TOTAL_MAX]; /* [n]，仅 n ∈ cw_bitmap 有效；NULL = 该 CW 无数据。 */
} fc_nand_dispatch_req_s;

typedef struct FC_NAND_DISPATCH_COMPLETION_S
{
    WORD          command_id;
    nand_result_e execution_status;
    BYTE          nand_status;                 /* OP 后读状态（FAIL/FAILC） */
    BYTE          pre_status;                  /* OP 前读状态（RDY/ARDY/WP） */
    BYTE          cw_status[CW_TOTAL_MAX];     /* [n]，n = plane*CW_NUM + cw（与请求掩码同空间）。 */
    BYTE          ecc_bit_count[CW_TOTAL_MAX]; /* [n]，同上。 */
    WORD          failed_cw;                   /* flat CW 编号；NAND_CW_INVALID = 无失败 CW。 */
} fc_nand_dispatch_comp_s;

/* FC_STATUS_* / FC_CW_STATUS_* macros are needed only in the .c implementation. */
#ifndef FC_CW_STATUS_UNUSED
#    define FC_CW_STATUS_UNUSED (0U)
#endif

/* Initialize fixed Channel Dispatch Queues. This function does not initialize FMC hardware. */
nand_result_e fc_nand_dispatch_init(void);

/* 异步放行闩锁本体在 fc_nand_engine.h（fc_nand_engine_set_armed / _is_armed）。
 * dispatch 只查询，不再提供自己的 set/is 入口。 */

/* Submit one pre-resolved command to its LUN FIFO (count = 1). */
nand_result_e fc_nand_dispatch_submit(const fc_nand_dispatch_req_s *request);

/*
 * 原子提交整条命令链到目标 LUN 的 FIFO（追加 count 槽）。
 * requests[0..count-1] 必须在同一 ch/ce/lun。
 * 异步路径未放行（fc_nand_engine_set_armed(1) 之前）返回 INVALID_ARGUMENT。
 * LUN FIFO 剩余深度不足整链时返回 NOT_READY，无副作用。
 * 追加后链占据连续槽位、按 FIFO 顺序执行，中间不可被后续命令插入。
 * 每条命令各回一份 completion（由 poll 逐条返回）。
 */
nand_result_e fc_nand_dispatch_submit_chain(const fc_nand_dispatch_req_s *requests, BYTE count);

/* 完成回调：poll 每产出一条完成就调用一次，调用方在回调里当场消费即可，无需缓存。 */
typedef void (*fc_nand_dispatch_sink_fn_t)(const fc_nand_dispatch_comp_s *completion, void *ctx);

/*
 * 推进全部 (ch, ce, lun)：每个 LUN 每轮恰好 step 一次，完成经 sink 逐条交出。
 * 返回本轮产出的完成条数（0 = 无完成，上限 CH_NUM×CE_NUM×LUN_NUM）。
 * 不早退——公平性依赖"每个 LUN 每轮都被走到"（见 fc_nand_dispatch.c 实现要点）。
 */
BYTE fc_nand_dispatch_poll(fc_nand_dispatch_sink_fn_t sink, void *ctx);

/*
 * 查询目标 LUN FIFO 的剩余槽数（0 .. NAND_DISPATCH_CHAIN_DEPTH）。
 * 供 FC 填充时判断能否放下整条链；ch/ce/lun 越界是调用者编程错误（断言）。
 */
BYTE fc_nand_dispatch_lun_free(BYTE ch, BYTE ce, BYTE lun);

/**
 * @brief 目标 LUN FIFO 的已用槽数（只读，诊断用）。
 *
 * @param ch  通道号。
 * @param ce  片选号。
 * @param lun LUN 号。
 *
 * @return 已用槽数（0 .. NAND_DISPATCH_CHAIN_DEPTH）。
 *
 * @note 与 `fc_nand_dispatch_lun_phase()` 合起来区分「FIFO 满（等完成）」与
 *       「FSM 卡在某个相位」——FC 侧出现「命令在 SQ 却不下发」时用。
 */
BYTE fc_nand_dispatch_lun_used(BYTE ch, BYTE ce, BYTE lun);

/**
 * @brief 目标 LUN FSM 的当前相位（只读，诊断用）。
 *
 * @param ch  通道号。
 * @param ce  片选号。
 * @param lun LUN 号。
 *
 * @return LUN_PHASE_*（0=IDLE / 1=ISSUE / 2=ISSUED / 3=FETCH）。
 */
BYTE fc_nand_dispatch_lun_phase(BYTE ch, BYTE ce, BYTE lun);

#endif /* FC_NAND_DISPATCH_H */