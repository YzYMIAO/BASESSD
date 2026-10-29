#ifndef FTL_GC_H
#define FTL_GC_H

/*
 * Garbage Collection
 *
 * 以 Super Block 为粒度回收已写满的 Block。
 *
 * 流程：
 *   ① 从 closed_sb_list 选 victim（多维加权评分：有效页 / 磨损 / 温度 / 时间）
 *   ② 逐 Plane 逐页读 victim Super Block
 *   ③ 读 META 获取 LBA → 查 L2P → 判断页是否有效
 *   ④ 有效页 → 搬移到新的 Cold Super Block
 *   ⑤ 擦除各 Plane Block → 归还到 Free Pool
 *
 * ⚠ ②~⑤ 的执行体**不在本文件**：搬移是一个异步 job，GC 与 WL 共用同一台引擎，
 *   见 `ftl/migrate.h` / `migrate.c`（mig_handle_t + mig_gc_begin / mig_wl_begin /
 *   mig_handle + ftl_migrate_callback）。本文件只留**选 victim 与共用原语**：
 *   · gc_select_victim   —— 四项加权评分（GC 侧选块策略）
 *   · gc_decode_cw_lba   —— 已完成读命令的逐 CW META 解码（异步搬移 + 掉电重建共用）
 *   · gc_read_page_with_lba —— 同步读 + 解码（只服务掉电重建）
 *   · gc_should_trigger  —— 触发判定
 *
 * 原子性：
 *   Journal 记录每一条 L2P 变更
 *   掉电时最多遗留"孤悬"Block，下次 GC 回收
 */

#include "allocator.h"   /* alc_t, super_block_t */
#include "fc_command.h"  /* fc_command_t（gc_decode_cw_lba 的形参） */
#include "ftl_config.h"  /* DWORD */
#include "ftl_type.h"    /* DWORD */
#include "nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK */

#include <stdint.h>

/* ── GC 触发位（设计 §4.4：只有**空间压力与磨损**三条）──
 * gc_should_trigger() 返回的**位掩码**：命中的位即触发起因（可多位同时命中）。
 * 只用于调用点打日志/调参，"要不要 GC" 的判定仍是 `!= 0`。
 *
 * ⚠ 位值是对外观测契约（`[GC  ] trig=` 日志 / 测试），**不得重排**；0x02 / 0x04 / 0x20
 *   不用但缺口保留，避免旧日志/脚本按位值解读时错位。触发判据的出处：SPDK 的
 *   `ftl_needs_reloc`（自由 band 水位）与 nvcache 的 free-chunk 阈值；DRAM-less 侧
 *   （DFTL / LeaFTL / VPFTL）与 dhara 同样只认**空间压力**。定时 / 写入量 / 碎片率
 *   三类触发在参考层查无出处，且碎片率会自激。 */
#define GC_TRIGGER_WATERMARK (0x01U) /* T1: free_count < FREE_POOL_TARGET */
#define GC_TRIGGER_EMERGENCY (0x08U) /* T4: free_count <= FREE_POOL_EMERGENCY */
/* 磨损均衡不再作为 GC 触发位：静态 WL 由 wear_leveling 的 UBI 判据
 * （max_free_ec − min_used_ec ≥ WL_THRESHOLD）在 host 空闲时独立启动。
 * 旧 T5 用“全表 max/min”比较，min 可能来自从未使用的空闲块 ⇒ 比值永不收敛。 */

/* ── GC 触发判定的求值节拍（由 migrate handle 自持）──
 * 判据本身是**事件**（空间压力/磨损），但求值含一次**全表扫描**，不能按主循环 pass 跑。 */
#define GC_EVAL_INTERVAL_MS (10U)

/* GC 触发日志的节拍（与求值同一套时基）。
 * ⚠ 求值节拍**不能**兼作日志节拍：求值节拍管「多久算一次」，10ms 拍仍会打出 100 行/秒。
 * 日志要的是「同一状态别重复报」。 */
#define GC_LOG_INTERVAL_MS (5000U)

/* GC 会话收尾的静默窗口：连续这么久没有 GC job 活动才认为本次会话结束。
 * 2 s 足以跨过单 victim 的正常间隙，又不会让一次短暂写入把会话拖成永远不结束。 */
#define GC_SESSION_IDLE_MS (2000U)

/* 无收益结论后的 closed-list 复扫间隔（migrate 消费）。 */
#define GC_NOGAIN_BACKOFF_MS (500U)

/* host 写流判定窗口（migrate 消费）：total_writes 变化后这么久内视为 host 在写。 */
#define GC_HOST_WRITE_WINDOW_MS (2000U)

/* ── 类型 ── */

/* 逐 CW META 解码结果。 */
typedef enum
{
    GC_DECODE_OK = 0,
    GC_DECODE_EMPTY,
    GC_DECODE_CORRUPT,
} gc_decode_e;

/* GC 收益判据模式（只对空间 GC T1/T4 有意义；磨损均衡走独立 WL 路径）。
 * 具体阈值是 gc.c 的内部策略参数（IDLE 80% / EMERGENCY 1%）。 */
