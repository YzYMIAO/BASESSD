#ifndef FTL_PENDING_H
#define FTL_PENDING_H

/*
 * Pending Request 表 — 跟踪异步请求的 caller 通知/回调与写回滚信息
 *
 * ftl_submit_read/write() 是非阻塞的。提交 FC 命令后，请求的完成处理
 * （读：数据写回 caller buffer；写：Journal/L2P 确认）需要 command_id → 请求
 * 映射。本表逐槽记录该子操作的完成组、LBA、缓冲与新 PBA 等元数据。
 *
 * ⚠ 读写必须**显式携带**操作类型（见 pend_op_e），**不可**从 buffer 字段反推：
 *   `buffer != NULL` 对写恒真（写请求的 buffer 是**数据源**，已拷进 desc 链，
 *   且 ftl_submit_write 拒绝 NULL）⇒ 用它判别会把写完成误走读分支：Journal/L2P/
 *   VPC 簿记与写失败回滚全不执行，却把 NAND 数据读回源 buffer。
 *   **buffer 非空 ≠ 写。**
 */

#include "assertion.h"  /* ASSERT（inline 组计数 fast path） */
#include "ftl_config.h" /* DWORD, NAND_PBA_INVALID */
#include "ftl_type.h"   /* DWORD, ftl_status_e, pending_cb_t, ssd_status_e */

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
 * 组容量取 pending 容量：**稳态**下一个组至少占一个槽（组不会比在飞子操作多）。
 * ⚠ 提交路径先 pend_group_alloc、后 pend_alloc ⇒ 瞬时可以「组已建、槽未分」，
 *   严格上界是「组数 ≤ 槽数 + 在飞 host 请求数」。当前 host 串行（在飞请求 ≤1~2），
 *   离 32 项上限很远；若将来 host 并发队列变深，需要重新论证这个等号。 */
#define FTL_GROUP_MAX (FTL_PENDING_MAX)

/**
 * @brief 组 gi 在 owners 位掩码里的位（pend_req_t.owners / pend_group_t 的约定）。
 * 组号由 pend_group_alloc 给出，恒 < FTL_GROUP_MAX ⇒ 移位量恒小于 DWORD 位宽。
 * 掩码运算一律经本函数：调用点不手写 `1U << gi`（位宽/移位量对不上时是 UB，且无从审查）。
 *
 * @param gi 组下标（0..FTL_GROUP_MAX-1）。
 *
 * @return 该组在 owners 位掩码里的位。
 */
static inline DWORD ftl_group_bit(WORD gi)
{
    return 1U << gi;
}

/* ── 操作类型：完成路径的判别依据 ──
 * 必须由 submit 侧显式给出，**不可**从 buffer 字段反推（见文件头 ⚠）。 */
typedef enum
{
    PENDING_OP_READ          = 0, /* 读：completion 把 NAND 数据写回 buffer */
    PENDING_OP_WRITE         = 1, /* 写：completion 确认 Journal + L2P（buffer 只是数据源，不再读它） */
    PENDING_OP_READ_RECOVERY = 2, /* 读失败、已**托管**给 Read Recovery job：FC 命令已脱钩
                                   *   （command_id = FTL_PENDING_CMD_NONE）、组在飞数**不**递减
                                   *   —— host 读组继续 pending，等 RR 收尾时统一结算。
                                   *   完成路径不会再碰本项（op != READ/WRITE），只有 RR 引擎
                                   *   用 pend_find_op 找它、用 pend_release_index 收尾。 */
    PENDING_OP_WRITE_CONFIRM = 3, /* 写**已成功**、确认（Journal + L2P 记账）尚未完成：
                                   *   L2P region 未就绪（正在异步加载）时把本项置为**待入账**——
                                   *   FC 命令已脱钩（command_id = FTL_PENDING_CMD_NONE）、
                                   *   组在飞数**不**递减（host 写组继续 pending）。
                                   *   与 READ_RECOVERY 的差别：无外部引擎，由调用者的写确认服务
                                   *   每轮用 pend_confirm_find_next 扫描推进。
                                   *   ⚠ 待入账项**不可被 pend_find 撞到**：id 已置哨兵。
                                   *   术语：本仓中文名「待入账」，主流称 deferred write completion
                                   *   （DFTL 的 translation page miss 导致映射更新延后）。英文代码
                                   *   仍用 park / confirm 命名（本轮只收敛中文）。 */
    PENDING_OP_READ_CACHE    = 4, /* FTL cache read 批次成员：完成拷回 host，
                                   *   但失败**不**托管给 RR（RR 会向同 LUN 插命令，
                                   *   破坏仍在队列里的 cache sequence）——直接报 IO_ERROR。 */
    PENDING_OP_WRITE_CACHE   = 5, /* FTL cache program 序列成员：页的最终结论由
                                   *   **下一条命令的 FAIL_CACHE**（或 END 的 FAIL）给出，
                                   *   见 cache_role。完成前只释放命令，不确认映射/不递减组。 */
    PENDING_OP_WRITE_PAGE0   = 6, /* 页0 补全写：命令覆盖两 plane（缺失 plane 写零），
                                   *   映射只按 cw_bitmap 的原始位图确认。 */
} pend_op_e;

