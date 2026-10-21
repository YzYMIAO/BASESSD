#ifndef FTL_READ_RECOVERY_H
#define FTL_READ_RECOVERY_H

/*
 * Read Recovery — 读取失败恢复 + ECC 趋势预测
 *
 * 企业级 SSD 的读恢复管线通常是：Read Retry（切读电压）→ 邻居页辅助 → XOR/RAID。
 * 本仓器件/配置下前两级**无效**：
 *   · MT29F4G08 的 Read Retry feature（0x89）是**保留地址**，SET_FEATURES 不产生效果；
 *   · SLC 无读重试表，读邻居页也无法恢复目标页；
 *   · RAID 关闭（`CONFIG_FC_RAID=0`）。
 * ⇒ 本仓只保留：**一次正常读 + 仅 `CRC_ERROR` 重读一次**；UNC / NAND_FAIL /
 *   MATA_ERROR 直接判失败并走块退役。（主流并非没有多级恢复——是本器件/本配置下
 *   这两级无效，故不实现。）
 *
 * ECC 趋势预测：
 *   ECC 连续 N 次上升 → 该 Block 进入"慢死"（Slow Death）状态
 *   → 在数据彻底损坏前主动搬移。
 *
 * 当前实现：
 *   ① ECC 趋势跟踪（per-Block bit 计数）
 *   ② 预测性坏块检测
 *   ③ 读失败后的恢复调度入口（单次读 + CRC 重读一次）
 *   ④ 不可恢复 ⇒ 各 CW 清零 + 块退役
 *
 * ⚠ 恢复路径是**异步状态机**（rr_handle_t + rr_handle +
 *   ftl_read_recovery_callback）：读失败的完成服务函数把 pending 子操作托管给 job 就返回，
 *   恢复在 rr_handle / 完成服务函数里分步推进，不在完成泵里嵌套同步等待。
 */

#include "allocator.h"
#include "bst.h"        /* rr_ecc_track_t, rr_block_health_e, bst_block_t */
#include "fc_command.h" /* fc_command_status_t（服务函数签名） */
#include "ftl_config.h" /* DWORD */
#include "ftl_module.h" /* bg_info_t / ftl_mod_status_t */
#include "ftl_type.h"
#include "l2p_cache.h"
#include "pending.h" /* pend_table_t */

#include <stdint.h>

/* 前向声明（ftl_ctx_t 在 ftl.h 中定义；本文件被 ftl.h include，用 struct 标签打破环） */
struct ftl_context;
typedef struct ftl_context ftl_ctx_t;

/* ── ECC 趋势参数 ── */
#define RR_ECC_RISING_THRESHOLD (4U)  /* 连续上升 ≥4 次触发预测 */
#define RR_ECC_HIGH_WATER       (6U)  /* 单次 ECC ≥6 bits → 告警 */
#define RR_ECC_MAX              (15U) /* BCH t=4 最大可纠 bit 数（保守上限）*/
/* read disturb 慢死阈值——单块自上次擦除以来的累计读次数超此值即视为慢死候选。
 * read_count 由 host 读 completion（ftl.c）与 GC/WL 搬移读（migrate.c）共同累加；
 * 取 SLC MT29F4G08 读干扰容忍度量级（NAND 读寿命 ~10^5 次，保守取每块 1000 次
 * 触发搬移，远早于数据损坏）。擦除成功会把 read_count 清零（alc_note_erase）。 */
#define RR_READ_DISTURB_THRESHOLD (1000U)

/* 慢死预测巡检节拍（由 migrate handle 自持）：≥1s 才扫一轮（read disturb 是慢变量，
 * 频繁全扫无谓开销）。 */
#define RR_SCAN_INTERVAL_MS (1000U)

/* 预测性坏块状态 rr_block_health_e / ECC 历史 rr_ecc_track_t 已迁入 ftl/bst.h */

/* ── Read Recovery 上下文（= 异步 job 状态机）──
 *
 * ECC 追踪数组在 bst_block_t.rr（Block State Table 单数据源）；本上下文承载
 * **整台恢复状态机**（per-instance 单 job，放 ftl->read_recovery_handle）。
 *
 * 为什么是「托管」而不是「当场恢复」：当场恢复要在完成服务函数里同步等待每一级命令
 * ⇒ 在完成泵里再开一个完成泵（嵌套泵），且整条 host 读流水线被一个坏页钉住。
 * 改为：读失败时把 pending 子操作**托管**给本 job（保留 pending 项、不递减组在飞数），
 * job 按 stage 一次只提交一条命令，完成由 g_op_callbacks[FTL_OP_READ_RECOVERY] 推进；
 * host 读组继续 pending，直到 job 收尾统一结算。
 */

