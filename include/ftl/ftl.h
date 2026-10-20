#ifndef FTL_SERVICE_H
#define FTL_SERVICE_H

/*
 * ftl_service — 主循环调度器
 *
 * 每轮 superloop 两段（**泵与实例分开**）：
 *   ① ftl_service_pump()   —— **实例无关**：Phase 1 推进 NAND + Phase 2 抽干完成表。
 *                              全局的 NAND 引擎与完成表，每轮**只调一次**。
 *   ② ftl_service(ftl)     —— **单实例**：Phase 2.5 补发子操作 + Phase 3 三级后台。
 *                              对**每一个**实例都要调（多 LUN 下不能只跑 [0][0][0]）。
 *
 * 三级模式切换依据未决命令数（pending + 组），非"本轮提交了几条 NAND"。
 */

#include "allocator.h"    /* ftl_table_refs_t, super_block_t */
#include "bg_scheduler.h" /* 时间预算后台调度器（ftl_context_t 含调度器） */
#include "checkpoint.h"
#include "ftl_type.h" /* ftl_io_request_t */
#include "gc.h"
#include "journal.h"
#include "l2p_cache.h"
#include "metadata.h"
#include "migrate.h"
#include "pending.h"
#include "read_recovery.h"
#include "ssd_types.h"
#include "wbuf.h"

#include <stdbool.h>
#include <stdint.h>

/* ── 宏 ── */

#define NAND_SUBMIT_BUDGET  (4U)     /* 每轮 fc_service 最多跑 4 次（首轮抽干完成，余为填充 pass） */
#define CP_WRITE_INTERVAL   (1024U)  /* 写入量触发间隔 */
#define CP_TIME_INTERVAL_MS (10000U) /* 定时触发间隔（ms）*/

#define FTL_READ_USER_SIZE (2048U) /* 一次读返回的用户数据大小 = 4 CW × 512B */

/* 一个组的子操作连续遇 FTL_AGAIN（瞬态背压）而**无任何进展**的轮数上限。
 * 超过即判真卡死：弃未下发子操作、组报 IO_ERROR（已下发的照常完成）。
 * 预算取 8——正常背压在下轮 ftl_service 抽干完成后即恢复，连续 8 轮无进展属异常。 */
#define FTL_DRAIN_STALL_MAX (8U)

/*
 * 主机可见的最大 LBA = 实际可用于用户数据的块 × 每块 CW 数。
 *
 * ⚠ `FTL_IU_TOTAL`（= 全部物理 CW）是**映射表寻址空间**，不是主机容量：
 *   它把固定元数据、OP 预留、Spare、L2P 日志区全算成了可写空间。直接拿它当容量
 *   会超卖，写满后 Free/OP 池耗尽、GC 没有搬移目标，前台写入失败。
 *
 * 每 plane 的预算：
 *   FTL_METADATA_AREA_END（78 = 固定元数据 46 + **L2P 日志区 32**）
 * + FTL_OP_BLOCKS_PER_PLANE（204，10% GC headroom）
 * + FTL_SPARE_POOL_PER_PLANE（50，坏块替换）
 * = 332 blocks/plane；数据块 = 2048 − 332 = 1716/plane，全设备 3432。
 * 当前几何：3432 × 64 × 4 = 878,592 LBA = 429 MiB（不是 512/437 MiB）。
 *
 * ⚠ L2P 日志区必须计入：它是**固定预留**的元数据区（4 MiB 表本体 × 2 余量
 *   = 8 MiB = 64 gblocks）。旧版把它从 Free Pool 动态分配且不扣容量，实测日志
 *   碎片到 1076 块（134 MiB），导致容量未满就写失败（RISK-MD-07 / RISK-CAP-02）。
 */
#define FTL_HOST_DATA_BLOCKS_PER_INSTANCE                                  \
    (FTL_BLOCK_TOTAL - ((FTL_METADATA_AREA_END + FTL_OP_BLOCKS_PER_PLANE + \
                         FTL_SPARE_POOL_PER_PLANE) *                       \
                        PLANE_NUM))

/* 全局主机 LBA 上限 = 每实例数据块 × 每块 CW × 设备维度数
 * （每 (ch,ce,lun) 是独立 FTL 实例；DIM=1 时 3432×256 = 878,592 = 429 MiB）。 */
#define FTL_LBA_MAX \
    ((QWORD)FTL_HOST_DATA_BLOCKS_PER_INSTANCE * PAGES_PER_BLOCK * CW_NUM * FTL_DIM_TOTAL)

/* 主机可见容量（字节）：给日志/文档用，和 USB MSC 的 READ CAPACITY 同源。 */
#define FTL_HOST_CAPACITY_BYTES ((QWORD)FTL_LBA_MAX * FTL_IU_BYTES)