/* ── cache program 页在序列中的角色（PENDING_OP_WRITE_CACHE 专用）──
 * 完成路径据此判断「本命令给哪一页的最终结论」：
 *   FIRST        —— 序列首条：没有本序列的上一页，FAILC 是残留，不据此结算；
 *   INTERMEDIATE —— 中间页：FAILC 结算**上一页**，本页等下一命令；
 *   END          —— 末条（10h）：FAILC 结算上一页，FAIL/NAND_FAIL 结算本页。
 * 值域 0..3 由 ARM EABI 短枚举落为 1 B，与旧 bool 字段同宽（pend_req_t 仍 32 B）。 */
typedef enum
{
    PEND_CACHE_ROLE_NONE = 0, /* 非 cache program 成员（普通写 / page0 / 读） */
    PEND_CACHE_ROLE_FIRST,
    PEND_CACHE_ROLE_INTERMEDIATE,
    PEND_CACHE_ROLE_END,
} pend_cache_role_e;

/* ── op 谓词（判别一律走这里，调用点不裸比枚举值）──
 * 形参取 BYTE：pend_req_t.op / pend_group_t.op 都是 BYTE 字段。
 *
 * 语义：
 *   is_read / is_write —— **活跃**子操作，完成路径按它们分派（拷数据 / 确认记账）。
 *   is_write_confirm   —— **待入账**态（命令已脱钩、组在飞数不递减），由写确认服务
 *                         每轮扫描推进，完成路径不碰它。
 *   （READ_RECOVERY 同为待入账态，但判别走 pend_find_op 的**搜索实参**，不是谓词。） */
/**
 * @brief 是否活跃读子操作（普通读或 cache 读）。
 *
 * @param op pending 操作码。
 *
 * @return true = 读类活跃子操作。
 */
static inline bool pend_op_is_read(BYTE op)
{
    return (op == PENDING_OP_READ) || (op == PENDING_OP_READ_CACHE);
}

/**
 * @brief 是否活跃写子操作（普通写或 page0 写）。
 *
 * @param op pending 操作码。
 *
 * @return true = 写类活跃子操作。
 */
static inline bool pend_op_is_write(BYTE op)
{
    return (op == PENDING_OP_WRITE) || (op == PENDING_OP_WRITE_PAGE0);
}

/**
 * @brief 是否待入账态写确认子操作。
 *
 * @param op pending 操作码。
 *
 * @return true = WRITE_CONFIRM（命令已脱钩，由写确认服务推进）。
 */
static inline bool pend_op_is_write_confirm(BYTE op)
{
    return op == PENDING_OP_WRITE_CONFIRM;
}

/**
 * @brief cache program 批次成员（中间页或末页）。
 *
 * @param op pending 操作码。
 *
 * @return true = WRITE_CACHE。
 */
static inline bool pend_op_is_write_cache(BYTE op)
{
    return op == PENDING_OP_WRITE_CACHE;
}

