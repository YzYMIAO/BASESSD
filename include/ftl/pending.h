#ifndef FTL_PENDING_H
#define FTL_PENDING_H

/*
 * Pending Request 表 — 跟踪异步请求的 caller 通知/回调与写回滚信息
 *
 * ftl_submit_read/write() 是非阻塞的。提交 FC 命令后，请求的完成处理
 * （读：数据写回 caller buffer；写：Journal/L2P 确认）需要 command_id → 请求
 * 映射。本表记录 command_id → ftl_io_request_t（嵌入，按值拷贝）+ 写专用字段。
 *
 * ⚠ 读写必须**显式携带**操作类型（见 ftl_pending_op_t）：完成处理曾以
 *   `io.buffer != NULL` 判别读写，但 ftl_submit_write 拒绝 NULL buffer
 *   （写请求的 buffer 是**数据源**，已拷进 desc 链）⇒ 该条件对写恒真 ⇒ 写分支
 *   （Journal/L2P/VPC 簿记 + 写失败回滚）成死代码，写完成误走读分支、
 *   把 NAND 数据读回源 buffer，且 total_writes 永不递增。**buffer 非空 ≠ 写。**
 */

#include "common/ftl_types.h" /* ftl_io_request_t, pending_cb_t */
#include "ftl/ftl_type.h"     /* lba_t, ftl_status_t */
#include "nand/nand_config.h" /* nand_pba_t, NAND_PBA_INVALID */

#include <stdint.h>

#define FTL_PENDING_MAX (32U)

/* ── 完成组（ftl_io_group_t）──
 *
 * 一条 host 请求（一次 ftl_submit_read/write 调用）内部可拆成多条 FC 子操作
 * （每个子操作 = 一条 FC 命令 = 一个 pending 槽）。组把「本 host 请求的全部
 * 子操作」聚成一个完成单元：**通知字段（complete_flag/status_out/on_complete）
 * 只在最后一个子操作完成后触发一次**（见 pending_group_complete_subop）。
 *
 * 组容量 = pending 容量（一个组至少占一个 pending 槽，组不会比在飞子操作多）。 */
#define FTL_GROUP_MAX  (FTL_PENDING_MAX)
#define FTL_GROUP_NONE (0xFFFFU) /* 「无组」哨兵（当前无使用者；留作显式语义） */

/* 前向声明（ftl_context_t 在 ftl.h 中定义；本文件被 ftl.h include，
 * 用 struct 标签前向引用打破环） */
struct ftl_context;
typedef struct ftl_context ftl_context_t;

/* ── 操作类型：完成路径的判别依据 ──
 * 必须由 submit 侧显式给出，**不可**从 io 字段反推（见文件头 ⚠）。 */
typedef enum
{
    PENDING_OP_READ  = 0, /* 读：completion 把 NAND 数据写回 io.buffer */
    PENDING_OP_WRITE = 1, /* 写：completion 确认 Journal + L2P（io.buffer 只是数据源，不再读它） */
} ftl_pending_op_t;

typedef struct
{
    uint16_t used;       /* 1: 槽位占用 */
    uint16_t command_id; /* FC command_id */
    uint16_t group;      /* ★ 所属完成组 ftl->groups[] 下标 */
    uint8_t  op;         /* ftl_pending_op_t：完成路径判别（读/写），见文件头 ⚠ */
    uint8_t  planes;     /* 本子操作页数 1/2（原 io.pages 的内部含义） */
    lba_t    lba;        /* 子操作起始 local LBA（host 起始 + 2k） */
    void    *buffer;     /* 子操作缓冲（= 组基缓冲 + 2k × FTL_READ_USER_SIZE） */
    /* ↓↓↓ 写完成确认/回滚（写专用，读请求时无效） ↓↓↓ */
    nand_pba_t old_pba;            /* 写前旧 PBA（journal/回滚用，plane0） */
    nand_pba_t new_pba;            /* 新分配的 PBA（坏块标记用，plane0） */
    nand_pba_t old_pba1;           /* planes=2 时 plane1 写前旧 PBA（Multi-Plane 双页簿记） */
    nand_pba_t new_pba1;           /* planes=2 时 plane1 新分配 PBA（Policy B：plane1 块独立，不可由 plane0 派生） */
    uint8_t    cache_intermediate; /* 1: Cache Program 中间页（15h），completion 时不更新 L2P/Journal */
} ftl_pending_req_t;

