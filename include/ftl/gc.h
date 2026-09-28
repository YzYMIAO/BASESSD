#ifndef FTL_GC_H
#define FTL_GC_H

/*
 * Garbage Collection
 *
 * 以 Super Block 为粒度回收已写满的 Block。
 *
 * 流程：
 *   ① 从 closed_sb_list 选 victim（贪心：选有效页最少的）
 *   ② 逐 Plane 逐页读 victim Super Block
 *   ③ 读 META 获取 LBA → 查 L2P → 判断页是否有效
 *   ④ 有效页 → 搬移到新的 Cold Super Block
 *   ⑤ 擦除双 Plane Block → 归还到 Free Pool
 *
 * 原子性：
 *   Journal 记录每一条 L2P 变更
 *   掉电时最多遗留"孤悬"Block，下次 GC 回收
 */

#include "nand/nand_config.h"   /* nand_pba_t, PLANE_NUM, PAGES_PER_BLOCK */
#include "ftl/l2p_cache.h"     /* l2p_cache_t */
#include "ftl/journal.h"       /* l2p_journal_t */
#include "ftl/allocator.h"     /* ftl_allocator_t, super_block_t */

#include <stdint.h>

/* ── GC 参数 ── */

#define PRE_COND_LOW_WATER      (32U)       /* Pre-conditioned 低水位 */
#define GC_INTERVAL_WRITES      (1024U)     /* 写入量触发间隔 */
#define GC_TIMER_INTERVAL_MS    (500U)      /* 定时触发间隔（ms）*/

/* ── 接口函数 ── */

/*
 * gc_select_victim - 从 closed_sb_list 选择一个 victim Super Block
 *
 * 贪心策略：有效页最少的 Super Block（无效页最多 → 回收效率最高）。
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
int gc_migrate_sb(ftl_allocator_t *alloc, l2p_cache_t *cache,
                   l2p_journal_t *journal, super_block_t *victim);

/*
 * gc_erase_and_free - 擦除 victim Block 并归还到 Free Pool
 *
 * @param alloc  Allocator 上下文
 * @param victim 已搬完的 victim Super Block
 */
void gc_erase_and_free(ftl_allocator_t *alloc, super_block_t *victim);

/*
 * gc_check_trigger - 检查是否需要触发 GC
 *
 * @param alloc           Allocator 上下文
 * @param total_writes    写入总次数
 * @param elapsed_ms      距上次 GC 的毫秒数
 * @return 1=需要触发 GC，0=不需要
 */
int gc_check_trigger(const ftl_allocator_t *alloc, uint32_t total_writes, uint32_t elapsed_ms);

#endif /* FTL_GC_H */