/* 字段按对齐从大到小排列；_Static_assert 钉住 32B。 */
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
    DWORD park_ms; /* WRITE_CONFIRM 待入账**预算起点**（ms；内部字段，外部不要读）。
                    * 语义 = 「最近一次确知有进展的时刻」：确认 handler 在其依赖确实在飞
                    * （region 加载 / Journal 页写）时刷新它，超时判据因此只在**无进展**
                    * 时才成立（RISK-HOST-05 支路 (a)，见 docs/design/assertion_guidelines）。 */

    /* ── 2B 对齐 ── */
    WORD command_id; /* FC command_id */

    /* ── 1B 对齐 ── */
    bool              used;        /* true: 槽位占用 */
    pend_op_e         op;          /* 完成路径判别（读/写），见文件头 ⚠ */
    BYTE              n_cw;        /* ★ 本子操作搬运的 **CW 数**（写 = popcount(cw_bitmap)；读 = 写回长度/512） */
    BYTE              cw_bitmap;   /* ★ 写：本页有效 CW 位图（bit = plane*CW_NUM+cw）；读：0 */
    pend_cache_role_e cache_role;  /* cache program 序列中的页角色；NONE = 其他写 */
    BYTE              retry_count; /* 写失败后已重试次数（透明重试；见 ftl_host_io_callback） */
    BYTE              cache_gen;   /* cache program 成员所属**写序列代号**：完成时与
                                    * `cache_stream.seq.gen` 比对，不一致 ⇒ 序列已中止/已被新序列
                                    * 取代，本页不得确认映射（迟到完成，RISK-CACHE-19） */
} pend_req_t;

_Static_assert(sizeof(pend_req_t) == 36, "pend_req_t 布局漂移（期望 36B）");

/* pending 只有 new_pba/new_pba1 两个 plane 字段：维度一旦 >2，必须显式改结构，
 * 不能靠 `pba[0]/pba[1]` 静默漏掉新增 plane（H39）。 */
_Static_assert(PLANE_NUM == 2U, "pend_req_t 固定两 plane 字段；PLANE_NUM>2 需扩展结构");

/*
 * pend_group_t — 一条 host 请求的完成组（见文件头）
 *
 * 生命周期：pend_group_alloc 建（used=1）→ 逐个 pending 子操作完成时
 * pend_group_complete_subop 递减 outstanding → 全部完成（next==total 且
 * outstanding==0）时 pend_group_maybe_complete 一次性发通知并置 used=0。
 *
 * ⚠ total/next 必须 DWORD：host 连续 LBA 数 count 可达 FTL_LBA_MAX（868,352，424 MiB），
 *   二者都以 **LBA 计**，WORD 会溢出。outstanding 被 FC buffer 池限到 ~16
 *   （128 buffer ÷ 每多 plane 命令 8 buffer），WORD 够。
 *
 * ⚠ 进度以 **LBA 偏移**计（total = count，next = 已下发到的 LBA 偏移），不是
 *   "子操作序号"：CW 级下一个子操作最多消费 FTL_LBA_PER_PAIR（= 8）个 LBA——
 *   一个 pair 窗口内的连续 LBA 段。窗口内的 LBA 落在哪个物理页由 FTL 现场判断
 *   （读路径查 L2P 分组、写路径按 plane 位选页，见 ftl_issue_subop），
 *   消费几个 LBA 也由它决定。这样"怎么拆"不外泄给调用者。
 */
/* 字段按对齐从大到小排列；_Static_assert 钉住 48B（-fshort-enums：枚举 1B）。 */
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
    DWORD total;         /* 本请求（run）LBA 总数（进度以 LBA 计） */
    DWORD next;          /* 已下发到的 LBA 偏移（0..total） */
    DWORD cmd_remaining; /* 同一 host 命令在本请求之后还剩的 LBA 数（0 = 命令末尾；见 ftl_io_req_t） */

    /* ── 2B / 1B ── */
    WORD         outstanding; /* 已下发、未完成的子操作数 */
    BYTE         stalls;      /* 连续无进展 drain 轮数（背压预算，见 FTL_DRAIN_STALL_MAX） */
    bool         used;        /* true: 槽位占用 */
    pend_op_e    op;          /* PENDING_OP_READ / PENDING_OP_WRITE */
    ssd_status_e agg_status;  /* 粘性错误：任一子操作失败 → 非 OK */
} pend_group_t;

_Static_assert(sizeof(pend_group_t) == 48, "pend_group_t 布局漂移（期望 48B）");

