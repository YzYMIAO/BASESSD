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
 * ⚠ ②~⑤ 的执行体**不在本文件**（阶段 4 起）：搬移是一个异步 job，GC 与 WL 共用同一台
 *   引擎，见 `ftl/migrate.h` / `migrate.c`（mig_job_t + mig_begin_gc / mig_begin_wl /
 *   mig_service + ftl_op_migrate）。本文件只留**选 victim 与共用原语**：
 *   · gc_select_victim   —— 四项加权评分（GC 侧选块策略）
 *   · gc_read_page_with_lba / gc_decode_cw_lba —— 搬移读与 META 解码（GC / WL / 掉电重建共用）
 *   · gc_check_triggers  —— 触发判定
 *   原 gc_migrate_sb / gc_erase_and_free 已随阶段 4 迁入 migrate.c 的 job 状态机。
 *
 * 原子性：
 *   Journal 记录每一条 L2P 变更
 *   掉电时最多遗留"孤悬"Block，下次 GC 回收
 */

#include "fc/fc_command.h"    /* fc_command_t（gc_decode_cw_lba 的形参） */
#include "ftl/allocator.h"    /* ftl_allocator_t, super_block_t */
#include "ftl/ftl_config.h"   /* nand_pba_t */
#include "ftl/ftl_type.h"     /* lba_t */
#include "nand/nand_config.h" /* PLANE_NUM, PAGES_PER_BLOCK */

#include <stdint.h>

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

/* ── GC 触发位（P1-2；设计 §4.4 **三条**触发，T2/T3/T6 均已废弃见下）──
 * gc_check_triggers() 返回的**位掩码**：命中的位即触发起因（可多位同时命中）。
 * 只用于调用点打日志/调参，"要不要 GC" 的判定仍是 `!= 0`。 */
#define GC_TRIGGER_WATERMARK (0x01U) /* T1: free_count < FREE_POOL_TARGET */
#define GC_TRIGGER_EMERGENCY (0x08U) /* T4: free_count <= FREE_POOL_EMERGENCY */
#define GC_TRIGGER_WEAR      (0x10U) /* T5: 全表擦除比 max/min > GC_WEAR_LEVEL_RATIO */

/* ⚠ **0x02 / 0x04 已随 T2/T3 退役，与 0x20 一样不再分配**（v0.63）：
 *   T2「写入量整除 GC_INTERVAL_WRITES」与 T3「elapsed_ms 定时」在参考层（`docs/reference/`
 *   全部论文/网页 + `storage_controller_learning.md`）**查无出处**——仓内唯一有出处的 GC
 *   触发是 SPDK 的**自由 band 水位**（`ftl_needs_reloc`：`num_free <= SPDK_FTL_LIMIT_START`，
 *   默认 5 个自由 band）与 nvcache 的 free-chunk 阈值，
 *   DRAM-less 侧（DFTL/LeaFTL/VPFTL）与 dhara 同样只认**空间压力**。二者的求值点也
 *   不可达（只在 `ftl_service_normal`，host IO 在场时是 BUSY、真空闲走 IDLE 的独立补池循环）。
 *   ⚠ 位值**刻意不重排**：`[GC] trig=` 日志与历史 ADR 里的 0x01/0x02/0x04/0x08/0x10 保持可比。 */

/* ⚠ 设计 §4.4 的 T6「碎片率」**刻意不实现**，0x20 位留空不再分配：
 *   原定义"平均有效页 > 70% 触发"方向与标签相反（碎片率高应对应有效页**少**），
 *   且会自激（GC 移走有效页最少的 victim ⇒ 剩余平均值上升 ⇒ 更易命中），
 *   语义又与 T1/T4 重复（closed 块平均 70% 有效 ⇒ 盘近满 ⇒ 空闲池必然低）。
 *   完整论证见 docs/design/ftl_design.md §4.4 表下注。 */

/* ── T5 磨损均衡阈值（设计 §4.7）──
 * ⚠ GC_WEAR_LEVEL_MIN_ERASE 是**地板**：max 没到该值前不触发。没有它时，早期
 *   （max 才擦过几次）就可能满足比例条件而反复空跑 GC。
 * ⚠ 另要求 min > 0：对 0 求比值无意义（"从未被擦过的块"是**静态磨损均衡**
 *   mig_begin_wl 的职责——它专门搬冷数据；T5 只管"所有块都在轮转、但轮得不均"）。 */
#define GC_WEAR_LEVEL_RATIO     (10U) /* 最大/最小擦除比阈值 */
#define GC_WEAR_LEVEL_MIN_ERASE (64U) /* 地板：max 擦除次数低于此值不触发 T5 */

/* ── 接口函数 ── */

