#ifndef FTL_METADATA_JOB_H
#define FTL_METADATA_JOB_H

/*
 * metadata_job.h — metadata 模块内部接口（metadata.c ↔ metadata_job.c 共享）
 *
 * 不是对外 API：外部模块用 `metadata.h`（Directory/布局）或 `ftl_op.h`
 * （完成服务注册）即可，**不得 include 本头**。
 *
 * 本头只放**声明**与**纯谓词**：
 *   · 改状态的闸门（位图置/清、写指针、job 复位）定义在 metadata_job.c，
 *     保证单一实现（H42）；
 *   · `md_handle_is_idle` / `md_handle_is_compacting` / `md_log_block_is_idle` 是纯读谓词，
 *     跨 TU 内联。
 */

#include "metadata.h" /* md_dir_cache_t / md_handle_t / PAGES_PER_BLOCK / alc_t */

/* ── 待回收日志块位图 ── */

/** @brief 置位；已置位返回 false。g 越界是内部调用者的编程错误 ⇒ 运行期拒绝（H31/C5）。 */
bool md_reclaim_mark(md_dir_cache_t *dir, WORD g);

/** @brief 清空全部待回收登记（RAM 态，不跨重启；init / load / 格式化时调用）。 */
void md_reclaim_reset(md_dir_cache_t *dir);

/**
 * @brief 扫描全池，把"已无引用、非当前写块、确实是 LOG"的块登记进待回收位图。
 * **只登记，不擦除**：换块时调用，擦除全部交给后台 job。
 */
void md_reclaim_mark_all(md_dir_cache_t *dir, const alc_t *alloc);

/* ── 写指针 ── */

/**
 * @brief 取当前日志块的**下一个空槽**并推进写指针（纯 RAM，不做任何块管理）。
 *
 * @return 槽位 PBA；无当前块 / 块已满 ⇒ NAND_PBA_INVALID。取到位置后**立即推进** cur_metadata_page：写失败时该页可能已半编程、不可复用。
 */
DWORD md_slot_take(md_dir_cache_t *dir);

/* ── job 状态 ── */

/**
 * @brief 把整理 job 清回 IDLE。
 * 哨兵值必须显式写（memset 只给 0）：victim / reclaim_gblock / reclaim_fail_gblock
 * 的"无"是 UINT16_MAX，slot 的"未取"是 NAND_PBA_INVALID。漏一个 ⇒ append_slot 永久让路（phase 卡死）
 * 或下次压缩复用已消耗的槽（二次编程 = 静默损坏）。
 * ⚠ 不清 `reclaim_pending` / `reclaim_scan`：待回收知识不属于 job 状态。
 */
void md_handle_reset(md_dir_cache_t *dir);

/** @brief 整理 job 是否空闲（无回收/压缩在跑）。 */
static inline bool md_handle_is_idle(const md_dir_cache_t *dir)
{
    return dir->handle.phase == MD_JOB_IDLE;
}

/**
 * @brief md_handle_is_compacting — 压缩 job 是否处于 CP_* 阶段。
 *
 * `commit_region` 据此**抑制逐次立即回收**：搬移 victim 的最后一个 region 时其活
 * 计数归零，但 Anchor 还没指向新位置 ⇒ 此刻擦 victim 会在掉电后丢映射（③④ 契约）。
 * 压缩期间归零的块由 job 自己在 CP_RECLAIM 里擦。
 * /
 */
static inline bool md_handle_is_compacting(const md_dir_cache_t *dir)
{
    return (dir->handle.phase >= MD_JOB_CP_READ) && (dir->handle.phase <= MD_JOB_CP_RECLAIM);
}

/**
 * @brief md_log_block_is_idle — 该块是否"已无可回收的顾虑"：在范围内、活 region 为 0、
 * 且不是当前写块。**纯账本判据**，不含 BST 状态。
 * /
 */
static inline bool md_log_block_is_idle(const md_dir_cache_t *dir, WORD g)
{
    if (g >= FTL_BLOCK_TOTAL)
    {
        return false;
    }
    if (dir->l2p_live[g] != 0U)
    {
        return false;
    }
    if (dir->cur_block_valid && (g == dir->cur_metadata_block))
    {
        return false;
    }

    return true;
}

#endif /* FTL_METADATA_JOB_H */