/*
 * pending_ctx_t — pending / group 两张表的 owner 上下文
 *
 * 从 ftl_ctx_t 中提取出来，让需要 pending 的模块（RR / wbuf）只拿这一个
 * owner 上下文，而不是整个 FTL 上下文。两张表仍内嵌在 ftl_ctx_t 中、常驻不拷贝；
 * 模块只持有 `pending_ctx_t *`，所有访问走 pending.c 的 owner API。
 */
typedef struct
{
    pend_req_t   slots[FTL_PENDING_MAX];
    pend_group_t groups[FTL_GROUP_MAX];
} pending_ctx_t;

/**
 * @brief 是否有**非 cache 序列**的写在飞或待入账（wbuf / GC/WL 的普通写确认）。
 *
 * 用途：cache 写序列开启前用它确认"顺序页不会被普通条目插进来"——顺序映射页
 * 不携带 LBA，混装会破坏隐式坐标（见 journal.h 的 JRN_PAGE_SEG）。
 *
 * @param table pending 表
 * @return true = 至少一条 WRITE / WRITE_PAGE0 / WRITE_CONFIRM 且 cache_role==NONE
 *         false = 没有普通写确认
 */
bool pend_has_foreign_write(const pending_ctx_t *table);

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

/**
 * @brief 清空 pending / group 两张表（per-instance）
 *
 * @param table  pending 表（由 ftl_ctx_t 内嵌持有）
 */
void pend_init(pending_ctx_t *table);

/**
 * @brief 认领一个空闲 pending 槽（**调用者就地逐字段填**）
 *
 * 返回该槽指针并置 used=1、其余字段清零；表满返回 NULL。字段由调用者（ftl / wbuf /
 * cache 批次的读写路径）**就地逐字段赋值**：字段名即文档（见 pend_req_t 的字段注释），
 * 不要做成多形参注册函数——那读不出「在登记什么」。
 *
 * 表满是预期内的资源结果 ⇒ 返回码（判据 3），不 ASSERT。
 *
 * ⚠ 填完后**必须**调 pend_validate() 再提交——owners / n_cw 的自洽不变量在那里守。
 * ⚠ 认领即置 used=1 ⇒ 调用者填字段之前不得有任何可能返 FTL_AGAIN 的早退，
 *   否则槽位泄漏（表里多一条 used=1 的垃圾项）。各调用点均填完立刻使用。
 *
 * @param table  pending 表
 * @return 空闲槽指针；NULL = 表满
 */
pend_req_t *pend_alloc(pending_ctx_t *table);

/**
 * @brief 校验已填好的 pending 项的自洽不变量（只断言，无副作用）
 *
 *   owners != 0  —— 至少一个贡献组（读恒单组；写页打包 ≥1）
 *   n_cw ∈ [1, FTL_LBA_PER_PAIR]
 *
 * ⚠ 这两条 [BOUND] 不变量只在这里定义一份；pend_alloc 的每个调用点填完槽后各调一次，
 *   不在调用点重复实现。
 *
 * @param req 已填好的 pending 项。
 */
static inline void pend_validate(const pend_req_t *req)
{
    (void)req; /* release（NDEBUG）下 ASSERT 被裁空 ⇒ 形参无引用 */
    ASSERT(req->owners != 0U);
    ASSERT((req->n_cw > 0U) && (req->n_cw <= FTL_LBA_PER_PAIR));
}

/**
 * @brief 按 command_id 查找本实例 pending 项
 *
 * @param table     pending 表
 * @param cmd_id  FC command_id
 * @return 指向 pending 项的指针, NULL 未找到
 */
pend_req_t *pend_find(pending_ctx_t *table, WORD cmd_id);

/**
 * @brief 移除本实例中已完成的 pending 项
 *
 * @param table     pending 表
 * @param cmd_id  FC command_id
 */
void pend_remove(pending_ctx_t *table, WORD cmd_id);

/**
 * @brief 找**第一个** op 类型匹配的 pending 项（按下标升序）
 *
 * 供 Read Recovery 引擎取走被托管的子操作：那些项的 command_id 已置哨兵
 * （FTL_PENDING_CMD_NONE），按 id 查不到，只能按 op 找。
 *
 * @param table      pending 表
 * @param op         目标操作类型
 * @param out_index  输出：命中项的下标（仅返回 1 时有效）
 * @return true 找到，false 无
 */