/*
 * gc_read_page_with_lba - 读取一页 NAND，返回用户数据 + 逐 CW 的扇区 LBA
 *
 * 内部管理 FC 命令生命周期，无需调用者关心 alloc/submit/release。
 * ⚠ 唯一调用者是**掉电重建**（ftl_rebuild_closed_sb_list 的逐页反查，开机结构性同步路径）。
 *   搬移读（GC / WL / 慢死预测）自阶段 4 起走异步路径：migrate.c 用 ftl_async_read_page
 *   建命令、在完成服务函数里调下面的 gc_decode_cw_lba 收尾——不经过本函数。
 *
 * ⚠ CW 级映射（IU = 1 CW）+ **DRAM-less approach ③**：本函数读**整页**（该 plane 的
 *   4 个 CW），从每个 CW 的**页内 META（= 本仓的 OOB 反向映射）**取回该 CW 自己的扇区
 *   LBA，写入 `cw_lba[CW_NUM]`；未写过的 CW 填 `FTL_LBA_INVALID`。调用者据此逐 CW
 *   **反查 L2P**（仍指向本物理 CW 才算有效）——这就是有效性的唯一真值来源，无常驻位图。
 * ⚠ 同时输出**页基址**（CW0 的扇区 LBA），供调用者构造写回页配置。页基址由**首个写过的
 *   CW** 反推（base = META(c_lo) − c_lo），**不是**直接取 CW0 的 META：CW 级写/搬移允许
 *   页内只编一个 CW 子集（`wr 2 2` 只落 plane0 的 CW2/CW3，GC 搬移也只编有效 CW），
 *   此时 CW0 是空位、META 为 0xFFFFFFFF。
 * ⚠ 全页无写过 CW ⇒ 返回 FTL_ERROR（无有效数据，调用者跳过）。
 *
 * @param pba            物理地址（**页基址**，cw 位为 0）
 * @param buffer         输出：整页用户数据 buffer（FTL_READ_USER_SIZE 字节，可为 NULL）
 * @param cw_lba         输出：逐 CW 的扇区 LBA，CW_NUM 项；未写过 = FTL_LBA_INVALID（不可为 NULL）
 * @param page_base_out  输出：页基址（CW0 的扇区 LBA）（可为 NULL）
 * @return 0 成功, -1 失败（读失败 / 全页无写过 CW）
 */
ftl_status_t gc_read_page_with_lba(nand_pba_t pba, uint8_t *buffer, lba_t cw_lba[CW_NUM], lba_t *page_base_out);

/*
 * gc_decode_cw_lba - 从**已完成的读命令**的 desc 链解出逐 CW 的扇区 LBA + 页基址
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
 * @param page_base_out 输出：页基址（CW0 的扇区 LBA）（可为 NULL）
 * @return 0 成功（至少一个 CW 写过）；-1 全页无写过 CW（无有效数据，调用者跳过本页）
 */
int gc_decode_cw_lba(fc_command_t *cmd, nand_pba_t pba, lba_t cw_lba[CW_NUM], lba_t *page_base_out);

/*
 * gc_select_victim - 从 closed_sb_list 选择一个 victim Super Block
 *
 * 多维加权评分（P1-1），cost 取小者胜：
 *
 *   cost = GC_WEIGHT_VALID·valid_ratio + GC_WEIGHT_ERASE·erase_ratio
 *        + GC_WEIGHT_TEMP·cold_penalty + GC_WEIGHT_AGE·youth_penalty
 *
 *   ① valid_ratio   有效 CW 占比（各 plane valid_count 之和 ÷ CW 容量）——核心项
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
 * gc_check_triggers - 检查是否需要触发 GC，并返回**触发起因位掩码**
 *
 * 三条触发（设计 §4.4；T2/T3/T6 已废弃，见上方位注释），可多位同时命中：
 *   T1 GC_TRIGGER_WATERMARK  free_count < FREE_POOL_TARGET          🔴 主动补池
 *   T4 GC_TRIGGER_EMERGENCY  free_count <= FREE_POOL_EMERGENCY      🔴 最后手段
 *   T5 GC_TRIGGER_WEAR       全表擦除比 max/min > GC_WEAR_LEVEL_RATIO 🟡 均衡
 *
 * ⚠ T1 与 T4 同源（都是"空闲块不够"），但阈值不同：T1 是**主动**补池（还留有余量
 *   时就开工），T4 是**最后手段**（池已见底）。
 * ⚠ T5 每被调用一次就多一次 O(FTL_BLOCK_TOTAL) 全表扫描。这与既有
 *   `wl_should_trigger`（wear_leveling.c，同样每轮服务扫 3 遍全表）同级，
 *   不需要额外节流。
 * ⚠ **调用点 = FTL 后台服务里唯一的 GC 求值点** `ftl_gc_evaluate`（ftl.c）：
 *   **IDLE 与 NORMAL 两条后台路径都从这里过**（IDLE 据此决定是否启动补池突发，
 *   NORMAL 据此决定是否回收一个 SB）；**BUSY 不求值**（host 饿着，让路），
 *   "无块可写"由 `ftl_issue_subop` 的**写路径按需 GC** 兜底。⚠ 这一条**是本仓自己的**、
 *   不是抄 SPDK：SPDK 的 GC 全在 core poller 里，判据是自由 band 水位；其写路径
 *   `ftl_band_get_next_free`（`ftl_core.c:769`）在 `free_bands` 空时直接返 NULL，
 *   **不就地触发 GC**。
 *   参数里已无 total_writes / elapsed_ms —— T2/T3 退役后本函数只看**空间与磨损**。
 *
 * @param alloc  Allocator 上下文
 * @return 触发起因位掩码（GC_TRIGGER_*），0=不需要触发
 */
uint32_t gc_check_triggers(const ftl_allocator_t *alloc);

#endif /* FTL_GC_H */