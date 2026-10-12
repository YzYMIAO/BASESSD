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
 *   ⑤ 擦除双 Plane Block → 归还到 Free Pool
 *
 * 原子性：
 *   Journal 记录每一条 L2P 变更
 *   掉电时最多遗留"孤悬"Block，下次 GC 回收
 */

#include "ftl/allocator.h"    /* ftl_allocator_t, super_block_t */
#include "ftl/ftl_type.h"     /* lba_t */
#include "ftl/journal.h"      /* l2p_journal_t */
#include "ftl/l2p_cache.h"    /* l2p_cache_t */
#include "nand/nand_config.h" /* nand_pba_t, PLANE_NUM, PAGES_PER_BLOCK */

#include <stdint.h>

/* ── GC 参数 ── */

#define GC_INTERVAL_WRITES   (1024U) /* 写入量触发间隔 */
#define GC_TIMER_INTERVAL_MS (500U)  /* 定时触发间隔（ms）*/

/* ── GC victim 多维加权评分（P1-1；设计 §4.3）──
 * 权重为**千分比整数**：本仓无 FPU 便利、newlib-nano 无 %f ⇒ 全整数，四项各自
 * 归一化到 0..1000 后再加权，cost 上限 (500+200+200+100)×1000 = 1,000,000。
 * **cost 取小 = 越优先回收**（保持旧契约「选最省事的块」的语义方向）。
 *
 * ⚠ 与设计文档 §4.3 逐字公式的三处偏离（文档登记）：
 *   ① 文档公式符号约定自相矛盾（0.50·valid_ratio 若按"取大"读，会奖励有效页**多**的块，
 *      与同节"选有效页最少"相反）⇒ 本仓钉死 **cost 取小**，有效页项逐字沿用；
 *   ② 擦除项按 §4.3 的理由（"避开临近寿命的 Block"）取 **+erase_ratio**，
 *      而非文档字面的 (1-erase_ratio)（后者在取小约定下会**偏向**磨损重的块）；
 *   ③ 温度项文档未定义 temp_factor，本仓定义为 **HOT 优先当 victim**
 *      （§4.3：COLD 数据有效页几乎不变，下次 GC 还得搬）。 */
#define GC_WEIGHT_VALID (500U) /* 有效页占比（核心指标，直接决定写放大） */
#define GC_WEIGHT_ERASE (200U) /* 擦除次数占比（磨损均衡，避开临近寿命块） */
#define GC_WEIGHT_TEMP  (200U) /* 温度（HOT 数据自然死亡快，投资未来无效收益） */
#define GC_WEIGHT_AGE   (100U) /* 时间（保证每个 Block 都有机会被回收） */

/* ── GC 触发位（P1-2；设计 §4.4 五条触发，T6 已废弃见下）──
 * gc_check_triggers() 返回的**位掩码**：命中的位即触发起因（可多位同时命中）。
 * 只用于调用点打日志/调参，"要不要 GC" 的判定仍是 `!= 0`。 */
#define GC_TRIGGER_WATERMARK (0x01U) /* T1: free_count < FREE_POOL_TARGET */
#define GC_TRIGGER_WRITES    (0x02U) /* T2: total_writes % GC_INTERVAL_WRITES == 0 */
#define GC_TRIGGER_TIMER     (0x04U) /* T3: elapsed_ms >= GC_TIMER_INTERVAL_MS */
#define GC_TRIGGER_EMERGENCY (0x08U) /* T4: free_count <= FREE_POOL_EMERGENCY */
#define GC_TRIGGER_WEAR      (0x10U) /* T5: 全表擦除比 max/min > GC_WEAR_LEVEL_RATIO */

/* ⚠ 设计 §4.4 的 T6「碎片率」**刻意不实现**，0x20 位留空不再分配：
 *   原定义"平均有效页 > 70% 触发"方向与标签相反（碎片率高应对应有效页**少**），
 *   且会自激（GC 移走有效页最少的 victim ⇒ 剩余平均值上升 ⇒ 更易命中），
 *   语义又与 T1/T4 重复（closed 块平均 70% 有效 ⇒ 盘近满 ⇒ 空闲池必然低）。
 *   完整论证见 docs/design/ftl_design.md §4.4 表下注。 */