/*
 * ftl_io_group_t — 一条 host 请求的完成组（见文件头）
 *
 * 生命周期：pending_group_alloc 建（used=1）→ 逐个 pending 子操作完成时
 * pending_group_complete_subop 递减 outstanding → 全部完成（next==total 且
 * outstanding==0）时 pending_group_maybe_complete 一次性发通知并置 used=0。
 *
 * ⚠ total/next 必须 uint32_t：host 连续 LBA 数 count 可达 FTL_LBA_MAX（1,048,576），
 *   二者都以 **LBA 计**，uint16_t 会溢出。outstanding 被 FC buffer 池限到 ~16
 *   （128 buffer ÷ 每多 plane 命令 8 buffer），uint16_t 够。
 *
 * ⚠ 进度以 **LBA 偏移**计（total = count，next = 已下发到的 LBA 偏移），不是
 *   "子操作序号"：一个子操作消费 1 或 2 个 LBA —— 相邻两 LBA 能否合成一条
 *   multi-plane 命令由 FTL 现场判断（GC/WL 搬移会把 plane1 页落到 plane0，
 *   此时必须降级为单 plane，见 ftl_issue_subop）。这样"怎么拆"不外泄给调用者。
 */
typedef struct
{
    uint8_t                used;            /* 1: 槽位占用 */
    uint8_t                op;              /* PENDING_OP_READ / PENDING_OP_WRITE */
    uint8_t                stalls;          /* 连续无进展 drain 轮数（背压预算，S2 用） */
    uint16_t               outstanding;     /* 已下发、未完成的子操作数 */
    uint32_t               total;           /* 本请求 LBA 总数（= count） */
    uint32_t               next;            /* 已下发到的 LBA 偏移（0..count） */
    uint64_t               lba;             /* host 起始 LBA */
    void                  *buffer;          /* host 基缓冲 */
    uint32_t               count;           /* host 连续 LBA 数 */
    ssd_status_t           agg_status;      /* 粘性错误：任一子操作失败 → 非 OK */
    volatile uint8_t      *complete_flag;   /* 全部子操作完成后置 1 */
    volatile ssd_status_t *status_out;      /* 全部子操作完成后写入 agg_status */
    pending_cb_t           on_complete;     /* 全部子操作完成后调用一次 */
    void                  *on_complete_ctx; /* 回调上下文 */
} ftl_io_group_t;

/*
 * pending_init - 清空本 FTL 实例的 pending 表（F6：per-instance）
 *
 * @param ftl  FTL 上下文（含 pending 数组存储）
 */
void pending_init(ftl_context_t *ftl);

/*
 * pending_add - 注册一个 pending 子操作
 *
 * 子操作字段按值拷入 pending 槽（submit 调用后调用者可复用其局部量）。
 *
 * @param ftl                FTL 上下文
 * @param cmd_id             FC command_id
 * @param group              所属完成组下标（ftl->groups[]）
 * @param lba                子操作起始 local LBA
 * @param buffer             子操作缓冲（读=写回处；写=数据源）
 * @param planes             子操作页数（1=单 Plane，2=双 Plane Multi-Plane）
 * @param old_pba            写前旧 PBA（写请求用，读传 NAND_PBA_INVALID）
 * @param new_pba            新分配的 PBA（写请求用，读传 NAND_PBA_INVALID）
 * @param old_pba1           planes=2 时 plane1 写前旧 PBA（单页请求传 NAND_PBA_INVALID）
 * @param new_pba1           planes=2 时 plane1 新分配 PBA（单页请求传 NAND_PBA_INVALID）
 * @param cache_intermediate Cache Program 中间页标志（1=中间页跳过 L2P/Journal 更新）
 * @param op                 操作类型（PENDING_OP_READ / PENDING_OP_WRITE）——完成路径判别依据
 * @return FTL_OK 成功，FTL_ERROR 表满
 */