/* 主机范围必须落在共享 cache 的键空间内（C9：同一语义只允许一套上限）。 */
_Static_assert((QWORD)FTL_LBA_MAX <= L2P_GLOBAL_LBA_MAX,
               "FTL_LBA_MAX 超出共享 cache 的全局 LBA 上限");

/* ── FTL 主上下文 ── */

typedef struct ftl_context
{
    /* 拓扑信息（本 FTL 绑定的 NAND 芯片路由） */
    ftl_topology_t topo; /* 本 FTL 绑定的 NAND 芯片路由 */

    /* L2P Cache（共享：指向 g_l2p_cache[ch][ce]，同一 (ch,ce) 下的所有 LUN 共用） */
    l2p_cache_t *cache;

    /* Pending Request 表 + 完成组表（per-instance）
     * 由 pending.c 操作：每个子操作一个槽（command_id → 组 + 子操作字段），
     * 一条 host 请求一个组（聚合其全部子操作的完成通知）。
     * 提取为 ftl_pending_table_t，让 RR / wbuf 只持有该表的指针，而不是整个 FTL 上下文。 */
    ftl_pending_table_t pending;

    /* 页打包写缓冲——同一 pair 窗口的多次写攒成一页再编程，
     * 见 wbuf.h 的 wbuf_handle_t。事件走 wb_handle()，读路径走 wb_has/read。
     * 指针，由 storage 提供并决定落哪个域（Phase 1.5）。 */
    wbuf_handle_t *wbuf;

    /* 五张表的引用（表本体由 HIL 独立定义、各自选内存域）。
     * 按值保存：多实例下把“本实例绑定的是哪组表”留成可审计的事实。
     * 下游访问走 `ftl->alloc->...`（同一组指针），不经过本字段。 */
    ftl_table_refs_t table_refs;

    /* Allocator（各指针绑到上面的 table_refs，见 alc_init） */
    ftl_allocator_t alloc;

    /* Journal（指针，由 storage 提供；落 SRAM3） */
    l2p_journal_t *journal;

    /* Metadata Directory（指针，由 storage 提供；落 SRAM4） */
    metadata_dir_cache_t *metadata_dir;

    /* Checkpoint 异步 job（含池游标/序号、定时基准与端口；见 checkpoint.h）。
     * ⚠ 含 256B 的 region 脏位图**快照**——必须在 job 起点取定，不能沿用实时位图
     *   （flush 成功会清实时位，见 checkpoint.c 的"快照点前移"）。
     * 指针，由 storage 提供；落 SRAM3。 */
    checkpoint_handle_t *cp_handle;

    /* 统计 */
    DWORD total_writes;

    /* 后台写闸门（`bbt --erase` 置位）。
     * 全片擦除后介质已空，而本实例 RAM 里的 BST/L2P/Journal 仍描述着**已不存在**的
     * 布局 ⇒ 唯一正确的后续动作是复位。置位后 ftl_bg_service 整体跳过（含 must-run
     * 集：checkpoint / journal / 元数据目录 / bbt / GC 全在那里），杜绝把旧 RAM 状态
     * 写回擦净的芯片；Phase 1/2/2.5 照常（NAND 泵与 host IO 不受影响）。
     * ⚠ 只由诊断命令置位，且**无清除路径**——清它只能靠复位。 */
    bool bg_suspend;

    /* Directory 加载失败（Anchor 池无有效 record / 读失败）。
     * ftl_init 仍建空 Directory（格式化/首次启动路径需要它），但**不能静默继续**：
     * 空 Directory 会让旧 region 映射整体丢失（RISK-BOOT-09）。ftl_boot_recovery 在
     * 介质存在有效 Checkpoint 时据此拒绝启动；无 Checkpoint（空片）才放行。 */
    bool md_load_failed;

    /* L2P 加载背压信号（per-instance）。
     * ftl_issue_subop 因 **L2P region 未就绪**（异步加载在飞，l2p_read 返
     * FTL_AGAIN）而返回 FTL_AGAIN 时置 1；ftl_drain_group 据此**不计** `stalls`——
     * 加载在飞 = 有进展，不是"下发不动"。否则一次正常的 region 加载会吃掉 8 轮预算
     * （FTL_DRAIN_STALL_MAX）里的一轮，随机读满盘时把组误判成卡死报 IO_ERROR。
     * 真卡死（命令丢失）由 NAND 层 wait/jump 超时兜底，不靠这个计数。 */
    bool l2p_defer;

    /* 时间预算后台调度器（per-instance）。
     * 任务表（10 项：6 must-run + 4 预算）由 ftl_bg_init 从 `ftl/bg_wiring.c` 的静态表
     * 装载；每轮 Phase 3 由 ftl_bg_service 选档后跑 bg_scheduler_run。 */
    bg_scheduler_t bg;

    /* 搬移 job（GC / 静态 WL / 慢死预测共用一台异步引擎，见 migrate.h）。
     * 观测计数（reclaimed / novictim）与求值节拍已收进 handle（migrate_status_t）。
     * 指针，由 storage 提供；含逐 plane 页镜像（page_buf，约 4KB），必须常驻。 */
    migrate_handle_t *mig_handle;

    /* Read Recovery：per-Block ECC 趋势追踪在 alloc.blocks[].rr（BST 单数据源）；
     * 本结构 = 异步恢复 job 状态机本体（见 read_recovery.h） */
    read_recovery_handle_t read_recovery_handle;

    /* Read Recovery 整页暂存（CW 级映射：一个子操作可能只覆盖页内部分 CW，
     * 而恢复读内部是**整页读**、从 CW0 起连续拷出。故恢复结果先落本页
     * 镜像，再按各 CW 的真实偏移搬回 caller 缓冲（见 rr_settle_plane）。
     * 指针，由 storage 提供；必须是常驻存储（不能是栈）。 */
    BYTE *rr_page_scratch;
} ftl_context_t;

