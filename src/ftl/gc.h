#ifndef FTL_GC_H
#define FTL_GC_H

/*
 * Garbage Collection
 *
 * 以 Super Block 为粒度回收已写满的 Block。
 *
 * 流程：
 *   ① 从 closed_sb_list 选 victim（多维加权评分：有效页 / 磨损 / 温度 / 时间）
 *   ② 逐 Plane 逐页读 victim Super Block
 *   ③ 读 META 获取 LBA → 查 L2P → 判断页是否有效
 *   ④ 有效页 → 搬移到新的 Cold Super Block
 *   ⑤ 擦除各 Plane Block → 归还到 Free Pool
 *
 * ⚠ ②~⑤ 的执行体**不在本文件**：搬移是一个异步 job，GC 与 WL 共用同一台引擎，
 *   见 `ftl/migrate.h` / `migrate.c`（mig_handle_s + mig_gc_begin / mig_wl_begin /
 *   mig_handle + ftl_migrate_op_callback）。本文件只留**选 victim 与共用原语**：
 *   · gc_select_victim   —— 四项加权评分（GC 侧选块策略）
 *   · gc_decode_cw_lba   —— 已完成读命令的逐 CW META 解码（异步搬移 + 掉电重建共用）
 *   · gc_read_page_with_lba —— 同步读 + 解码（只服务掉电重建）
 *   · gc_trigger_mask  —— 触发判定
 *
 * 原子性：
 *   Journal 记录每一条 L2P 变更
 *   掉电时最多遗留"孤悬"Block，下次 GC 回收
 */

#include "allocator.h"    /* alc_s, super_block_s */
#include "fc_interface.h" /* fc_cmd_s（gc_decode_cw_lba 的形参） */
#include "ftl_config.h"   /* DWORD */
#include "ftl_type.h"     /* DWORD */
#include "nand_config.h"  /* PLANE_NUM, PAGES_PER_BLOCK */

#include <stdint.h>

/* ── GC 触发位（设计 §4.4：**空间压力**两条；磨损维护不在 GC 触发里）──
 * gc_trigger_mask() 返回的**位掩码**：命中的位即触发起因（可多位同时命中）。
 * 只用于调用点打日志/调参，"要不要 GC" 的判定仍是 `!= 0`。
 *
 * ⚠ 位值是对外观测契约（`[GC  ] trig=` 日志 / 测试），**不得重排**；0x02 / 0x04 / 0x20
 *   不用但缺口保留，避免旧日志/脚本按位值解读时错位。触发判据的出处：SPDK 的
 *   `ftl_needs_reloc`（自由 band 水位）与 nvcache 的 free-chunk 阈值；DRAM-less 侧
 *   （DFTL / LeaFTL / VPFTL）与 dhara 同样只认**空间压力**。定时 / 写入量 / 碎片率
 *   三类触发在参考层查无出处，且碎片率会自激。 */
#define GC_TRIGGER_WATERMARK (0x01U) /* T1: free_count < FREE_POOL_TARGET */
#define GC_TRIGGER_EMERGENCY (0x08U) /* T4: free_count <= FREE_POOL_EMERGENCY */
/* 磨损均衡不再作为 GC 触发位：静态 WL 由 wear_leveling 的 UBI 判据
 * （max_free_ec − min_used_ec ≥ WL_THRESHOLD）在 host 空闲时独立启动。
 * 旧 T5 用“全表 max/min”比较，min 可能来自从未使用的空闲块 ⇒ 比值永不收敛。 */

/* ── GC 触发判定的求值节拍（由 migrate handle 自持）──
 * 判据是**事件**（空间压力）；求值本身只读 free 计数，触发后的 victim 评分扫 closed 列表，
 * 故按节拍求值而不是主循环每轮跑。 */
#define GC_EVAL_INTERVAL_MS (10U)

/* GC 会话收尾的静默窗口：连续这么久没有 GC job 活动才认为本次会话结束。
 * 2 s 足以跨过单 victim 的正常间隙，又不会让一次短暂写入把会话拖成永远不结束。 */
#define GC_SESSION_IDLE_MS (2000U)

/* 无收益结论后的 closed-list 复扫间隔（migrate 消费）。
 * 有新建写 / 进入紧急水位 / 时钟缺失时立即重扫；5000 ms 只用于「无写入、状态未变」的空闲尾，
 * 避免满盘时每 500 ms 全表扫 closed list（板端实测约 2 次/s、closed≈1695）。
 * 写路径需要空间时先走 OP 兜底，program 完成会改变 total_writes ⇒ 下一轮立即重扫。 */
#define GC_NOGAIN_BACKOFF_MS (5000U)

