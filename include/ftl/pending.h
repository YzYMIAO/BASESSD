#ifndef FTL_PENDING_H
#define FTL_PENDING_H

/*
 * Pending Request 表 — 跟踪异步请求的 caller 通知/回调与写回滚信息
 *
 * ftl_submit_read/write() 是非阻塞的。提交 FC 命令后，请求的完成处理
 * （读：数据写回 caller buffer；写：Journal/L2P 确认）需要 command_id → 请求
 * 映射。本表记录 command_id → ftl_io_req_t（嵌入，按值拷贝）+ 写专用字段。
 *
 * ⚠ 读写必须**显式携带**操作类型（见 pend_op_e），**不可**从 io 字段反推：
 *   `io.buffer != NULL` 对写恒真（写请求的 buffer 是**数据源**，已拷进 desc 链，
 *   且 ftl_submit_write 拒绝 NULL）⇒ 用它判别会把写完成误走读分支：Journal/L2P/
 *   VPC 簿记与写失败回滚全不执行，却把 NAND 数据读回源 buffer。
 *   **buffer 非空 ≠ 写。**
 */

#include "ftl_config.h" /* DWORD, NAND_PBA_INVALID */
#include "ftl_module.h" /* bg_info_t（confirm service 入参） */
#include "ftl_type.h"   /* DWORD, ftl_status_e, ftl_io_req_t, pending_cb_t */

#include <stdint.h>

#define FTL_PENDING_MAX (32U)

/* 「本项已与 FC 命令脱钩」哨兵：Read Recovery 托管读失败的子操作时用它覆盖 command_id。
 * 理由：那条 FC 命令随即被 release，其 command_id 会被后续 fc_command_alloc **复用**，
 * 而 pend_find 按 command_id 线性扫首个匹配——留旧值会让新命令的完成误撞上本项。
 * 0xFFFF 不可达：FC_COMMAND_POOL_DEPTH = 64（命令 id 恒 < 64）。 */
#define FTL_PENDING_CMD_NONE (0xFFFFU)

/* ── 完成组（pend_group_t）──
 *
 * 一条 host 请求（一次 ftl_submit_read/write 调用）内部可拆成多条 FC 子操作
 * （每个子操作 = 一条 FC 命令 = 一个 pending 槽）。组把「本 host 请求的全部
 * 子操作」聚成一个完成单元：**通知字段（complete_flag/status_out/on_complete）
 * 只在最后一个子操作完成后触发一次**（见 pend_group_complete_subop）。
 *
 * 组容量 = pending 容量（一个组至少占一个 pending 槽，组不会比在飞子操作多）。 */
#define FTL_GROUP_MAX  (FTL_PENDING_MAX)
#define FTL_GROUP_NONE (0xFFFFU) /* 「无组」哨兵（当前无使用者；留作显式语义） */

/* 组 gi 在 owners 位掩码里的位（pend_req_t.owners / pend_group_t 的约定）。
 * 组号由 pend_group_alloc 给出，恒 < FTL_GROUP_MAX ⇒ 移位量恒小于 DWORD 位宽。
 * 掩码运算一律经本函数：调用点不手写 `1U << gi`（位宽/移位量对不上时是 UB，且无从审查）。 */
static inline DWORD ftl_group_bit(WORD gi)
{
    return 1U << gi;
}

/* 前向声明（ftl_ctx_t 在 ftl.h 中定义；本文件被 ftl.h include，
 * 用 struct 标签前向引用打破环） */
struct ftl_context;
typedef struct ftl_context ftl_ctx_t;

/* ── 操作类型：完成路径的判别依据 ──
 * 必须由 submit 侧显式给出，**不可**从 io 字段反推（见文件头 ⚠）。 */
typedef enum
{
    PENDING_OP_READ          = 0, /* 读：completion 把 NAND 数据写回 io.buffer */
    PENDING_OP_WRITE         = 1, /* 写：completion 确认 Journal + L2P（io.buffer 只是数据源，不再读它） */
    PENDING_OP_READ_RECOVERY = 2, /* 读失败、已**托管**给 Read Recovery job：FC 命令已脱钩
                                   *   （command_id = FTL_PENDING_CMD_NONE）、组在飞数**不**递减
                                   *   —— host 读组继续 pending，等 RR 收尾时统一结算。
                                   *   完成路径不会再碰本项（op != READ/WRITE），只有 RR 引擎
                                   *   用 pend_find_op 找它、用 pend_release_index 收尾。 */
    PENDING_OP_WRITE_CONFIRM = 3, /* 写**已成功**、确认（Journal + L2P 记账）尚未完成：
                                   *   L2P region 未就绪（正在异步加载）时**停泊**本项——
                                   *   FC 命令已脱钩（command_id = FTL_PENDING_CMD_NONE）、
                                   *   组在飞数**不**递减（host 写组继续 pending）。
                                   *   与 READ_RECOVERY 的差别：无外部引擎，由 pend_confirm_service
                                   *   每轮扫描推进（pend_find_op 按下标序找它）。
                                   *   ⚠ 停泊项**不可被 pend_find 撞到**：id 已置哨兵。 */
} pend_op_e;