/*
 * ftl_storage_t - 描述符的外部存储
 *
 * storage 数组由 HIL 提供并决定落哪个域（main.c：五张表与 md→各自 section、
 * journal/cp_handle→SRAM3；migrate/wbuf/rr_page_scratch→AXI .bss）。
 * 五张表各自独立定义，本结构只收它们的引用（指针），不约束表本体的放置。
 * ftl_init 只绑定指针；数组定义在 main.c，
 * 生命周期覆盖全程（完成表的 ctx 会保存这些地址）。
 */
typedef struct
{
    const ftl_table_refs_t *table_refs;
    metadata_dir_cache_t   *metadata_dir;
    l2p_journal_t          *journal;
    checkpoint_handle_t    *cp_handle;
    migrate_handle_t       *migrate;         /* Phase 1.5：挂载指针（不减少总 RAM） */
    wbuf_handle_t          *wbuf;            /* Phase 1.5 */
    BYTE                   *rr_page_scratch; /* Phase 1.5：FTL_READ_USER_SIZE */
} ftl_storage_t;

/* ── 接口 ── */

/*
 * ftl_init - 初始化 FTL 子系统
 *
 * 描述符只持有指针；storage 数组/挂载块由调用者提供。
 * L2P cache 是**设备级共享对象**（同一 (ch,ce) 一份），由调用者初始化一次后
 * 传入；本函数只绑定指针，不 init、不 set_md_dir。
 *
 * ⚠ storage 指向的数组可能在 NOLOAD 段（`.sram*_data`）：启动代码不清零，
 *   本函数会显式 memset 四个数组。
 *
 * @param ftl     FTL 描述符
 * @param topo    本实例绑定的 NAND 芯片路由
 * @param cache   本实例所属 (ch,ce) 的共享 L2P cache（非空，设备级已初始化）
 * @param storage storage 数组/挂载块的存储（非空，所有指针都非空）
 * @return FTL_OK 成功
 */
ftl_status_t ftl_init(ftl_context_t *ftl, ftl_topology_t topo, l2p_cache_t *cache, const ftl_storage_t *storage);

/*
 * ftl_contract_audit - debug-only 初始化契约自检（release 下空实现）
 *
 * 在 ftl_init 与设备级 owner 登记完成后调用一次。只证明初始化后**不可变**的绑定：
 * 句柄、storage 数组、端口回调、topology 副本、PBA 布局、bg 任务表、owner/route 身份。
 * 不检查运行期可变状态（phase / 计数 / 池成员），也不替代每次调用的入参断言。
 *
 * @param ftl FTL 上下文（非 NULL；由 ssd_ftl_boot 在 ftl_init 成功后调用）
 */
void ftl_contract_audit(const ftl_context_t *ftl);

/*
 * ftl_service_pump - 实例无关的每轮泵（superloop 每轮调一次）
 *
 * Phase 1: fc_service() 推进 NAND（≤ NAND_SUBMIT_BUDGET 次，首轮即抽干完成）
 * Phase 2: ct_drain() 处理完成回调（按命令 ch/ce/lun 路由回实例）
 *
 * ⚠ 必须在所有 ftl_service() 之前调，且**每轮只调一次**——NAND 引擎与完成表都是
 *   全局单份，per-instance 调用会把 fc_service 预算重复消耗 N 遍。
 */
void ftl_service_pump(void);

/*
 * ftl_service - 单个 FTL 实例的服务（对**每个**实例都要调）
 *
 * Phase 2.5: 补发本实例各组的待下发子操作（host IO 优先于后台）
 * Phase 3:   本实例的时间预算后台调度（ftl_bg_service：选档 → 任务表 → 预算）
 *
 * ⚠ 调用方须先调 ftl_service_pump()。多实例（多 LUN）下漏调某实例 ⇒ 它的
 *   journal / checkpoint / bbt / GC 永不推进。
 *
 * @param ftl  FTL 上下文（**非 NULL**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 */
