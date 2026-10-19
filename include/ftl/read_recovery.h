#ifndef FTL_READ_RECOVERY_H
#define FTL_READ_RECOVERY_H

/*
 * Read Recovery — 读取失败恢复 + ECC 趋势预测
 *
 * 企业级 SSD 在读取失败时不是立即返回错误，而是走多级恢复管线：
 *
 *   Stage 1 — Read Retry（读重试）
 *     切换 NAND 读电压/时序参数，重试读取。
 *     ONFI 标准通过 SET_FEATURES 写入 Read Retry 模式。
 *
 *   Stage 2 — 邻近页辅助
 *     读取目标页的上下邻居页，利用 NAND 单元间电压干扰
 *     的物理特性辅助纠错。适用于 ECC 略超纠错上限的场景。
 *
 *   Stage 3 — XOR/RAID 恢复（未实现）
 *     跨 Plane 的 Stripe RAID 数据恢复。
 *
 *   Stage 4 — 坏块标记 + 数据搬移
 *     确定 Block 不可靠后，标记坏块并搬移剩余有效数据。
 *
 * ECC 趋势预测：
 *   ECC 连续 N 次上升 → 该 Block 进入"慢死"（Slow Death）状态
 *   → 在数据彻底损坏前主动搬移。
 *
 * 当前实现：
 *   ① ECC 趋势跟踪（per-Block bit 计数）
 *   ② 预测性坏块检测
 *   ③ Read Retry 重试循环（通过 FC SET_FEATURES 切模式 + 重读）
 *   ④ 读失败后的恢复调度入口
 *
 * ⚠ 恢复路径是**异步状态机**（ftl_read_recovery_t + rr_kick +
 *   ftl_op_read_recovery）：读失败的完成服务函数把 pending 子操作托管给 job 就返回，
 *   恢复在 rr_kick / 完成服务函数里分步推进，不在完成泵里嵌套同步等待。
 */

#include "allocator.h"
#include "bst.h"        /* rr_ecc_track_t, rr_block_health_t, ftl_block_t */
#include "fc_command.h" /* fc_command_status_t（服务函数签名） */
#include "ftl_config.h" /* nand_pba_t */
#include "ftl_type.h"
#include "l2p_cache.h"

#include <stdint.h>

/* 前向声明（ftl_context_t 在 ftl.h 中定义；本文件被 ftl.h include，用 struct 标签打破环） */
struct ftl_context;
typedef struct ftl_context ftl_context_t;

/* ── ECC 趋势参数 ── */
#define RR_ECC_RISING_THRESHOLD (4U)  /* 连续上升 ≥4 次触发预测 */
#define RR_ECC_HIGH_WATER       (6U)  /* 单次 ECC ≥6 bits → 告警 */
#define RR_ECC_MAX              (15U) /* BCH t=4 最大可纠 bit 数（保守上限）*/
#define RR_MAX_RETRY            (8U)  /* 最大 read retry 次数 */
#define RR_NEIGHBOR_ATTEMPTS    (2U)  /* 邻近页辅助尝试次数 */

/* read disturb 慢死阈值——单块自上次擦除以来的累计读次数超此值即视为慢死候选。
 * read_count 由 host 读 completion（ftl.c）与 GC/WL 搬移读（migrate.c）共同累加；
 * 取 SLC MT29F4G08 读干扰容忍度量级（NAND 读寿命 ~10^5 次，保守取每块 1000 次
 * 触发搬移，远早于数据损坏）。擦除成功会把 read_count 清零（allocator_note_erase）。 */
#define RR_READ_DISTURB_THRESHOLD (1000U)

/* ── Read Retry ONFI Feature Address ──
 * ⚠ 0x89 在 MT29F4G08 上是**保留地址**：SET_FEATURES 不产生实际效果，Stage 1
 *   在本器件上自始空转（用户明确不处理）。保留该路径只为接口/状态机完整。 */
#define RR_FEATURE_READ_RETRY      (0x89U) /* 本器件保留；见上 */
#define RR_FEATURE_READ_RETRY_MODE (0x00U) /* Mode 0 = normal, 1-7 = retry modes */

/* 预测性坏块状态 rr_block_health_t / ECC 历史 rr_ecc_track_t 已迁入 ftl/bst.h */

/* ── Read Recovery 上下文（= 异步 job 状态机）──
 *
 * ECC 追踪数组在 ftl_block_t.rr（Block State Table 单数据源）；本上下文承载
 * **整台恢复状态机**（per-instance 单 job，放 ftl->rr）。
 *
 * 为什么是「托管」而不是「当场恢复」：当场恢复要在完成服务函数里同步等待每一级命令
 * ⇒ 在完成泵里再开一个完成泵（嵌套泵），且整条 host 读流水线被一个坏页钉住。
 * 改为：读失败时把 pending 子操作**托管**给本 job（保留 pending 项、不递减组在飞数），
 * job 按 stage 一次只提交一条命令，完成由 g_op_service[FTL_OP_READ_RECOVERY] 推进；
 * host 读组继续 pending，直到 job 收尾统一结算。
 */