bool pend_find_op(const pending_ctx_t *table, pend_op_e op, BYTE *out_index);

/**
 * @brief 按 op 取一个 pending 槽的只读视图（不释放槽）
 *
 * 与 pend_find_op 的差别：一次填好 RR 需要的字段（owners/index/buffer/lba/n_cw），
 * 槽的释放仍由调用者在收尾时走 pend_release_index。字段拷贝即“视图”，不是第二套坐标。
 *
 * @param table  pending 表（非空）
 * @param op     目标操作类型
 * @param out    输出视图（非空；仅返回 true 时有效）
 * @return true 找到，false 无
 */
bool pend_view_op(const pending_ctx_t *table, pend_op_e op, pend_op_view_t *out);

/**
 * @brief 按下标释放 pending 项（used=0）
 *
 * 与 pend_remove 的差别：走**下标**而非 command_id。托管项脱钩后无 id 可查，
 * 收尾只能靠下标。越界是调用者 bug ⇒ ASSERT。
 *
 * @param table    pending 表
 * @param index  pending 槽下标（0..FTL_PENDING_MAX-1）
 */
void pend_release_index(pending_ctx_t *table, BYTE index);

/**
 * @brief 本实例当前未决（占用）的 pending 项数
 *
 * ftl_service 的档位判定用它近似 host IO 空闲度——
 * pending 占用高 = host IO 繁忙 → BUSY；占用低 = 空闲 → IDLE。
 *
 * @param table  pending 表
 * @return 占用槽位数（0..FTL_PENDING_MAX）
 */
WORD pend_slot_get_count(const pending_ctx_t *table);

/* ── 完成组 API（pend_group_t）── */

/**
 * @brief 申请一个空闲完成组槽（used=1，字段清零）
 *
 * @param table   pending 表
 * @param out_gi  输出：组下标（0..FTL_GROUP_MAX-1；仅返回 true 时有效）
 * @return true 成功；false = 组槽满
 */
bool pend_group_alloc(pending_ctx_t *table, BYTE *out_gi);

/**
 * @brief 当前占用（未完成）的组数
 *
 * ftl_service 的档位判定用它近似"排队未下发的 host 请求"——
 * 有未完成组 = host 工作饿着 → 归 BUSY。
 *
 * @param table  pending 表
 * @return 占用组数（0..FTL_GROUP_MAX）
 */
WORD pend_group_get_count(const pending_ctx_t *table);

/**
 * @brief 组在飞数增减的唯一闸门
 *
 * 下发前 add；提交失败回滚 drop（不触发完成通知）。完整完成走
 * pend_group_complete_subop*（聚合状态 + 递减 + 试触发）。
 *
 * add 是单字段 fast path（H42：头文件 inline）；drop / complete 带下溢策略与完成回调，
 * 定义在 `.c`。
 *
 * @param table  pending 表
 * @param gi   组下标（0..FTL_GROUP_MAX-1；组必须 used）
 */
static inline void pend_group_add_subop(pending_ctx_t *table, WORD gi)
{
    ASSERT(gi < FTL_GROUP_MAX);
    ASSERT(table->groups[gi].used == true);

    table->groups[gi].outstanding++;
}

void pend_group_drop_subop(pending_ctx_t *table, WORD gi);

/**
 * @brief 一个子操作完成：聚合状态 + 递减 + 试触发
 *
 * 由 ftl_host_io_callback 在子操作簿记后调用（caller 通知字段的唯一写点）。
 * sub_status != OK 时粘进 agg_status；outstanding-- 后试发组完成通知。
 *
 * @param table        pending 表
 * @param gi         组下标
 * @param sub_status 本子操作的完成状态（SSD_STATUS_OK / IO_ERROR）
 */
void pend_group_complete_subop(pending_ctx_t *table, WORD gi, ssd_status_e sub_status);

