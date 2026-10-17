#ifndef FTL_TYPE_H
#define FTL_TYPE_H

#include "common/ssd_types.h" /* ssd_status_t（ftl_io_request_t 的完成状态字段） */

#include <stdint.h>

/*
 * FTL 层基础类型 —— 标量/枚举/拓扑 + 对外请求类型。
 *
 * 地址体系（行业标准）：
 *
 *   LBA (Host)  →  PBA (FTL full physical)  →  PAA (per-chip address)
 *   cmd->meta        L2P 表存的值               nand_paa_t
 *   逻辑地址         [CH][CE][PAA]             [LUN][BLOCK][PLANE][PAGE][CW]
 *
 * lba_t — Host 逻辑地址（uint32_t）。
 *   当前容量 512MiB ÷ 512B = 1,048,576 个 LBA，uint32_t 足够。
 *   作为 cmd->meta 传入 FC 用于 META 校验。
 */
typedef uint32_t lba_t;

/* 非法 LBA 哨兵：容量上限 1,048,576 ⇒ 0xFFFFFFFF 恒不可达。
 * 用于「本 CW 未写过 / 无映射」的逐 CW 输出（GC/WL 从页内 META 取回）。 */
#define FTL_LBA_INVALID ((lba_t)0xFFFFFFFFU)

/*
 * ftl_status_t — FTL 内部操作通用返回状态
 *
 *   FTL_OK    =  0  操作成功
 *   FTL_ERROR = -1  永久失败（配置错误 / 无可用空间）——重试无意义
 *   FTL_AGAIN = -2  瞬态背压（资源暂空）——调用者应**排队重试**
 *
 * FTL_AGAIN 的存在是为了把「资源暂空」与「真失败」分开：FC 命令/desc/buffer 池、
 * SQ、pending 表都可能在某一瞬间满（如 16 个多 plane 子操作同时在飞），但下一轮
 * ftl_service 抽干完成后即恢复。大请求被 FTL 拆成多个子操作时，若把这种瞬态当致命，
 * 请求就会在第 N 个子操作处整体失败——这正是本设计要消除的（见 ftl.c ftl_drain_group）。
 */
typedef enum
{
    FTL_OK    = 0,  /* 成功 */
    FTL_ERROR = -1, /* 永久失败（通用，不细分错误码） */
    FTL_AGAIN = -2, /* 瞬态背压（资源暂空，可重试） */
} ftl_status_t;

/*
 * ftl_topology_t — FTL 拓扑描述
 *
 * 描述本 FTL 实例管理的芯片路由。
 *   ch/ce/lun — 本 FTL 绑定的 NAND 芯片。
 *               元数据（Journal/BBT/Checkpoint/Anchor）存放在此芯片。
 *               用户数据由 allocator 基于此拓扑做负载均衡路由。
 *
 * ftl_init 内部初始化，不暴露给 controller 层。
 * 各子模块在初始化时接收一份副本。
 *
 * ⚠ 2026-10-17 记（**有意不做的优化**，别当漏项）：本类型**可以**改成位域
 *   （ch:2 / ce:2 / lun:?）省下每份 2 字节，但**当前不改**。收益 ~10 字节
 *   （3 B → 1 B，常驻副本仅 3 份：ftl->topo / journal->topo / metadata_dir.topo），
 *   代价四项：① 位宽宏在 nand_config.h，本头在依赖图底座，得加 include 才够得着；
 *   ② 本器件 LUN_ADDR_WIDTH == 0，不能声明 `lun : 0`（C11 6.7.2.1p4）——隐去该成员
 *   会打断全仓 20+ 处 `.lun`（本类型 HIL 可见，见 main.c 的 ftl_init 调用），
 *   给 `lun : 1` 兜底则是个装饰位（PBA 里根本没有 lun 位，唯一合法值是 0）；
 *   ③ 位域布局 implementation-defined（同 nand_pba_fields_t，得配开机自检）；
 *   ④ CH_NUM 长大后静默截断，而防它的 _Static_assert 放不进本头。
 *   ⇒ 换多 die 芯片（LUN_ADDR_WIDTH > 0）或本类型副本数显著增多时再重估。
 */
typedef struct
{
    uint8_t ch;
    uint8_t ce;
    uint8_t lun;
} ftl_topology_t;

/* ── 对外请求类型 ──
 *
 * （原 include/common/ftl_types.h，并入本文件：内容全是 FTL 概念，放 common/ 会让
 *   依赖图底层的目录反向 include ftl/ftl_type.h；且两文件名只差一个 s，易混。）
 *
 * 聚合对外接口的高频参数，减少形参数（ftl_submit_read/write 8→2 已落地；
 * ftl_init 7→2 / allocator_init 8→2 曾由 ftl_init_params_t 落地，P2-5 后该结构体
 * 已删除——大数组改由 ftl_context_t **自持**（`ftl->tables`，定义于
 * include/ftl/allocator.h —— 因其依赖 super_block_t，放此处会形成循环 include）：
 *   pending_cb_t      — pending 完成回调（Step 3 从 pending.h 迁入，打破循环 include）
 *   ftl_io_request_t  — 一次读写请求的完整描述（含通知/回调字段，Step 3 嵌入 pending）
 *
 * 设计原则：请求结构体用普通字段而非位域（访问更快，C 编译器无
 * 位域可移植性问题）；**地址一律用 nand_pba_t**（ftl/ftl_config.h §三.2）。
 * 具名字段访问经 pba_fields() 值语义视图（4 字节位域，仅内存内，不落介质；
 * 见 ftl_config.h §三.2）。本文件属 FTL 公共层 ⇒ 不出现 nand_addr_t /
 * nand_paa_t——那是 NAND 层的表示。
 */

/* pending 完成回调 — 在 ftl_op_host_io 数据拷贝 + 标志置位后调用。
 * 定义于此（而非 pending.h），避免 pending.h ↔ ftl_type.h 循环 include。 */
typedef void (*pending_cb_t)(void *ctx);

/* ── 请求参数 ── */

/*
 * ftl_io_request_t — 一次 FTL 读写请求的完整参数。
 *
 * 由 Controller 层构造后整体传入 ftl_submit_read / ftl_submit_write
 * （形参 (ftl, req) 共 2 个，替换原先 8 个散落形参）。
 * pending 表按值嵌入本结构体（ftl->pending[i].io = *req），submit 调用期间
 * 只要求 req 有效，之后完成处理用 pending 表中的拷贝。
 */
typedef struct
{
    uint64_t               lba;             /* Host 逻辑地址（NVMe 协议宽度） */
    void                  *buffer;          /* 读：caller 数据 buffer（数据写回处）；写：数据源 */
    uint32_t               count;           /* ★ 连续 LBA 数（= 上层 block_count）；FTL 自行决定单/双 plane 拆分 */
    volatile uint8_t      *complete_flag;   /* 非 NULL 时全部子操作完成后置 1（异步跟踪） */
    volatile ssd_status_t *status_out;      /* 非 NULL 时组完成后写入 SSD_STATUS_OK/IO_ERROR */
    pending_cb_t           on_complete;     /* 组完成后回调（可选，Controller 子请求聚合） */
    void                  *on_complete_ctx; /* 回调上下文（ssd_request_t*） */
} ftl_io_request_t;

#endif /* FTL_TYPE_H */