/* 状态机的 stage。约定：**stage 恒表示"当前该做/正在等的那一步"**——
 *   · 纯计算 stage（IDLE / PLANE / PLANE_END / FINISH）只从 rr_step 进入，
 *     跑完立刻转下一个 stage（不提交命令）；
 *   · 提交 stage（READ）由 rr_step 提交命令
 *     后**原地停留**，等完成服务函数改写 stage。
 * 判「有没有命令在飞」用 waiting 而不是 stage：提交可能因背压失败（stage 不变但无命令）。 */
typedef enum
{
    RR_STAGE_IDLE = 0,  /* 空闲：无 job（= 无在飞的恢复） */
    RR_STAGE_PLANE,     /* 选下一个 plane 的目标页（纯计算） */
    RR_STAGE_READ,      /* 在飞：读目标页 → rr_page_scratch（CRC 错时允许一次重读） */
    RR_STAGE_PLANE_END, /* 结算本 plane：拷回 buffer 或清零 + 标坏块（纯计算） */
    RR_STAGE_FINISH     /* 收尾：释放 pending 槽 + 组聚合（纯计算） */
} rr_stage_e;

/*
 * rr_plane_step_e - rr_select_plane 的结果（RR_STAGE_PLANE 一步）
 *
 * ⚠ 这里的 RR_PLANE_WAIT **不是硬失败**——是「该 LBA 的 L2P region 还在异步加载，
 *   原地等」；调用者据此**清零** stall 计数（加载在飞是有确定完成时刻的进展）。故它不能
 *   与 md_compact_step_e / cp_flush_step_e 共用一套名字（那两处的 FAILED 都要求 abort）。
 */
typedef enum
{
    RR_PLANE_SKIP  = 0U, /* 本子操作不覆盖本 plane：调用者 plane++ 跳过 */
    RR_PLANE_READY = 1U, /* 找到本 plane 的目标页：进入 READ */
    RR_PLANE_WAIT  = 2U, /* region 尚未就绪（加载在飞）：调用者原地等，不计卡死 */
} rr_plane_step_e;

/* 提交 deadline：连续提交不出去超过 `FTL_SUBMIT_TIMEOUT_MS` 即判卡死，
 * 按恢复失败收尾。⚠ 只兜「提交侧」——命令一旦在飞，NAND 层自身的 wait/jump 超时保证
 * 它必有完成（超时置 FC_CW_STATUS_TIMEOUT），故不在飞时强拆 job。
 * 时钟缺失（now_ms == 0）时 deadline 不生效；本板 time_us 恒非空 ⇒ 不可达。 */

/*
 * rr_owner_t — 被托管读子操作的身份（docs §4.5）
 *
 * 本仓采用**compact 重下发**机制（与主流一致：失败读释放后按 LBA/PBA 重新下发新命令
 * + 新 buffer），不保留失败命令的 desc 链。owner 只携带结算与数据回填所需的最小信息。
 * RR 只持有 pending 表指针：托管项的领取/结算走 pending owner API。
 */
typedef struct
{
    DWORD owners;      /* 贡献过的完成组位掩码 */
    WORD  pending_idx; /* 被托管的 pending 槽 */
    BYTE *upper_buf;   /* host 数据缓冲（恢复结果按 CW 偏移写回） */
    DWORD lba;         /* 子操作起始全局 LBA */
    BYTE  n_cw;        /* 子操作覆盖的 CW 数 */
} rr_owner_t;

typedef struct
{
    rr_stage_e stage;             /* 异步恢复状态机 */
    bool       waiting;           /* true = 有一条 RR 命令在飞（等 g_op_callbacks[FTL_OP_READ_RECOVERY]） */
    BYTE       plane;             /* 当前 plane（0..PLANE_NUM-1）；== PLANE_NUM ⇒ 收尾 */
    BYTE       attempt;           /* 本 plane 已发起的读次数（1 = 首次，2 = CRC 重读） */
    BYTE       pending_idx;       /* 被托管的 pending 槽下标（收尾据此释放） */
    BYTE       first;             /* 本 plane 首个被覆盖 CW 在子操作内的偏移（诊断用 LBA） */
    bool       recovered;         /* 本 plane 是否恢复成功（PLANE_END 结算用） */
    bool       all_ok;            /* true = 本 job 覆盖的所有 plane 都恢复成功（收尾状态用） */
    DWORD      stall_ms;          /* 本轮连续「提交不出去」的起点（0 = 未计时） */
    DWORD      page_lba;          /* 本 plane 目标页的**页** LBA（META 比对用） */
    DWORD      page;              /* 本 plane 目标页 PBA（NAND_PBA_INVALID = 本子操作不覆盖本 plane） */
    BYTE       retry_count;       /* 本次恢复已用的读重试次数（观测，收尾 LOGE 用） */
    BYTE       planes_recovered;  /* 本次 job 恢复成功的 plane 数（观测） */
    BYTE       planes_failed;     /* 本次 job 恢复失败的 plane 数（观测） */
    DWORD      last_status;       /* 最近一次读完成状态（FC_CW_STATUS_*，日志解码用） */
    DWORD      already_bad_reads; /* host 重读已退役块的累计次数（观测） */
    /* 已退役块重读日志：每 gblock 只打一次，其余只计数（防 host 反复读同一 LBA 刷屏）。 */
    DWORD already_bad_logged[FTL_BLOCK_TOTAL / 32U];

    /* 窄上下文：只持有实际使用的 owner 指针，不持有 ftl_context / 端口结构体。 */
    l2p_cache_t  *cache;
    alc_t        *alloc;
    ftl_topo_t    topo;
    BYTE         *page_scratch;
    pend_table_t *pending;

    rr_owner_t owner;     /* 当前被托管的子操作 */
    bool       has_owner; /* true = owner 有效（rr_trigger 登记） */
    int32_t    error;     /* 最近一次 abort 的归因 */

    /* 心跳基准（见 ftl_job.h）：job 在飞期间每满 1s 打一行 stage/age */
    DWORD start_ms;
    DWORD log_ms;
} rr_handle_t;