/* ── op 谓词（判别一律走这里，调用点不裸比枚举值）──
 * 形参取 BYTE：pend_req_t.op / pend_group_t.op 都是 BYTE 字段。
 *
 * 语义：
 *   is_read / is_write —— **活跃**子操作，完成路径按它们分派（拷数据 / 确认记账）。
 *   is_write_confirm   —— **停泊**态（命令已脱钩、组在飞数不递减），由 pend_confirm_service
 *                         每轮扫描推进，完成路径不碰它。
 *   （READ_RECOVERY 同为停泊态，但判别走 pend_find_op 的**搜索实参**，不是谓词。） */
static inline bool pend_op_is_read(BYTE op)
{
    return op == PENDING_OP_READ;
}

static inline bool pend_op_is_write(BYTE op)
{
    return op == PENDING_OP_WRITE;
}

static inline bool pend_op_is_write_confirm(BYTE op)
{
    return op == PENDING_OP_WRITE_CONFIRM;
}

/* 字段按对齐从大到小排列：32 槽 × 4B 尾部填充 = 128B，见下方 static_assert。 */
typedef struct
{
    /* ── 4B 对齐 ── */
    DWORD owners;   /* ★ 贡献本子操作的**组位掩码**（bit gi = ftl_group_bit(gi)）。读恒单组；
                     *   写页打包下一页可能由多个组拼成 ⇒ 完成时逐组递减（见
                     *   pend_group_complete_subop_multi）。 */
    DWORD lba;      /* 子操作起始 **全局** host LBA（CW 级：1 LBA = 1 CW = 512B）。
                     * ⚠ 写页打包：lba = **窗口基址**，逐 CW 的 LBA = lba + bit（bit 同 cw_bitmap） */
    void *buffer;   /* 子操作缓冲（读=写回处，= 组基缓冲 + LBA 偏移 × FTL_IU_BYTES；
                     *   写页打包后恒 NULL——数据在 ftl->wbuf.data，完成时不读它） */
    DWORD new_pba;  /* 新分配的 **plane0 页** PBA（本页未覆盖 plane0 ⇒ NAND_PBA_INVALID） */
    DWORD new_pba1; /* 新分配的 **plane1 页** PBA（未覆盖 plane1 ⇒ NAND_PBA_INVALID） */
    /* ⚠ **不存 old_pba**：完成时按 LBA 重查 L2P 得到"此刻被替换的映射"。这比提交时快照
     *   更正确——同一 LBA 的两次写在飞时，先完成的先改 L2P，后完成的重查即得前者，
     *   提交时快照会让两条 journal 都指向最初映射（链断）。 */
    DWORD park_ms; /* WRITE_CONFIRM 停泊起点（ms；内部字段，外部不要读） */

    /* ── 2B 对齐 ── */
    WORD command_id; /* FC command_id */

    /* ── 1B 对齐 ── */
    bool      used;               /* true: 槽位占用 */
    pend_op_e op;                 /* 完成路径判别（读/写），见文件头 ⚠ */
    BYTE      n_cw;               /* ★ 本子操作搬运的 **CW 数**（写 = popcount(cw_bitmap)；读 = 写回长度/512） */
    BYTE      cw_bitmap;          /* ★ 写：本页有效 CW 位图（bit = plane*CW_NUM+cw）；读：0 */
    bool      cache_intermediate; /* true: Cache Program 中间页（15h），completion 时不更新 L2P/Journal */
    BYTE      retry_count;        /* 写失败后已重试次数（透明重试；见 ftl_host_io_callback） */
} pend_req_t;

_Static_assert(sizeof(pend_req_t) == 32, "pend_req_t 布局漂移（期望 32B）");