/* 状态机的 stage。约定：**stage 恒表示"当前该做/正在等的那一步"**——
 *   · 纯计算 stage（IDLE / PLANE / PLANE_END / FINISH）只从 rr_step 进入，
 *     跑完立刻转下一个 stage（不提交命令）；
 *   · 提交 stage（SF_SET / READ / SF_RESTORE / NB_READ / NB_RETRY）由 rr_step 提交命令
 *     后**原地停留**，等完成服务函数改写 stage。
 * 判「有没有命令在飞」用 waiting 而不是 stage：提交可能因背压失败（stage 不变但无命令）。 */
typedef enum
{
    RR_STAGE_IDLE = 0,   /* 空闲：无 job（= 无在飞的恢复） */
    RR_STAGE_PLANE,      /* 选下一个 plane 的目标页（纯计算） */
    RR_STAGE_SF_SET,     /* 在飞：SET_FEATURES(attempt) —— 切入读重试电压模式 */
    RR_STAGE_READ,       /* 在飞：重试读目标页 → rr_page_scratch */
    RR_STAGE_SF_RESTORE, /* 在飞：SET_FEATURES(0) —— 恢复默认读模式 */
    RR_STAGE_NB_READ,    /* 在飞：Stage 2 读邻居页（只为"这页读得出来"这个事实） */
    RR_STAGE_NB_RETRY,   /* 在飞：Stage 2 邻居读成功后再试目标页 */
    RR_STAGE_PLANE_END,  /* 结算本 plane：拷回 buffer 或清零 + 标坏块（纯计算） */
    RR_STAGE_FINISH      /* 收尾：释放 pending 槽 + 组聚合（纯计算） */
} rr_stage_t;

/*
 * rr_plane_step_t - rr_select_plane 的结果（RR_STAGE_PLANE 一步）
 *
 * ⚠ 这里的 -1（RR_PLANE_WAIT）**不是硬失败**——是「该 LBA 的 L2P region 还在异步加载，
 *   原地等」；调用者据此**清零** stall 计数（加载在飞是有确定完成时刻的进展）。故它不能
 *   与 md_compact_step_t / cp_flush_step_t 共用一套名字（那两处的 -1 都要求 abort）。
 */
typedef enum
{
    RR_PLANE_WAIT  = -1, /* region 尚未就绪（加载在飞）：调用者原地等，不计卡死 */
    RR_PLANE_SKIP  = 0,  /* 本子操作不覆盖本 plane：调用者 plane++ 跳过 */
    RR_PLANE_READY = 1,  /* 找到本 plane 的目标页：进入 SF_SET */
} rr_plane_step_t;

/* 连续「提交不出去」的轮数上限：超过即判卡死，按恢复失败收尾。
 * 正常背压（命令池被在飞的 host I/O 占满）下一两轮就恢复；预算取 256 轮留足余量。
 * ⚠ 只兜「提交侧」——命令一旦在飞，NAND 层自身的 wait/jump 超时保证它**必有**完成
 *   （超时置 FC_CW_STATUS_TIMEOUT），故不需要（也不该）在飞时强拆 job。 */
#define RR_SUBMIT_STALL_MAX (256U)

typedef struct
{
    uint8_t    stage;        /* rr_stage_t */
    uint8_t    waiting;      /* 1 = 有一条 RR 命令在飞（等 g_op_service[FTL_OP_READ_RECOVERY]） */
    uint8_t    plane;        /* 当前 plane（0..PLANE_NUM-1）；== PLANE_NUM ⇒ 收尾 */
    uint8_t    attempt;      /* Stage 1 读重试计数（1..RR_MAX_RETRY） */
    uint8_t    neighbor_i;   /* Stage 2 邻居序号（0..RR_NEIGHBOR_ATTEMPTS-1） */
    uint8_t    pending_idx;  /* 被托管的 pending 槽下标（收尾据此释放） */
    uint8_t    first;        /* 本 plane 首个被覆盖 CW 在子操作内的偏移（诊断用 LBA） */
    uint8_t    recovered;    /* 本 plane 是否恢复成功（PLANE_END 结算用） */
    uint8_t    all_ok;       /* 1 = 本 job 覆盖的所有 plane 都恢复成功（收尾状态用） */
    uint16_t   stall_rounds; /* 连续提交不出去的轮数（见 RR_SUBMIT_STALL_MAX，> uint8_t 上限） */
    lba_t      page_lba;     /* 本 plane 目标页的**页** LBA（META 比对用） */
    nand_pba_t page;         /* 本 plane 目标页 PBA（NAND_PBA_INVALID = 本子操作不覆盖本 plane） */
    nand_pba_t neighbor;     /* Stage 2 邻居页 PBA（NB_READ 提交前算好） */
    uint8_t    retry_count;  /* 本次恢复已用的读重试次数（观测，收尾 LOGE 用） */

    /* 心跳基准（见 ftl_job.h）：job 在飞期间每满 1s 打一行 stage/age */
    uint32_t start_ms;
    uint32_t log_ms;
} ftl_read_recovery_t;