/**
 * @brief 一个子操作完成、归属**多个组**：逐组聚合
 *
 * 页打包写缓冲下一页可能由多个组拼成（一页 = 一次 FC 命令 = 一个 pending 槽），
 * 完成时须对**每个**贡献组各递减一次在飞数（每个组对该窗口至多贡献一次 append ⇒
 * 递增/递减数恒等）。读路径传单组位图（1<<gi），与 pend_group_complete_subop 等价。
 *
 * @param table        pending 表
 * @param owners     贡献组位掩码（bit gi）
 * @param sub_status 本子操作的完成状态（SSD_STATUS_OK / IO_ERROR）
 */
void pend_group_complete_subop_multi(pending_ctx_t *table, DWORD owners, ssd_status_e sub_status);

/**
 * @brief 若组已全部下发且全部完成则发通知并释放槽
 *
 * 条件：used 且 next==total 且 outstanding==0。满足则写 status_out/complete_flag、
 * 调 on_complete（**最后**才 used=0，因回调可能重入 submit 再申请组）。
 *
 * @param table pending 表
 * @param gi    组下标
 */
void pend_group_maybe_complete(pending_ctx_t *table, WORD gi);

/**
 * @brief 是否还有写组的下一个 LBA 落在指定 pair 窗口
 *
 * 供 wbuf 的空闲 flush 判据使用；扫描 pending 表内的 groups[]，不暴露数组本身。
 *
 * @param table  pending 表
 * @param window pair 窗口基址
 * @return true 有写组的 next 落在本窗口
 */
bool pend_group_has_window_pending(const pending_ctx_t *table, DWORD window);

/* ── write-confirm park（docs §5「Confirm → pending 服务」）──
 *
 * 待入账项就存在 pending 表里（op = WRITE_CONFIRM、command_id 已脱钩为哨兵），
 * 不另建第二张列表（C9）。本模块只提供表原语（遍历 / 判超时 / 结算）；
 * 数据面事务（Journal → L2P → BST ±）由调用者的写确认服务直调执行，
 * 避免把数据面事务复制进 pending 模块（C2/R9）。
 *
 * 调用者按事务结果选择结算状态：
 *   FTL_OK    —— 事务已应用；以 SSD_STATUS_OK 结算；
 *   FTL_AGAIN —— region 未就绪；未到期保留待入账，到期后以 IO_ERROR 结算；
 *   FTL_ERROR —— 永久失败；以 IO_ERROR 结算。
 */

/**
 * @brief 把一个已成功编程、但确认尚未完成的写子操作登记进待入账列表。
 *
 * @param table       pending 表。
 * @param pending_idx 该子操作的 pending 槽下标。
 * @param owners      贡献本子操作的组位掩码。
 */
void pend_confirm_park(pending_ctx_t *table, WORD pending_idx, DWORD owners);

/**
 * @brief 从 cursor 起取下一个待入账槽（只读遍历，不改任何状态）。
 *
 * @param table      pending 表。
 * @param cursor     [in,out] 扫描起点；命中后推进到命中项之后，扫完置 FTL_PENDING_MAX。
 * @param out_index  命中的槽下标。
 * @return true 命中一个待入账槽；false 本轮扫描结束。
 */
bool pend_confirm_find_next(const pending_ctx_t *table, BYTE *cursor, BYTE *out_index);

/**
 * @brief 待入账项是否已超过确认 deadline。
 *
 * @param pending 待入账槽。
 * @param now_ms  当前时刻（ms）。
 * @return true 已超时；
 *         false 未超时，或尚未起算（park_ms == 0）。
 */
bool pend_confirm_is_expired(const pend_req_t *pending, DWORD now_ms);

/**
 * @brief 结算一个待入账槽：取 owners、释放槽、按 status 聚合完成组。
 *
 * @param table  pending 表。
 * @param index  待入账槽下标（必须仍处于待入账态）。
 * @param status 本次结算的完成状态（SSD_STATUS_OK / SSD_STATUS_IO_ERROR）。
 *
 * @note 顺序不变量在本函数内维护：owners 必须在释放**之前**取走 —— 组完成通知
 *       可能重入提交并复用本槽。
 */
void pend_confirm_settle(pending_ctx_t *table, BYTE index, ssd_status_e status);

/**
 * @brief 当前待入账项数（日志用：必须报「同时有多少条在等」）。
 *
 * @param table pending 表。
 * @return 待入账项数。
 */
WORD pend_confirm_get_count(const pending_ctx_t *table);

#endif /* FTL_PENDING_H */
