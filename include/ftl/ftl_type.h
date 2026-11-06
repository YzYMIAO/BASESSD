#ifndef FTL_TYPE_H
#define FTL_TYPE_H

#include "ssd_types.h" /* ssd_status_e（ftl_io_req_s 的完成状态字段） */

#include <stdint.h>

/* FTL 实例句柄的不透明类型（完整定义在 ftl.h）。模块头用它声明形参指针，
 * 避免反向 include ftl.h 形成头文件环（ftl.h 会 include 全部模块头）。 */
typedef struct FTL_CONTEXT_S ftl_ctx_s;

/*
 * FTL 层基础类型 —— 标量/枚举/拓扑 + 对外请求类型。
 *
 * 地址体系（行业标准）：
 *
 *   LBA (Host)  →  PBA (FTL full physical)  →  PAA (per-chip address)
 *   cmd->meta        L2P 表存的值               DWORD
 *   逻辑地址         [CH][CE][PAA]             [LUN][BLOCK][PLANE][PAGE][CW]
 *
 * Host 逻辑地址 = DWORD（32-bit）。
 *   容量口径：32-bit LBA × 4 KiB AU = 2^32 × 4 KiB = **16 TiB**（主流消费级上限）；
 *   本仓当前 IU = 512 B（1 CW），32-bit 对应 2 TiB，同样足够。
 *   作为 cmd->meta 传入 FC 用于 META 校验。
 */

/* 非法 LBA 哨兵：容量上限 1,048,576 ⇒ 0xFFFFFFFF 恒不可达。
 * 用于「本 CW 未写过 / 无映射」的逐 CW 输出（GC/WL 从页内 META 取回）。 */
#define FTL_LBA_INVALID (0xFFFFFFFFU)

/*
 * ftl_status_e — FTL 内部操作通用返回状态
 *
 *   FTL_OK      = 0  操作成功
 *   FTL_ERROR   = 1  永久失败（配置错误 / 无可用空间）——重试无意义
 *   FTL_AGAIN   = 2  瞬态背压（资源暂空）——调用者应**排队重试**
 *   FTL_TIMEOUT = 3  等待超时（ms deadline 到期）——调用者按失败收尾
 *
 * ⚠ 状态值一律**非负**：0 = 成功，其余为具名错误码。调用者按具名值分派，
 *   不要写 `status < 0` 这类依赖符号的判据。
 *
 * FTL_AGAIN 的存在是为了把「资源暂空」与「真失败」分开：FC 命令/desc/buffer 池、
 * SQ、pending 表都可能在某一瞬间满（如 16 个多 plane 子操作同时在飞），但下一轮
 * ftl_service 抽干完成后即恢复。大请求被 FTL 拆成多个子操作时，若把这种瞬态当致命，
 * 请求就会在第 N 个子操作处整体失败——这正是本设计要消除的（见 ftl.c ftl_drain_group）。
 *
 * FTL_TIMEOUT 与 NVMe timeout / POSIX -ETIMEDOUT 同构：它**不是** FTL_AGAIN
 * （重试无意义），也**不是** FTL_ERROR（失败原因可归因到等待者）。模块的
 * `*_status_t.timeout_phase` 给出超时发生在哪一步（docs/archive/ftl_module_contracts.md §6.3）。
 */
typedef enum FTL_STATUS_E
{
    FTL_OK      = 0U, /* 成功 */
    FTL_ERROR   = 1U, /* 永久失败（通用，不细分错误码） */
    FTL_AGAIN   = 2U, /* 瞬态背压（资源暂空，可重试） */
    FTL_TIMEOUT = 3U, /* 等待超时（ms deadline 到期） */
} ftl_status_e;

/* 后台档位（load level）：bg_scheduler 选择并发布、typed 模块入口消费。
 * 三档决定 must-run 之外的任务条数与预算，见 bg_scheduler.h。 */
typedef enum BG_LEVEL_E
{
    BG_LEVEL_IDLE   = 0,
    BG_LEVEL_NORMAL = 1,
    BG_LEVEL_BUSY   = 2,
} bg_level_e;

/* 档位值域上界：档位表数组维度 + 遍历/边界用（H40）。 */
#define BG_LEVEL_NUM (3U)