void ftl_service(ftl_context_t *ftl);

/*
 * ftl_bg_init - 装配实例的时间预算后台调度器
 *
 * 把 `ftl/bg_wiring.c` 的静态任务表（10 项：6 must-run 正确性关键集 + 4 预算任务，
 * 含 bg_a_* 适配器）装载进 ftl->bg。由 ftl_init 末尾调用一次。
 *
 * ⚠ 为什么不做成 ftl.c 直接调 bg_scheduler_init：任务表是 bg_wiring.c 的 static 数据，
 *   外部模块（ftl.c）不该看到它——本函数是这张表的**唯一装配口**（H13 三问：隐藏数组 /
 *   边界检查 / 单点读写，三者皆是）。
 */
void ftl_bg_init(ftl_context_t *ftl);

/*
 * ftl_bg_service - Phase 3 入口：bg_suspend 判定 → 选档 → 档位配置 → 引擎跑一轮
 *
 * ⚠ bg_suspend 判定在函数内**最先**做（早于 must-run pass）：`bbt --erase` 之后介质已空
 *   而 RAM 状态陈旧，任何后台写都是往干净芯片上倒垃圾。调用方不要在外层重复判。
 *
 * @param ftl  FTL 上下文（**非 NULL**；唯一调用点 ftl_service 已保证）
 */
void ftl_bg_service(ftl_context_t *ftl);

/*
 * ftl_bg_level_select - 负载 → 后台档位（纯函数）
 *
 * 判据：
 *   inflight = pend_get_count + pend_get_group_count
 *   inflight==0 ⇒ IDLE；pending > FTL_PENDING_MAX/2 或 queued!=0 ⇒ BUSY；否则 NORMAL。
 * 返回 BG_LEVEL_IDLE / BG_LEVEL_NORMAL / BG_LEVEL_BUSY（bg_scheduler.h）。
 */
bg_level_t ftl_bg_level_select(const ftl_context_t *ftl);

/*
 * ftl_submit_read - 非阻塞提交读请求（长度任意）
 *
 * 请求带**长度**（req->count 个连续 LBA），FTL 自行决定单/双 plane 拆分：
 *   ① 建完成组（一条 host 请求 = 一个组）
 *   ② 逐子操作下发（每个子操作 2 页 Multi-Plane，末条奇数页时单 plane）
 *   ③ 未映射子操作同步返回全零；余下子操作遇资源背压留待 ftl_service Phase 2.5
 *   ④ 数据在 ct_drain 中路由回 buffer
 *
 * ⚠ 一次调用的完成通知（complete_flag/status_out/on_complete）**只在全部子操作
 *   完成后触发一次**。返回 FTL_OK 仅表示已受理（含部分子操作排队），不代表已完成。
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（lba/buffer/count/complete_flag/status_out/on_complete/ctx）
 * @return FTL_OK 已受理，FTL_ERROR 立即失败（参数非法 / LBA 越界 / 组槽满）
 */
ftl_status_t ftl_submit_read(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_submit_write - 非阻塞提交写请求（长度任意）
 *
 * 与 ftl_submit_read 对称：FTL 按 count 拆子操作，每子操作分配新 PBA（**逐 plane
 * 独立选块**——Policy B：SB 两 plane 的块号各自独立）+ 建 desc 链 + 异步写 NAND。
 * Journal + L2P 在 completion handler 中确认 NAND PROGRAM 成功后执行。
 *
 * @param ftl  FTL 上下文
 * @param req  IO 请求描述（同 ftl_submit_read）
 * @return FTL_OK 已受理，FTL_ERROR 立即失败
 */
ftl_status_t ftl_submit_write(ftl_context_t *ftl, const ftl_io_request_t *req);

/*
 * ftl_flush - 落盘已持久化映射（L2P cache 写回 + journal 尾页刷出）
 *
 * 语义 = "把 dirty 元数据落到 NAND"，**非** NVMe 式 barrier（不等待在飞/排队的写）。
 *
 * @param ftl  FTL 上下文（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 */
void ftl_flush(ftl_context_t *ftl);

/*
 * ftl_trim - 取消 [lba, lba+count) 的 LBA 映射（非阻塞、同步完成）
 *
 * @param ftl   FTL 上下文（**前置条件**；实例绑定由 ftl_contract_audit 在 boot 期证明）
 * @param lba   起始逻辑地址（QWORD）
 * @param count 连续 LBA 数（0 视作 1，兼容单 LBA 调用）
 */
void ftl_trim(ftl_context_t *ftl, QWORD lba, DWORD count);

#endif /* FTL_SERVICE_H */