/* ── T5 磨损均衡阈值（设计 §4.7）──
 * ⚠ GC_WEAR_LEVEL_MIN_ERASE 是**地板**：max 没到该值前不触发。没有它时，早期
 *   （max 才擦过几次）就可能满足比例条件而反复空跑 GC。
 * ⚠ 另要求 min > 0：对 0 求比值无意义（"从未被擦过的块"是**静态磨损均衡**
 *   wl_execute 的职责——它专门搬冷数据；T5 只管"所有块都在轮转、但轮得不均"）。 */
#define GC_WEAR_LEVEL_RATIO     (10U) /* 最大/最小擦除比阈值 */
#define GC_WEAR_LEVEL_MIN_ERASE (64U) /* 地板：max 擦除次数低于此值不触发 T5 */

/* ── 接口函数 ── */

/*
 * gc_read_page_with_lba - 读取一页 NAND，返回用户数据 + 从 META 提取的 LBA
 *
 * 内部管理 FC 命令生命周期，无需调用者关心 alloc/submit/release。
 * 可被 GC、WL 等需要读 NAND + 取 LBA 的模块复用。
 *
 * B4：同步返回本次读的 4 CW 最大 ECC 纠正 bit 数与"读成功"标志——
 * GC/WL 搬移读是 read disturb 盲区（旧实现不记录 ECC/read_count），
 * 调用方据此 rr_record_ecc + read_count++（慢死预测闭环）。
 *
 * @param pba            物理地址
 * @param buffer         输出：用户数据 buffer（2048 字节，可为 NULL）
 * @param lba_out        输出：从 META 提取的 LBA（可为 NULL）
 * @param ecc_out        输出：本次读 4 CW 最大 ECC 纠正 bit 数（0..4，可为 NULL）
 * @param read_count_out 输出：1=本次读成功（调用方 ++ 该块 read_count，可为 NULL）
 * @return 0 成功, -1 失败
 */
ftl_status_t gc_read_page_with_lba(nand_pba_t pba, uint8_t *buffer, lba_t *lba_out, uint8_t *ecc_out, uint8_t *read_count_out);

/*
 * gc_select_victim - 从 closed_sb_list 选择一个 victim Super Block
 *
 * 多维加权评分（P1-1），cost 取小者胜：
 *
 *   cost = GC_WEIGHT_VALID·valid_ratio + GC_WEIGHT_ERASE·erase_ratio
 *        + GC_WEIGHT_TEMP·cold_penalty + GC_WEIGHT_AGE·youth_penalty
 *
 *   ① valid_ratio   有效页占比（各 plane VPC 之和 ÷ 容量）——核心项
 *   ② erase_ratio   擦除次数占比（本 SB 各 plane 取大 ÷ 全表最大）——避开临近寿命块
 *   ③ cold_penalty  HOT 块 0 / COLD 块 1000——HOT 数据自然死亡快，优先回收
 *   ④ youth_penalty 1000 − age_ratio，age = 全表最大 last_pe_ts − 本 SB 最小 last_pe_ts
 *                   ——越久没被擦过越优先，保证每个 Block 都有机会被回收
 *
 * ⚠ last_pe_ts **不持久化**（P0-1 刻意未落盘）：上电后全表为 0 ⇒ 时间项对所有条目
 *   同值（常量偏移，不影响排序）；开机后被 GC 擦过的块才有非零时间戳，于是"本上电
 *   周期内还没被回收过"的块优先——正是公平性语义本身。
 * ⚠ 权重与符号约定的完整说明见 gc.h 的 GC_WEIGHT_* 块注释。
 * ⚠ cost 相等时取 valid_count 更小者（确定性，且退化为旧的单维贪心）。
 * ⚠ 退化单 plane 条目（掉电重建产物）跳过哨兵 plane；全哨兵条目直接跳过。
 *
 * @param alloc  Allocator 上下文
 * @param victim 输出：选中的 victim 指针
 * @return 1=找到 victim，0=无可用 victim
 */