/*
 * ftl_topo_s — FTL 拓扑描述
 *
 * 描述本 FTL 实例管理的芯片路由。
 *   ch/ce/lun — 本 FTL 绑定的 NAND 芯片。
 *               元数据（Journal/BBT/Checkpoint/目录检查点）存放在此芯片。
 *               用户数据由 allocator 基于此拓扑做负载均衡路由。
 *
 * ftl_init 内部初始化，不暴露给 controller 层。
 * 各子模块在初始化时接收一份副本。
 *
 * ⚠ **有意不做的优化**（别当漏项）：本类型**可以**改成位域
 *   （ch:2 / ce:2 / lun:?）省下每份 2 字节，但**当前不改**。收益 ~10 字节
 *   （3 B → 1 B，常驻副本仅 3 份：ftl->topo / journal->topo / metadata_dir.topo），
 *   代价四项：① 位宽宏在 nand_config.h，本头在依赖图底座，得加 include 才够得着；
 *   ② 本器件 LUN_ADDR_WIDTH == 0，不能声明 `lun : 0`（C11 6.7.2.1p4）——隐去该成员
 *   会打断全仓 20+ 处 `.lun`（本类型 HIL 可见，见 main.c 的 ftl_init 调用），
 *   给 `lun : 1` 兜底则是个装饰位（PBA 里根本没有 lun 位，唯一合法值是 0）；
 *   ③ 位域布局 implementation-defined（同 nand_pba_fields_s，得配开机自检）；
 *   ④ CH_NUM 长大后静默截断，而防它的 _Static_assert 放不进本头。
 *   ⇒ 换多 die 芯片（LUN_ADDR_WIDTH > 0）或本类型副本数显著增多时再重估。
 */
typedef struct FTL_TOPOLOGY_S
{
    BYTE ch;
    BYTE ce;
    BYTE lun;
} ftl_topo_s;

/* ── 对外请求类型 ──
 *
 * （原 include/common/ftl_types.h，并入本文件：内容全是 FTL 概念，放 common/ 会让
 *   依赖图底层的目录反向 include ftl/ftl_type.h；且两文件名只差一个 s，易混。）
 *
 * 聚合对外接口的高频参数，减少形参数（如 ftl_submit_read/write 只收 2 个实参）。
 * 五张表由 HIL 独立定义、FTL 只收引用（`ftl_table_refs_s`，定义于
 * include/ftl/allocator.h —— 因其依赖 super_block_s，放此处会形成循环 include）：
 *   pending_cb_fn_t      — pending 完成回调（Step 3 从 pending.h 迁入，打破循环 include）
 *   ftl_io_req_s  — 一次读写请求的完整描述（含通知/回调字段，Step 3 嵌入 pending）
 *
 * 设计原则：请求结构体用普通字段而非位域（访问更快，C 编译器无
 * 位域可移植性问题）；**地址一律用 DWORD**（ftl/ftl_config.h §三.2）。
 * 具名字段访问经 pba_fields() 值语义视图（4 字节位域，仅内存内，不落介质；
 * 见 ftl_config.h §三.2）。本文件属 FTL 公共层 ⇒ 不出现 nand_addr_s /
 * PAA——那是 NAND 层的表示。
 */

/* pending 完成回调 — 在 ftl_host_io_callback 数据拷贝 + 标志置位后调用。
 * 定义于此（而非 pending.h），避免 pending.h ↔ ftl_type.h 循环 include。 */
typedef void (*pending_cb_fn_t)(void *ctx);

/* ── 请求参数 ── */

/*
 * ftl_io_req_s — 一次 FTL 读写请求的完整参数。
 *
 * 由 Controller 层构造后整体传入 ftl_submit_read / ftl_submit_write
 * （形参 (ftl, req) 共 2 个）。
 * pending 表按值嵌入本结构体（ftl->pending[i].io = *req），submit 调用期间
 * 只要求 req 有效，之后完成处理用 pending 表中的拷贝。
 */
typedef struct FTL_IO_REQUEST_S
{
    QWORD lba;                              /* Host 逻辑地址（NVMe 协议宽度） */
    void *buffer;                           /* 读：caller 数据 buffer（数据写回处）；写：数据源 */
    DWORD count;                            /* ★ 连续 LBA 数（= 上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    DWORD cmd_remaining;                    /* 同一 host 命令在本请求之后还剩的 LBA 数（0 = 本请求是命令末尾）。
                                             * **只**用于识别「命令尾不足一个 pair」并在最后一个整 pair
                                             * 数据页上提前收尾（尾 1..7 LBA 只能走 wbuf/普通读）。
                                             * host 命令边界**不是** cache 序列的断点：设备序列跨命令延续，
                                             * 没有数据页可 END 时由 padding END 收尾（ftl_design §3.16）。
                                             * 也**不是**开启顺序读写的开关。 */
    volatile BYTE         *complete_flag;   /* 非 NULL 时全部子操作完成后置 1（异步跟踪） */
    volatile ssd_status_e *status_out;      /* 非 NULL 时组完成后写入 SSD_STATUS_OK/IO_ERROR */
    pending_cb_fn_t        on_complete;     /* 组完成后回调（可选，Controller 子请求聚合） */
    void                  *on_complete_ctx; /* 回调上下文（ssd_req_s*） */
} ftl_io_req_s;

#endif /* FTL_TYPE_H */