/* ── 接口 ── */

/*
 * rr_record_ecc - 记录一次读取的 ECC bit 数，更新趋势
 *
 * 每次 read 完成后记录 ecc_bits，与上一次读数比较：
 * 连续上升 → warning → critical → 触发搬移。
 *
 * @param track    per-Block ECC 追踪（&ftl->alloc.blocks[gblock].rr）
 * @param ecc_bits 本次读取的 ECC 纠正 bit 数（取 desc 链全链 CW 的最大值，见 fc_get_cmd_ecc_max）
 */
void rr_record_ecc(rr_ecc_track_t *track, uint8_t ecc_bits);

/*
 * rr_kick - 推进 Read Recovery job（唯一的驱动入口，每轮 ftl_service 调一次）
 *
 * 三件事，按优先级：
 *   ① job 空闲 ⇒ 找有没有被托管的 pending 子操作，有则领走并起第一步；
 *   ② 命令在飞 ⇒ 直接返回（等 g_op_service[FTL_OP_READ_RECOVERY]）；
 *   ③ 未在飞又推进不下去（提交侧背压）⇒ 累计 stall_rounds，超 RR_SUBMIT_STALL_MAX
 *      判卡死，按「恢复失败」收尾（避免 host 读被一个泄漏的命令池永久挂起）。
 *
 * 幂等且廉价：job 空闲且无托管项时只是一次 32 项线性扫描。
 * ⚠ 本函数**不**同步等待；提交走 ftl_async_* + ftl_op_submit。
 *
 * @param ftl FTL 实例
 */
void rr_kick(ftl_context_t *ftl);

/* 完成服务函数 ftl_op_read_recovery 与其余服务函数同处声明（ftl_op.h）——
 * 那里是"g_op_service 各槽"的唯一权威列表，此处不再重复声明。 */

/*
 * rr_check_predictive - 检查是否需要预测性搬移
 *
 * ECC 趋势连续上升 ≥ RR_ECC_RISING_THRESHOLD 次
 * 或单次 ECC ≥ RR_ECC_HIGH_WATER → 返回该 Block。
 *
 * @param blocks    Block State Table（全表）
 * @param max_blocks 表大小（FTL_BLOCK_TOTAL）
 * @param gblock    输出：需搬移的全局 Block 编号
 * @return true 需要搬移，false 不需要
 */
bool rr_check_predictive(const ftl_block_t *blocks, uint16_t max_blocks, uint16_t *gblock);

/*
 * rr_get_health - 获取 Block 健康状态
 *
 * @param track per-Block ECC 追踪（&blocks[gblock].rr）
 * @return rr_block_health_t
 */
rr_block_health_t rr_get_health(const rr_ecc_track_t *track);

/*
 * rr_predictive_scan - 巡检：找第一个 health ≥ CRITICAL 的 Closed 块
 *
 * 只扫 CLOSED（跳过 OPEN/FREE/BAD/LOG/RETIRED），返回全局物理 Block 号。
 * 慢死预测命中 Open SB 的块不搬（还在写），等关闭后进 closed 再巡检。
 *
 * @param alloc  FTL allocator（含 blocks 表）
 * @param gblock 输出：命中的全局 Block 号
 * @return true 命中，false 无
 */
bool rr_predictive_scan(const ftl_allocator_t *alloc, uint16_t *gblock);

/*
 * rr_retire_block - 退役慢死块（不再分配新数据，不标坏）
 *
 * 有效数据已由调用方搬走，本函数：
 *   - 状态机：置 RETIRED（互斥枚举 ⇒ 一行赋值即抹掉原状态；温度字段正交，不动）
 *   - 清反向索引 sb_id → NO_SB
 *   - 复位 rr 健康（health=HEALTHY、rising_count=0、last_ecc=0、read_count=0）
 *     块仍可读（ECC 能纠），只是不再当新块分配。
 *
 * @param b 目标物理块
 */
void rr_retire_block(ftl_block_t *b);

#endif /* FTL_READ_RECOVERY_H */