typedef enum
{
    GC_BENEFIT_OFF = 0,   /* 不筛：host 正在写（原水位行为） */
    GC_BENEFIT_IDLE,      /* host 不写：只接受 valid 占比不高的 victim */
    GC_BENEFIT_EMERGENCY, /* 紧急水位：只接受可回收占比足够的 victim */
} gc_benefit_mode_e;

/* 选块结果：把「没有候选」与「有候选但都不值得搬」分开，后者用于 no-gain 退避与
 * NO_SPACE 判定（`RISK-GC-13/14`）。 */
typedef enum
{
    GC_SELECT_OK = 0,         /* 已选中，out->victim 有效 */
    GC_SELECT_NO_CANDIDATE,   /* closed 列表里没有可用候选（空 / 全 open / 全哨兵） */
    GC_SELECT_NOT_WORTHWHILE, /* 有候选，但全部被收益判据拒绝 */
} gc_select_e;

/* 选块出参（即使 NOT_WORTHWHILE 也填 best_valid_pct，供日志归因）。 */
typedef struct
{
    const super_block_t *victim;         /* GC_SELECT_OK 时有效 */
    WORD                 best_valid_pct; /* 域内最低有效率（0..100），无候选时为 0 */
    WORD                 scanned;        /* 参与评分的候选数 */
    WORD                 eligible;       /* 通过收益判据的候选数 */
} gc_select_out_t;

/* ── 接口函数 ── */

/**
 * @brief 读取一页 NAND，取回逐 CW 的扇区 LBA
 *
 * 内部管理 FC 命令生命周期，无需调用者关心 alloc/submit/release。
 * ⚠ 唯一调用者是**掉电重建**（PFR 的 full metadata rebuild（内部）的逐页反查，开机结构性同步路径）。
 *   搬移读（GC / WL / 慢死预测）走异步路径：migrate.c 用 ftl_async_read_page
 *   建命令、在完成服务函数里调下面的 gc_decode_cw_lba 收尾——不经过本函数。
 *
 * ⚠ 读整页只是为了取回页内 META；**不向调用者拷用户数据**。
 * ⚠ CW 级映射（IU = 1 CW）+ **DRAM-less approach ③**：本函数读**整页**（该 plane 的
 *   4 个 CW），从每个 CW 的**页内 META（= 本仓的 OOB 反向映射）**取回该 CW 自己的扇区
 *   LBA，写入 `cw_lba[CW_NUM]`；未写过的 CW 填 `FTL_LBA_INVALID`。调用者据此逐 CW
 *   **反查 L2P**（仍指向本物理 CW 才算有效）——这就是有效性的唯一真值来源，无常驻位图。
 * ⚠ 全页无写过 CW ⇒ 返回 FTL_ERROR（无有效数据，调用者跳过）。
 *
 * @param pba     物理地址（**页基址**，cw 位为 0）
 * @param cw_lba  输出：逐 CW 的扇区 LBA，CW_NUM 项；未写过 = FTL_LBA_INVALID（不可为 NULL）
 * @return FTL_OK 成功；FTL_AGAIN 命令池空；FTL_ERROR 读失败 / 全页无写过 CW
 */
ftl_status_e gc_read_page_with_lba(DWORD pba, DWORD cw_lba[CW_NUM]);

/**
 * @brief 从**已完成的读命令**的 desc 链解出逐 CW 的扇区 LBA + 页基址
 *
 * gc_read_page_with_lba 的后半段（读之后的解码部分），单独导出供**异步**搬移 job 用：
 * job 的读是 ftl_async_read_page + 服务函数收尾，命令不在 gc_read_page_with_lba 的栈上，
 * 但解码逻辑必须与同步版**逐字相同**（页基址由首个写过的 CW 反推、低位 plane/cw 自洽校验）。
 *
 * ⚠ **必须在 fc_command_release 之前调用**——meta 指针指向命令的 desc 链。
 *
 * @param cmd           已完成的读命令（链恒覆盖该 plane 全 4 CW）
 * @param pba           该命令的目标页 PBA（页基址，cw 位为 0；用于校验 META 的 plane 位）
 * @param cw_lba        输出：逐 CW 的扇区 LBA，CW_NUM 项；未写过 = FTL_LBA_INVALID
 * @param page_base_out 输出：页基址（首个写过 CW 反推）（可为 NULL）
 * @return GC_DECODE_OK = 至少一个 CW 写过且页基址一致
 *         GC_DECODE_EMPTY = 全页未写过
 *         （无有效数据，调用者跳过）；GC_DECODE_CORRUPT = 写过的 CW 页基址不一致
 *         （META 损坏，调用者必须按不可迁移处理，**不得**擦 victim）。
 */
gc_decode_e gc_decode_cw_lba(fc_command_t *cmd, DWORD pba, DWORD cw_lba[CW_NUM], DWORD *page_base_out);