/*
 * pend_group_t — 一条 host 请求的完成组（见文件头）
 *
 * 生命周期：pend_group_alloc 建（used=1）→ 逐个 pending 子操作完成时
 * pend_group_complete_subop 递减 outstanding → 全部完成（next==total 且
 * outstanding==0）时 pend_group_maybe_complete 一次性发通知并置 used=0。
 *
 * ⚠ total/next 必须 DWORD：host 连续 LBA 数 count 可达 FTL_LBA_MAX（894,976），
 *   二者都以 **LBA 计**，WORD 会溢出。outstanding 被 FC buffer 池限到 ~16
 *   （128 buffer ÷ 每多 plane 命令 8 buffer），WORD 够。
 *
 * ⚠ 进度以 **LBA 偏移**计（total = count，next = 已下发到的 LBA 偏移），不是
 *   "子操作序号"：CW 级下一个子操作最多消费 FTL_LBA_PER_PAIR（= 8）个 LBA——
 *   一个 pair 窗口内的连续 LBA 段。窗口内的 LBA 落在哪个物理页由 FTL 现场判断
 *   （读路径查 L2P 分组、写路径按 plane 位选页，见 ftl_issue_subop），
 *   消费几个 LBA 也由它决定。这样"怎么拆"不外泄给调用者。
 */
/* 字段按对齐从大到小排列：32 组 × 8B 尾部填充 = 256B，见下方 static_assert。 */
typedef struct
{
    /* ── 8B / 指针 ── */
    QWORD                  lba;             /* host 起始 LBA */
    void                  *buffer;          /* host 基缓冲 */
    volatile BYTE         *complete_flag;   /* 全部子操作完成后置 1 */
    volatile ssd_status_e *status_out;      /* 全部子操作完成后写入 agg_status */
    pending_cb_t           on_complete;     /* 全部子操作完成后调用一次 */
    void                  *on_complete_ctx; /* 回调上下文 */

    /* ── 4B ── */
    DWORD total; /* 本请求 LBA 总数（= count） */
    DWORD next;  /* 已下发到的 LBA 偏移（0..count） */
    DWORD count; /* host 连续 LBA 数 */

    /* ── 2B / 1B ── */
    WORD         outstanding; /* 已下发、未完成的子操作数 */
    BYTE         stalls;      /* 连续无进展 drain 轮数（背压预算，见 FTL_DRAIN_STALL_MAX） */
    bool         used;        /* true: 槽位占用 */
    pend_op_e    op;          /* PENDING_OP_READ / PENDING_OP_WRITE */
    ssd_status_e agg_status;  /* 粘性错误：任一子操作失败 → 非 OK */
} pend_group_t;

_Static_assert(sizeof(pend_group_t) == 48, "pend_group_t 布局漂移（期望 48B）");

/*
 * pend_table_t — pending / group 两张表的 owner 上下文
 *
 * 从 ftl_ctx_t 中提取出来，让需要 pending 的模块（RR / wbuf）只拿这一张表，
 * 而不是整个 FTL 上下文。表仍内嵌在 ftl_ctx_t 中、常驻不拷贝；模块只持有
 * `pend_table_t *`，所有访问走 pending.c 的 owner API。
 */
typedef struct
{
    pend_req_t   slots[FTL_PENDING_MAX];
    pend_group_t groups[FTL_GROUP_MAX];
} pend_table_t;

/*
 * pend_op_view_t — 按 op 查找一个 pending 槽的只读视图
 *
 * `pend_view_op` 只拷贝字段、**不释放槽**；槽的释放仍由调用者在收尾时走
 * `pend_release_index`。RR 在领取被托管的读子操作时使用。
 */
typedef struct
{
    BYTE  index;  /* 槽下标（释放时用） */
    DWORD owners; /* 贡献组位掩码 */
    void *buffer; /* 子操作缓冲 */
    DWORD lba;    /* 子操作起始 LBA */
    BYTE  n_cw;   /* 子操作覆盖的 CW 数 */
} pend_op_view_t;

/*
 * pend_init - 清空 pending / group 两张表（per-instance）
 *
 * @param table  pending 表（由 ftl_ctx_t 内嵌持有）
 */
void pend_init(pend_table_t *table);

/* ── write-confirm park（docs §5「Confirm → pending 服务」）──
 *
 * 停泊列表归 pending 模块；FTL 不再扫 pending[] 找 PENDING_OP_WRITE_CONFIRM。
 * 事务（Journal → L2P → BST ±）仍由 FTL 注入的 handler 执行——pending 只负责
 * 遍历/超时/结算，避免把数据面事务复制进 pending 模块（C2/R9）。
 *
 * handler 返回：
 *   FTL_OK    —— 事务已应用；pending 释放槽 + 组聚合完成；
 *   FTL_AGAIN —— region 未就绪；保留停泊，超 ms deadline 后按 IO_ERROR 收尾；
 *   FTL_ERROR —— 永久失败；释放槽 + 组报 IO_ERROR。
 */