/* ── 模块 handle（typed 入口） ── */
typedef struct
{
    ftl_mod_status_t base;

    rr_stage_e stage; /* 观测；外部不要据此驱动 */
    bool       waiting;
    bool       all_ok;  /* 本次 job 所有 plane 都恢复成功 */
    BYTE       retries; /* 已用读重试次数 */
    BYTE       planes_recovered;
    BYTE       planes_failed;
    WORD       desc_count; /* 保留的 desc 数（compact 机制恒 0） */
} rr_status_t;

/* rr_init - 绑定窄上下文并复位 handle。 */
void rr_init(rr_handle_t *rr, l2p_cache_t *cache, alc_t *alloc, ftl_topo_t topo, BYTE *page_scratch, pend_table_t *pending);

/* rr_trigger - 登记一个被托管的读子操作（不立即推进）。 */
void rr_trigger(rr_handle_t *rr, const rr_owner_t *owner);

/*
 * rr_handle - 推进 Read Recovery job（每轮 must-run；提交即返回）。
 *
 * @param rr    handle；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param info  本轮动态信息（当前未用，可传 NULL）
 * @param out   出参：本轮结束后的上报（**非空**）
 */
void rr_handle(rr_handle_t *rr, const bg_info_t *info, rr_status_t *out);

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
void rr_record_ecc(rr_ecc_track_t *track, BYTE ecc_bits);

/*
 * rr_record_block_ecc - 记录 gblock 一次读的 ECC 趋势并累加 read_count
 *
 * 迁移读路径不直接摸 allocator->blocks[]：由 RR（趋势数据 owner）提供唯一写入点。
 *
 * @param alloc    分配器（持有 BST）
 * @param gblock   全局块号
 * @param ecc_bits 本次读最大纠正 bit 数
 */
void rr_record_block_ecc(alc_t *alloc, WORD gblock, BYTE ecc_bits);

/*
 * rr_retire_block_at - 退役 gblock（复位 read_count/ECC 趋势）
 *
 * @param alloc   分配器（持有 BST）
 * @param gblock  全局块号
 */
void rr_retire_block_at(alc_t *alloc, WORD gblock);

/* 完成服务函数 ftl_read_recovery_callback 与其余服务函数同处声明（ftl_op.h）——
 * 那里是"g_op_callbacks 各槽"的唯一权威列表，此处不再重复声明。 */

/*
 * rr_should_predict - 检查是否需要预测性搬移
 *
 * ECC 趋势连续上升 ≥ RR_ECC_RISING_THRESHOLD 次
 * 或单次 ECC ≥ RR_ECC_HIGH_WATER → 返回该 Block。
 *
 * @param blocks    Block State Table（全表）
 * @param max_blocks 表大小（FTL_BLOCK_TOTAL）
 * @param gblock    输出：需搬移的全局 Block 编号
 * @return true 需要搬移，false 不需要
 */
bool rr_should_predict(const bst_block_t *blocks, WORD max_blocks, WORD *gblock);

/*
 * rr_get_health - 获取 Block 健康状态
 *
 * @param track per-Block ECC 追踪（&blocks[gblock].rr）
 * @return rr_block_health_e
 */
rr_block_health_e rr_get_health(const rr_ecc_track_t *track);

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
bool rr_predictive_scan(const alc_t *alloc, WORD *gblock);

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
void rr_retire_block(bst_block_t *b);

#endif /* FTL_READ_RECOVERY_H */