/**
 * @brief 从 closed_sb_list 选择一个 victim Super Block
 *
 * 多维加权评分（设计 §4.3），cost 取小者胜：
 *
 *   cost = 500·valid_ratio + 200·erase_ratio
 *        + 200·cold_penalty + 100·youth_penalty
 *
 *   ① valid_ratio   有效 CW 占比（各 plane valid_count 之和 ÷ CW 容量）——核心项
 *   ② erase_ratio   擦除次数占比（本 SB 各 plane 取大 ÷ 全表最大）——避开临近寿命块
 *   ③ cold_penalty  HOT 块 0 / COLD 块 1000——HOT 数据自然死亡快，优先回收
 *   ④ youth_penalty 1000 − age_ratio，age = 全表最大 last_pe_ts − 本 SB 最小 last_pe_ts
 *                   ——越久没被擦过越优先，保证每个 Block 都有机会被回收
 *
 * ⚠ last_pe_ts **刻意不持久化**：上电后全表为 0 ⇒ 时间项对所有条目
 *   同值（常量偏移，不影响排序）；开机后被 GC 擦过的块才有非零时间戳，于是"本上电
 *   周期内还没被回收过"的块优先——正是公平性语义本身。
 * ⚠ 权重与符号约定的完整说明见 gc.c 的内部策略注释。
 * ⚠ cost 相等时取 valid_count 更小者（确定性，且退化为单维贪心）。
 * ⚠ 退化单 plane 条目（掉电重建产物）跳过哨兵 plane；全哨兵条目直接跳过。
 *
 * **收益判据**（`mode != GC_BENEFIT_OFF` 时）：
 * 在 cost 评分**之前**逐候选过滤——先筛掉不值得搬的块，再在剩下的里选 cost 最小。
 * 不能「先选 cost 最小、再检查收益」：那样可能因为最差块仍太有效而报无收益，
 * 而其实存在另一个更稀疏的候选。
 *
 *   IDLE      ：valid 占比 > 80% ⇒ 跳过
 *   EMERGENCY ：可回收占比 < 1% ⇒ 跳过
 *   （阈值常量定义在 gc.c，属本仓自拟经验值。）
 *
 * total = st.plane_count * FTL_CW_PER_BLOCK（`alc_super_get_stats` 已排除 BAD/哨兵 plane）。
 * **不要**按 next_page 缩分母：提前关闭的半写块有空洞页，按整块容量算才不会
 * 把「还有空洞可写」的块误判成 100% 有效。
 *
 * @param alloc  Allocator 上下文（前置条件；实例绑定由 ftl_contract_audit 证明）
 * @param mode   收益判据模式（GC_BENEFIT_*）
 * @param out    输出：victim 指针 + 观测计数（非 NULL）
 * @return GC_SELECT_OK / GC_SELECT_NO_CANDIDATE / GC_SELECT_NOT_WORTHWHILE
 */
gc_select_e gc_select_victim(const alc_t *alloc, gc_benefit_mode_e mode, gc_select_out_t *out);

/**
 * @brief 检查是否需要触发 GC，并返回**触发起因位掩码**
 *
 * 三条触发（设计 §4.4），可多位同时命中：
 *   T1 GC_TRIGGER_WATERMARK  free_count < FREE_POOL_TARGET          🔴 主动补池
 *   T4 GC_TRIGGER_EMERGENCY  free_count <= FREE_POOL_EMERGENCY      🔴 最后手段
 *   磨损均衡不属于 GC 触发：由 `wear_leveling` 的 UBI 判据（free-max vs used-min，
 *   差值阈值）在 host 空闲时独立启动。
 *
 * ⚠ T1 与 T4 同源（都是"空闲块不够"），但阈值不同：T1 是**主动**补池（还留有余量
 *   时就开工），T4 是**最后手段**（池已见底）。
 * ⚠ GC 触发求值仍需 O(FTL_BLOCK_TOTAL) 的全表极值扫描；求值点由
 *   `mig_gc_evaluate` 的节拍门（GC_EVAL_INTERVAL_MS）兜成本，不在本函数内节流。
 * ⚠ **调用点 = FTL 后台服务里唯一的 GC 求值点** `mig_gc_evaluate`（ftl.c）：
 *   **IDLE 与 NORMAL 两条后台路径都从这里过**（IDLE 据此决定是否启动补池突发，
 *   NORMAL 据此决定是否回收一个 SB）；**BUSY 不求值**（host 饿着，让路），
 *   "无块可写"由 `ftl_issue_subop` 的**写路径按需 GC** 兜底。⚠ 这一条**是本仓自己的**、
 *   不是抄 SPDK：SPDK 的 GC 全在 core poller 里，判据是自由 band 水位；其写路径
 *   `ftl_band_get_next_free`（`ftl_core.c:769`）在 `free_bands` 空时直接返 NULL，
 *   **不就地触发 GC**。
 *
 * @param alloc  Allocator 上下文；NULL ⇒ 返回 0（UT 钉住的契约，不触发）
 * @return 触发起因位掩码（GC_TRIGGER_*），0=不需要触发
 */
DWORD gc_should_trigger(const alc_t *alloc);

#endif /* FTL_GC_H */