typedef ftl_status_e (*pending_confirm_handler_t)(void *ctx, pend_req_t *pending);

/* 注册事务 handler（ftl_init 中调用一次）。 */
void pend_confirm_set_handler(pending_confirm_handler_t handler);

/* 把一个已成功编程、但确认尚未完成的写子操作登记进停泊列表。 */
void pend_confirm_park(pend_table_t *table, WORD pending_idx, DWORD owners);

/* 推进停泊列表（ftl_service Phase 2.5 开头调用；info 可为 NULL，此时用平台时钟）。 */
void pend_confirm_service(pend_table_t *table, void *confirm_ctx, const bg_info_t *info);

/*
 * pend_alloc - 认领一个空闲 pending 槽（**调用者就地逐字段填**）
 *
 * 返回该槽指针并置 used=1、其余字段清零；表满返回 NULL。字段由调用者（ftl.c 的
 * 读写路径）**就地赋值**——11 个形参的注册函数读不出「这是在登记什么」，改为
 * 逐字段赋值后字段名即文档（见 pend_req_t 的字段注释）。
 *
 * 表满是预期内的资源结果 ⇒ 返回码（判据 3），不 ASSERT。
 *
 * ⚠ 填完后**必须**调 pend_validate() 再提交——owners / n_cw 的自洽不变量在那里守。
 * ⚠ 认领即置 used=1 ⇒ 调用者填字段之前不得有任何可能返 FTL_AGAIN 的早退，
 *   否则槽位泄漏（表里多一条 used=1 的垃圾项）。两个调用点都是「填完立刻用」。
 *
 * @param ftl  FTL 上下文
 * @return 空闲槽指针；NULL = 表满
 */
pend_req_t *pend_alloc(pend_table_t *table);

/*
 * pend_validate - 校验已填好的 pending 项的自洽不变量（只断言，无副作用）
 *
 *   owners != 0  —— 至少一个贡献组（读恒单组；写页打包 ≥1）
 *   n_cw ∈ [1, FTL_LBA_PER_PAIR]
 *
 * ⚠ 这两条原是 pending_add 的 [BOUND] 检查点（§二十六 定的唯一权威点）。改成调用者
 *   填槽后，定义挪到这里**一份**，两个调用点各调一次——不在调用点重复实现。
 */
static inline void pend_validate(const pend_req_t *req)
{
    (void)req; /* release 下 ASSERT 被 NDEBUG 裁空 ⇒ 形参无引用（见文件内既有 allowed/plane_bits 同类） */
    ASSERT(req->owners != 0U);
    ASSERT((req->n_cw > 0U) && (req->n_cw <= FTL_LBA_PER_PAIR));
}

/*
 * pend_find - 按 command_id 查找本实例 pending 项
 *
 * @param ftl     FTL 上下文
 * @param cmd_id  FC command_id
 * @return 指向 pending 项的指针, NULL 未找到
 */
pend_req_t *pend_find(pend_table_t *table, WORD cmd_id);

/*
 * pend_remove - 移除本实例中已完成的 pending 项
 *
 * @param ftl     FTL 上下文
 * @param cmd_id  FC command_id
 */
void pend_remove(pend_table_t *table, WORD cmd_id);

/*
 * pend_find_op - 找**第一个** op 类型匹配的 pending 项（按下标升序）
 *
 * 供 Read Recovery 引擎取走被托管的子操作：那些项的 command_id 已置哨兵
 * （FTL_PENDING_CMD_NONE），按 id 查不到，只能按 op 找。
 *
 * @param table      pending 表
 * @param op         目标操作类型
 * @param out_index  输出：命中项的下标（仅返回 1 时有效）
 * @return true 找到，false 无
 */
bool pend_find_op(const pend_table_t *table, pend_op_e op, BYTE *out_index);

/*
 * pend_view_op - 按 op 取一个 pending 槽的只读视图（不释放槽）
 *
 * 与 pend_find_op 的差别：一次填好 RR 需要的字段（owners/index/buffer/lba/n_cw），
 * 槽的释放仍由调用者在收尾时走 pend_release_index。字段拷贝即“视图”，不是第二套坐标。
 *
 * @param table  pending 表
 * @param op     目标操作类型
 * @param out    输出视图（仅返回 true 时有效）
 * @return true 找到，false 无
 */
bool pend_view_op(const pend_table_t *table, pend_op_e op, pend_op_view_t *out);