int gc_select_victim(ftl_allocator_t *alloc, super_block_t **victim);

/*
 * gc_migrate_sb - 搬移一个 victim Super Block 的所有有效页
 *
 * 内部流程：
 *   ① 遍历双 Plane × 64 页
 *   ② 对每页：读 NAND → 从 META 取 LBA → 查 L2P
 *   ③ L2P 指向本 PBA → 有效，搬移到 Cold Super Block
 *   ④ 否则 → 跳过（无效页）
 *   ⑤ 搬完 → 擦除双 Block → 归还 Free Pool
 *
 * @param alloc   Allocator 上下文
 * @param cache   L2P Cache
 * @param journal Journal（记录搬移导致的 L2P 变更）
 * @param victim  要搬移的 victim Super Block
 * @return 搬移的页数
 */
int gc_migrate_sb(ftl_allocator_t *alloc, l2p_cache_t *cache, l2p_journal_t *journal, super_block_t *victim);

/*
 * gc_erase_and_free - 擦除 victim Block 并归还到 Free Pool
 *
 * 擦除成功后从 Closed 列表移除 victim（swap-remove，修 list 只增不减），
 * 记录 last_pe_ts = now（单调写入量时间戳）。
 *
 * @param alloc   Allocator 上下文
 * @param victim  已搬完的 victim Super Block
 * @param now     当前单调时间戳（ftl->total_writes）
 */
void gc_erase_and_free(ftl_allocator_t *alloc, super_block_t *victim, uint32_t now);

/*
 * gc_check_triggers - 检查是否需要触发 GC，并返回**触发起因位掩码**
 *
 * 五条触发（设计 §4.4；T6 已废弃），可多位同时命中：
 *   T1 GC_TRIGGER_WATERMARK  free_count < FREE_POOL_TARGET          🔴 紧急
 *   T2 GC_TRIGGER_WRITES     total_writes % GC_INTERVAL_WRITES == 0 🟡 主动
 *   T3 GC_TRIGGER_TIMER      elapsed_ms >= GC_TIMER_INTERVAL_MS     🟢 后台
 *   T4 GC_TRIGGER_EMERGENCY  free_count <= FREE_POOL_EMERGENCY      🔴 紧急
 *   T5 GC_TRIGGER_WEAR       全表擦除比 max/min > GC_WEAR_LEVEL_RATIO 🟡 均衡
 *
 * ⚠ T1 与 T4 同源（都是"空闲块不够"），但阈值不同：T1 是**主动**补池（还留有余量
 *   时就开工），T4 是**最后手段**（池已见底）。T4 一旦命中，调用点应立即同步回收
 *   而不只是"排一次 GC"。
 * ⚠ T5 每被调用一次就多一次 O(FTL_BLOCK_TOTAL) 全表扫描。这与既有
 *   `wl_should_trigger`（wear_leveling.c，同样每轮 NORMAL 服务扫 3 遍全表）同级，
 *   不需要额外节流。
 * ⚠ 调用点：`ftl_service_normal`（ftl.c）。IDLE 模式走自己的补池循环，不调本函数
 *   ⇒ T5 只在 host IO 在场时评估（空闲期的磨损均衡由静态 WL `wl_execute` 负责）。
 *
 * @param alloc           Allocator 上下文
 * @param total_writes    写入总次数
 * @param elapsed_ms      距上次 GC 的毫秒数
 * @return 触发起因位掩码（GC_TRIGGER_*），0=不需要触发
 */
uint32_t gc_check_triggers(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms);

/*
 * gc_check_trigger - 检查是否需要触发 GC
 *
 * `gc_check_triggers(...) != 0` 的薄封装，保留旧契约给不关心成因的调用者。
 *
 * @param alloc           Allocator 上下文
 * @param total_writes    写入总次数
 * @param elapsed_ms      距上次 GC 的毫秒数
 * @return 1=需要触发 GC，0=不需要
 */
int gc_check_trigger(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms);

#endif /* FTL_GC_H */