int pending_add(ftl_context_t *ftl, uint16_t cmd_id, uint16_t group, lba_t lba, void *buffer, uint8_t planes, nand_pba_t old_pba, nand_pba_t new_pba, nand_pba_t old_pba1, nand_pba_t new_pba1, uint8_t cache_intermediate, ftl_pending_op_t op);

/*
 * pending_lookup - 按 command_id 查找本实例 pending 项
 *
 * @param ftl     FTL 上下文
 * @param cmd_id  FC command_id
 * @return 指向 pending 项的指针, NULL 未找到
 */
ftl_pending_req_t *pending_lookup(ftl_context_t *ftl, uint16_t cmd_id);

/*
 * pending_remove - 移除本实例中已完成的 pending 项
 *
 * @param ftl     FTL 上下文
 * @param cmd_id  FC command_id
 */
void pending_remove(ftl_context_t *ftl, uint16_t cmd_id);

/*
 * pending_count - 本实例当前未决（占用）的 pending 项数
 *
 * B3：ftl_service 三级模式判定用它近似 host IO 空闲度——
 * pending 占用高 = host IO 繁忙 → BUSY；占用低 = 空闲 → IDLE。
 *
 * @param ftl  FTL 上下文
 * @return 占用槽位数（0..FTL_PENDING_MAX）
 */
uint16_t pending_count(const ftl_context_t *ftl);

/* ── 完成组 API（ftl_io_group_t）── */

/*
 * pending_group_alloc - 申请一个空闲完成组槽（used=1，字段清零）
 *
 * @param ftl  FTL 上下文
 * @return 组下标（0..FTL_GROUP_MAX-1），无空闲槽返回 FTL_ERROR
 */
int pending_group_alloc(ftl_context_t *ftl);

/*
 * pending_group_free - 释放完成组槽（used=0）
 *
 * @param ftl  FTL 上下文
 * @param gi   组下标
 */
void pending_group_free(ftl_context_t *ftl, uint16_t gi);

/*
 * pending_group_count - 当前占用（未完成）的组数
 *
 * B3：ftl_service 三级模式判定用它近似"排队未下发的 host 请求"——
 * 有未完成组 = host 工作饿着 → 归 BUSY。
 *
 * @param ftl  FTL 上下文
 * @return 占用组数（0..FTL_GROUP_MAX）
 */
uint16_t pending_group_count(const ftl_context_t *ftl);

/*
 * pending_group_complete_subop - 一个子操作完成：聚合状态 + 递减 + 试触发
 *
 * 由 ftl_handle_completion 在子操作簿记后调用（替代原先直接写 caller 通知字段）。
 * sub_status != OK 时粘进 agg_status；outstanding-- 后试发组完成通知。
 *
 * @param ftl        FTL 上下文
 * @param gi         组下标
 * @param sub_status 本子操作的完成状态（SSD_STATUS_OK / IO_ERROR）
 */
void pending_group_complete_subop(ftl_context_t *ftl, uint16_t gi, ssd_status_t sub_status);

/*
 * pending_group_maybe_complete - 若组已全部下发且全部完成则发通知并释放槽
 *
 * 条件：used 且 next==total 且 outstanding==0。满足则写 status_out/complete_flag、
 * 调 on_complete（**最后**才 used=0，因回调可能重入 submit 再申请组）。
 *
 * @param ftl  FTL 上下文
 * @param gi   组下标
 */
void pending_group_maybe_complete(ftl_context_t *ftl, uint16_t gi);

#endif /* FTL_PENDING_H */
