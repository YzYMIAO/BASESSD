#ifndef FTL_CORE_H
#define FTL_CORE_H

#include "allocator.h"
#include "bg_policy.h"
#include "checkpoint.h"
#include "ftl_cache.h"
#include "ftl_flush.h"
#include "ftl_internal.h"
#include "ftl_sched.h"
#include "ftl_type.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "read_recovery.h"
#include "stats.h"
#include "wbuf.h"

typedef struct FTL_FLAGS_S
{
    BOOL deferred_error; /* 已回执的 cache 页后来失败，下一命令报 ERROR 一次 */
    BOOL bg_suspend;     /* bbt --erase 后整体跳过 Phase 3（无清除路径，只能复位） */
    BOOL md_load_failed; /* Directory 加载失败：有 CP 时拒绝启动 */
    BOOL l2p_defer;      /* L2P region 未就绪的背压信号：drain 不计 stall */
} ftl_flags_s;

typedef struct FTL_STATISTICS_S
{
    DWORD total_writes;   /* 累计 host 编程次数（功能量：mig 判 host 写流） */
    DWORD dangling_total; /* L2P 指向 valid_count==0 的累计覆盖写次数 */
#if SSD_STATS_ENABLE
    DWORD deferred_taken; /* host 取走 deferred error 的次数（非 FUA 写迟到失败） */
#endif
} ftl_stats_s;

typedef struct FTL_CONTEXT_S
{
    ftl_topo_s   topo;    /* 本 FTL 绑定的 NAND 芯片路由 */
    l2p_cache_s *cache;   /* 共享：按 (ch,ce) 由 l2p_cache_workspace_get() 取 */
    pend_ctx_s   pending; /* pending / group 两张表 */
    wb_handle_s *wbuf;    /* 页打包写缓冲（装配挂载） */

    ftl_table_refs_s table_refs; /* 五张表引用（按值，可审计） */
    alc_s            alloc;      /* allocator（指针绑到 table_refs） */

    l2p_jrn_s      *journal;      /* Journal（装配挂载） */
    md_dir_cache_s *metadata_dir; /* Directory（装配挂载） */
    cp_handle_s    *cp_handle;    /* Checkpoint job（装配挂载） */

    ftl_flags_s flags; /* deferred_error / bg_suspend / md_load_failed / l2p_defer */
    ftl_stats_s stats; /* total_writes / dangling_total */

    ftl_cache_stream_s cache_stream; /* 顺序检测 / 预取 / 观测 / 序列 */

    ftl_flush_job_s flush; /* host SYNCHRONIZE CACHE / CLI fl */
    ftl_trim_job_s  trim;  /* CLI tr */

    bg_policy_s bg;
    ftl_sched_s sched;         /* request scheduler（L2P/host/后台 job + host 写 admission） */
    BYTE        l2p_flush_due; /* timer 置位，bg_policy 消费 */

    mig_handle_s *mig_handle;           /* GC / WL / predict（装配挂载） */
    rr_handle_s   read_recovery_handle; /* 异步恢复 job */
    BYTE         *rr_page_scratch;      /* 恢复整页暂存（装配挂载） */
    BYTE          write_retry_buf[FTL_LBA_PER_PAIR * FTL_IU_BYTES];
} ftl_ctx_s;

/**
 * @brief FTL 实例的 workspace 引用（内部装配结构）
 *
 * 各块由**拥有它的模块**定义并选定内存域（allocator→SRAM12、cp→SRAM3、
 * md→SRAM4、journal/wbuf→DTCM、migrate/rr→AXI）；本结构只收引用（指针）。
 * 由 ftl_instance_init 按 die 装配，ftl_init 只绑定指针；
 * 生命周期覆盖全程（完成表的 ctx 会保存这些地址）。
 */
typedef struct FTL_STORAGE_S
{
    const ftl_table_refs_s *table_refs;
    md_dir_cache_s         *metadata_dir;
    l2p_jrn_s              *journal;
    cp_handle_s            *cp_handle;
    mig_handle_s           *migrate;         /* 装配挂载：指针（不减少总 RAM） */
    wb_handle_s            *wbuf;            /* 装配挂载 */
    BYTE                   *rr_page_scratch; /* 装配挂载：FTL_READ_USER_SIZE */
} ftl_storage_s;

/**
 * @brief 初始化 FTL 子系统
 *
 * 描述符只持有指针；storage 引用的 workspace 由各模块自持，
 * 装配入口 ftl_instance_init 按 die 取引用后传入。
 * L2P cache 是**设备级共享对象**（同一 (ch,ce) 一份），由 l2p_device_init
 * 初始化一次后传入；本函数只绑定指针，不 init、不 set_md_dir。
 *
 * ⚠ storage 指向的数组可能在 NOLOAD 段（`.sram*_data`）：启动代码不清零，
 *   本函数会显式 memset 四个数组。
 *
 * @param ftl     FTL 描述符
 * @param topo    本实例绑定的 NAND 芯片路由
 * @param cache   本实例所属 (ch,ce) 的共享 L2P cache（非空，设备级已初始化）
 * @param storage storage 数组/挂载块的存储（非空，所有指针都非空）
 * @return FTL_OK    成功
 *         FTL_ERROR 元数据候选池构建失败（拒绝启动）
 */
ftl_status_e ftl_init(ftl_ctx_s *ftl, ftl_topo_s topo, l2p_cache_s *cache, const ftl_storage_s *storage);

#endif /* FTL_CORE_H */