/* host 写流判定窗口（migrate 消费）：total_writes 变化后这么久内视为 host 在写。 */
#define GC_HOST_WRITE_WINDOW_MS (2000U)

/* ── 类型 ── */

/* 逐 CW META 解码结果。 */
typedef enum GC_DECODE_E
{
    GC_DECODE_OK = 0,
    GC_DECODE_EMPTY,
    GC_DECODE_CORRUPT,
} gc_decode_e;

/* GC 收益判据模式（只对空间 GC T1/T4 有意义；磨损均衡走独立 WL 路径）。
 * 具体阈值是 gc.c 的内部策略参数（IDLE 80% / EMERGENCY 1%）。 */
typedef enum GC_BENEFIT_MODE_E
{
    GC_BENEFIT_OFF = 0,   /* 不筛：host 正在写（原水位行为） */
    GC_BENEFIT_IDLE,      /* host 不写：只接受 valid 占比不高的 victim */
    GC_BENEFIT_EMERGENCY, /* 紧急水位：只接受可回收占比足够的 victim */
} gc_benefit_mode_e;

/* 选块结果：把「没有候选」与「有候选但都不值得搬」分开，后者用于 no-gain 退避与
 * NO_SPACE 判定（``）。 */
typedef enum GC_SELECT_E
{
    GC_SELECT_OK = 0,         /* 已选中，out->victim 有效 */
    GC_SELECT_NO_CANDIDATE,   /* closed 列表里没有可用候选（空 / 全 open / 全哨兵） */
    GC_SELECT_NOT_WORTHWHILE, /* 有候选，但全部被收益判据拒绝 */
} gc_select_e;

/* 选块出参（即使 NOT_WORTHWHILE 也填 best_valid_pct，供日志归因）。 */
typedef struct GC_SELECT_OUT_S
{
    const super_block_s *victim;         /* GC_SELECT_OK 时有效 */
    WORD                 best_valid_pct; /* 域内最低有效率（0..100），无候选时为 0 */
    WORD                 scanned;        /* 参与评分的候选数 */
    WORD                 eligible;       /* 通过收益判据的候选数 */
} gc_select_out_s;

/* ── 接口函数 ── */

/**
 * @brief 读取一页 NAND，取回逐 CW 的扇区 LBA（同步，内部管理命令生命周期）。
 *
 * @note 读整页只为取回页内 META，**不向调用者拷用户数据**；未写过的 CW 填
 *       `FTL_LBA_INVALID`；全页无写过 CW ⇒ FTL_ERROR（调用者跳过）。
 * @note 唯一调用者是掉电重建的同步路径；搬移读走异步路径并用 `gc_decode_cw_lba`
 *       收尾。
 */
ftl_status_e gc_read_page_with_lba(DWORD pba, DWORD cw_lba[CW_NUM]);

/**
 * @brief 从已完成的读命令的 desc 链解出逐 CW 的扇区 LBA + 页基址。
 *
 * @note **必须在 fc_command_release 之前调用**（meta 指针指向命令的 desc 链）。
 * @note 返回：GC_DECODE_OK = 至少一个 CW 写过且页基址一致；GC_DECODE_EMPTY = 全页
 *       未写过（调用者跳过）；GC_DECODE_CORRUPT = 页基址不一致（META 损坏，调用者
 *       必须按不可迁移处理，**不得**擦 victim）。
 */
gc_decode_e gc_decode_cw_lba(fc_cmd_s *cmd, DWORD pba, DWORD cw_lba[CW_NUM], DWORD *page_base_out);

/**
 * @brief 从 closed_sb_list 选择一个 victim Super Block（cost 评分 + 收益判据）。
 *
 * @note 收益判据在 cost 评分**之前**逐候选过滤：不能先选 cost 最小再检查收益，
 *       否则可能因最差块仍太有效而报无收益，而其实存在更稀疏的候选。
 * @note 权重 / 符号约定见 gc.c 的内部策略注释；cost 相等取 valid_count 更小者；
 *       单 plane 哨兵条目跳过，全哨兵条目直接跳过。
 */
gc_select_e gc_select_victim(const alc_s *alloc, gc_benefit_mode_e mode, gc_select_out_s *out);

/**
 * @brief 检查是否需要触发 GC，并返回触发起因位掩码。
 *
 * @note T1（主动补池）与 T4（最后手段）同源但阈值不同；磨损均衡不归 GC 触发。
 * @note 只读 free 计数（O(PLANE_NUM)），不做全表扫描；victim 评分在
 *       `gc_select_victim` 里。唯一求值点是 `mig_gc_benefit_eval`，BUSY 档不求值。
 */
DWORD gc_trigger_mask(const alc_s *alloc);

#endif /* FTL_GC_H */