/*
 * pend_release_index - 按下标释放 pending 项（used=0）
 *
 * 与 pend_remove 的差别：走**下标**而非 command_id。托管项脱钩后无 id 可查，
 * 收尾只能靠下标。越界是调用者 bug ⇒ ASSERT。
 *
 * @param ftl    FTL 上下文
 * @param index  pending 槽下标（0..FTL_PENDING_MAX-1）
 */
void pend_release_index(pend_table_t *table, BYTE index);

/*
 * pend_get_count - 本实例当前未决（占用）的 pending 项数
 *
 * ftl_service 的档位判定用它近似 host IO 空闲度——
 * pending 占用高 = host IO 繁忙 → BUSY；占用低 = 空闲 → IDLE。
 *
 * @param ftl  FTL 上下文
 * @return 占用槽位数（0..FTL_PENDING_MAX）
 */
WORD pend_get_count(const pend_table_t *table);

/* ── 完成组 API（pend_group_t）── */

/*
 * pend_group_alloc - 申请一个空闲完成组槽（used=1，字段清零）
 *
 * @param ftl  FTL 上下文
 * @return 组下标（0..FTL_GROUP_MAX-1），无空闲槽返回 FTL_ERROR
 */
int pend_group_alloc(pend_table_t *table);

/*
 * pend_get_group_count - 当前占用（未完成）的组数
 *
 * ftl_service 的档位判定用它近似"排队未下发的 host 请求"——
 * 有未完成组 = host 工作饿着 → 归 BUSY。
 *
 * @param ftl  FTL 上下文
 * @return 占用组数（0..FTL_GROUP_MAX）
 */
WORD pend_get_group_count(const pend_table_t *table);

/*
 * pend_group_add_subop / pend_group_drop_subop - 组在飞数增减的唯一闸门
 *
 * 下发前 add；提交失败回滚 drop（不触发完成通知）。完整完成走
 * pend_group_complete_subop*（聚合状态 + 递减 + 试触发）。
 *
 * @param ftl  FTL 上下文
 * @param gi   组下标（0..FTL_GROUP_MAX-1；组必须 used）
 */
void pend_group_add_subop(pend_table_t *table, WORD gi);
void pend_group_drop_subop(pend_table_t *table, WORD gi);

/*
 * pend_group_complete_subop - 一个子操作完成：聚合状态 + 递减 + 试触发
 *
 * 由 ftl_host_io_callback 在子操作簿记后调用（caller 通知字段的唯一写点）。
 * sub_status != OK 时粘进 agg_status；outstanding-- 后试发组完成通知。
 *
 * @param ftl        FTL 上下文
 * @param gi         组下标
 * @param sub_status 本子操作的完成状态（SSD_STATUS_OK / IO_ERROR）
 */
void pend_group_complete_subop(pend_table_t *table, WORD gi, ssd_status_e sub_status);

/*
 * pend_group_complete_subop_multi - 一个子操作完成、归属**多个组**：逐组聚合
 *
 * 页打包写缓冲下一页可能由多个组拼成（一页 = 一次 FC 命令 = 一个 pending 槽），
 * 完成时须对**每个**贡献组各递减一次在飞数（每个组对该窗口至多贡献一次 append ⇒
 * 递增/递减数恒等）。读路径传单组位图（1<<gi），与 pend_group_complete_subop 等价。
 *
 * @param ftl        FTL 上下文
 * @param owners     贡献组位掩码（bit gi）
 * @param sub_status 本子操作的完成状态（SSD_STATUS_OK / IO_ERROR）
 */
void pend_group_complete_subop_multi(pend_table_t *table, DWORD owners, ssd_status_e sub_status);

/*
 * pend_group_maybe_complete - 若组已全部下发且全部完成则发通知并释放槽
 *
 * 条件：used 且 next==total 且 outstanding==0。满足则写 status_out/complete_flag、
 * 调 on_complete（**最后**才 used=0，因回调可能重入 submit 再申请组）。
 *
 * @param table pending 表
 * @param gi    组下标
 */
void pend_group_maybe_complete(pend_table_t *table, WORD gi);

/*
 * pend_group_has_window_pending - 是否还有写组的下一个 LBA 落在指定 pair 窗口
 *
 * 供 wbuf 的空闲 flush 判据使用；扫描 pending 表内的 groups[]，不暴露数组本身。
 *
 * @param table  pending 表
 * @param window pair 窗口基址
 * @return true 有写组的 next 落在本窗口
 */
bool pend_group_has_window_pending(const pend_table_t *table, DWORD window);

#endif /* FTL_PENDING_H */
