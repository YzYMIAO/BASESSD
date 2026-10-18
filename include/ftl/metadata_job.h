#ifndef FTL_METADATA_JOB_H
#define FTL_METADATA_JOB_H

/*
 * metadata_job.h — metadata 模块内部接口（metadata.c ↔ metadata_job.c 共享）
 *
 * 不是对外 API：外部模块用 `metadata.h`（Directory/布局）或 `ftl_op.h`
 * （完成服务注册）即可，**不得 include 本头**。
 * 放在 include/ftl/ 是遵守本仓"头文件统一在 include/<模块>/"的约定；文件名按
 * 主要使用者（metadata_job.c）命名，但内容包含 Directory 核心也要用的写指针原语
 * 与待回收位图操作。
 *
 * 为什么这些函数不内联进调用者：
 *   · metadata_dir_slot_now —— append_slot 与压缩 job 跨文件共用；job 不能调
 *     append_slot（`job.phase != IDLE` 会自锁），内联必然复制第二份逻辑。
 *   · md_job_reset / 位图操作 —— 多字段哨兵复位与位图计数必须单点维护，
 *     摊开会让"漏复位一个字段"变成静默故障（§3.1）。
 */

#include "bst.h"      /* ftl_gblock_plane / ftl_gblock_block */
#include "metadata.h" /* metadata_dir_cache_t / md_job_t / PAGES_PER_BLOCK */

#include <stdint.h>
#include <string.h>

/* ── 待回收日志块位图 ── */

/* 置位；已置位返回 false。g 越界是内部调用者的编程错误 ⇒ 运行期拒绝（H31/C5）。 */
static inline bool md_reclaim_mark(metadata_dir_cache_t *dir, uint16_t g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return false;
    }

    uint32_t *word = &dir->reclaim_pending[g >> 5U];
    uint32_t  bit  = 1UL << (g & 31U);

    if ((*word & bit) != 0U)
    {
        return false;
    }

    *word |= bit;
    dir->reclaim_pending_count++;
    return true;
}

/* 取第一个置位的块号；**保持置位**（擦除在飞期间不丢，完成成功才清）。 */
static inline bool md_reclaim_take(const metadata_dir_cache_t *dir, uint16_t *out)
{
    for (uint16_t w = 0U; w < (FTL_BLOCK_TOTAL / 32U); w++)
    {
        const uint32_t word = dir->reclaim_pending[w];

        if (word != 0U)
        {
            uint16_t bit = 0U;

            while ((word & (1UL << bit)) == 0U)
            {
                bit++;
            }
            *out = (uint16_t)((w << 5U) + bit);
            return true;
        }
    }

    return false;
}

/* 清位（置位过才减计数）。 */
static inline void md_reclaim_clear(metadata_dir_cache_t *dir, uint16_t g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return;
    }

    uint32_t *word = &dir->reclaim_pending[g >> 5U];
    uint32_t  bit  = 1UL << (g & 31U);

    if ((*word & bit) != 0U)
    {
        *word &= ~bit;
        if (dir->reclaim_pending_count > 0U)
        {
            dir->reclaim_pending_count--;
        }
    }
}

/* 清空全部待回收登记（RAM 态，不跨重启；init / load / 格式化时调用）。 */
static inline void md_reclaim_reset(metadata_dir_cache_t *dir)
{
    memset(dir->reclaim_pending, 0, sizeof(dir->reclaim_pending));
    dir->reclaim_pending_count = 0U;
}

/*
 * 扫描全池，把"已无引用、非当前写块、确实是 LOG"的块登记进待回收位图。
 *
 * **只登记，不擦除**：换块时调用（与旧同步 sweep 同一时机），擦除全部交给后台 job。
 * 这样既保留了旧 sweep 的覆盖范围（开机快照 / journal 回放 / 压缩中止留下的零活块），
 * 又不会在服务循环里阻塞 ~3 ms 的 tERASE。扫描本身 O(FTL_BLOCK_TOTAL)，无 NAND I/O。
 */
static inline void md_reclaim_mark_all(metadata_dir_cache_t *dir, const ftl_allocator_t *alloc)
{
    for (uint16_t g = 0U; g < FTL_BLOCK_TOTAL; g++)
    {
        if (dir->l2p_live[g] != 0U)
        {
            continue;
        }
        if ((dir->cur_block_valid != 0U) && (g == dir->cur_metadata_block))
        {
            continue; /* 当前写入块不可回收 */
        }
        if (ftl_block_state(&alloc->blocks[g]) != FTL_BLOCK_STATE_LOG)
        {
            continue; /* 只认本池独占的日志块，别碰恰好活计数为 0 的用户块 */
        }
        (void)md_reclaim_mark(dir, g);
    }
}

/* ── 写指针 ── */

/*
 * 取当前日志块的**下一个空槽**并推进写指针（纯 RAM，不做任何块管理）。
 *
 * 返回：槽位 PBA；无当前块 / 块已满 ⇒ NAND_PBA_INVALID。
 * 取到位置后**立即推进** cur_metadata_page：写失败时该页可能已半编程、不可复用。
 */
static inline nand_pba_t metadata_dir_slot_now(metadata_dir_cache_t *dir)
{
    if ((dir->cur_block_valid == 0U) || (dir->cur_metadata_page >= PAGES_PER_BLOCK))
    {
        return NAND_PBA_INVALID;
    }

    uint16_t plane = ftl_gblock_plane(dir->cur_metadata_block);
    uint16_t block = ftl_gblock_block(dir->cur_metadata_block);
    uint8_t  page  = dir->cur_metadata_page;

    dir->cur_metadata_page++;

    return pba_make(dir->topo.ch, dir->topo.ce, dir->topo.lun, block, plane, page, 0U);
}

/* ── job 状态 ── */

/*
 * 把整理 job 清回 IDLE。
 *
 * 哨兵值必须显式写（memset 只给 0）：victim / reclaim_gblock 的"无"是 UINT16_MAX，
 * slot 的"未取"是 NAND_PBA_INVALID。漏一个 ⇒ append_slot 永久让路（phase 卡死）
 * 或下次压缩复用已消耗的槽（二次编程 = 静默损坏）。
 *
 * ⚠ 不清 `reclaim_pending`：待回收知识不属于 job 状态，job 中止后仍要重试。
 */
static inline void md_job_reset(metadata_dir_cache_t *dir)
{
    memset(&dir->job, 0, sizeof(dir->job));
    dir->job.victim         = UINT16_MAX;
    dir->job.reclaim_gblock = UINT16_MAX;
    dir->job.slot           = NAND_PBA_INVALID;
}

/* md_job_idle — 整理 job 是否空闲（无回收/压缩在跑）。 */
static inline bool md_job_idle(const metadata_dir_cache_t *dir)
{
    return dir->job.phase == (uint8_t)MD_JOB_IDLE;
}

/*
 * md_compacting — 压缩 job 是否处于 CP_* 阶段。
 *
 * `commit_region` 据此**抑制逐次立即回收**：搬移 victim 的最后一个 region 时其活
 * 计数归零，但 Anchor 还没指向新位置 ⇒ 此刻擦 victim 会在掉电后丢映射（③④ 契约）。
 * 压缩期间归零的块由 job 自己在 CP_RECLAIM 里擦。
 */
static inline bool md_compacting(const metadata_dir_cache_t *dir)
{
    return (dir->job.phase >= (uint8_t)MD_JOB_CP_READ) && (dir->job.phase <= (uint8_t)MD_JOB_CP_RECLAIM);
}

#endif /* FTL_METADATA_JOB_H */
