#ifndef FAULT_INJECT_H
#define FAULT_INJECT_H

#include "ssd_types.h"

/* ── 测试镜像专属故障注入（分层负例框架）──
 *
 * 目的：让「负例」验证不需要真 NAND 失败也能重复跑。按故障类型建一张表，
 * 每个故障可「按次数 + 按 key（plane / region / page LBA / opcode / phase）」定点注入。
 *
 * 契约：
 *   · **只在测试镜像里存在**：开关是 `FC_NAND_TEST_ENABLE`（只有 `make tests` 定义），
 *     **不是** `NDEBUG`。产品固件不定义该宏 ⇒ 本头不参与生产构建。
 *   · 生产模块**不新增**引用：FC/FTL 的故障由 UT 构建的链接期 wrap 库注入；
 *     只有无法 wrap 的静态缝才允许 hook，且 include 与调用点必须用
 *     `#if defined(FC_NAND_TEST_ENABLE)` 整体包住（不允许无条件调用空实现）。
 *   · 按**数量**注入，不是布尔开关：每次命中自减；0 = 不注入。
 *   · `fi_arm(f, n)` 对任意 key 生效；`fi_arm_at(f, key, n)` 只对匹配 key 生效；
 *     `fi_take` 只匹配无 key 的武装，`fi_take_at` 匹配「无 key 或同 key」。
 *   · 状态是文件级静态量，**测试 setup/teardown 必须 `fi_reset()`**。
 *
 * 设计参照（仅借鉴公开工具的形状；实现为**本仓自拟**）：
 *   · libfiu：故障点 + 控制 API + 次数；
 *   · QEMU blkdebug：event / 地址 / once 三个维度；
 *   · dm-flakey / dm-dust / nandbiterrs：永久 / 瞬态 / 坏区 / 位翻转的分类。
 *
 * 设计文档：docs/design/negative_test_framework.md
 */

/* key 的「任意」哨兵。真实 key（plane / phase / opcode / region / LBA）远小于该值。 */
#define FI_KEY_ANY (UINT32_MAX)

/* 第二 key 未设置的哨兵（与 FI_KEY_ANY 区分：ANY 会匹配所有现场）。 */
#define FI_KEY_NONE (UINT32_MAX - 1U)

/* 命名构造函数：坐标域由故障类型绑定（C11），测试侧用这些宏表达意图。 */
#define FI_KEY_PLANE(p)  ((DWORD)(p))
#define FI_KEY_REGION(r) ((DWORD)(r))
#define FI_KEY_LBA(l)    ((DWORD)(l))
#define FI_KEY_OP(o)     ((DWORD)(o))
#define FI_KEY_PHASE(p)  ((DWORD)(p))

typedef enum FI_FAULT_E
{
    FI_CACHE_FAIL = 0,        /* cache program 完成：伪造 FAIL_CACHE（key = 页基址 LBA） */
    FI_JOURNAL_WRITE_FAIL,    /* Journal 页写失败（key 未用） */
    FI_FC_STATUS,             /* FC 完成状态位注入（key = NAND opcode，arg = status 位） */
    FI_ALLOC_FAIL,            /* 分配失败，模拟 free/OP 都拿不到块（key 未用，按次数） */
    FI_L2P_LOAD_FAIL,         /* L2P region 加载启动失败（key 未用，按 META 读次数） */
    FI_CP_ABORT_PHASE,        /* 指定 CP phase 中止（key = cp_phase_e） */
    FI_BBT_WRITE_FAIL,        /* BBT record 写失败（key 未用） */
    FI_MIG_READ_FAIL,         /* GC/WL victim 页读失败（key = plane） */
    FI_MIG_WRITE_FAIL,        /* GC/WL 目的页写失败（key = plane） */
    FI_MIG_ERASE_FAIL,        /* GC/WL victim 擦除失败（key = plane） */
    FI_RR_READ_FAIL,          /* Read Recovery 恢复读硬失败（key = plane） */
    FI_SEQ_GATE_STARVE,       /* 序列门控拒绝，模拟后台被饿死（key 未用） */
    FI_FC_STATUS_DATA,        /* 仅 DATA 命令的 FC 完成状态注入（key = opcode 或 FI_KEY_ANY） */
    FI_FC_STATUS_PLANE,       /* 按 plane 的 FC 完成状态注入（key = plane，arg = status 位） */
    FI_FC_STATUS_PBA,         /* 按命令目标 PBA 的 FC 完成状态注入（key = PBA，arg = status 位） */
    FI_FC_ENGINE_FAIL,        /* dispatch 执行结果强制失败（key = NAND opcode） */
    FI_COMPLETION_DROP,       /* 丢弃一条完成（key = NAND opcode；消费后不再投递） */
    FI_COMPLETION_LATE,       /* 延迟一条完成到下轮 poll（key = NAND opcode） */
    FI_COMPLETION_HOLD,       /* 扣住完成，测试显式 release（key = NAND opcode） */
    FI_READ_COLUMN_FAIL,      /* ftl_sync_read_column 失败（key = PBA） */
    FI_PLANE_STATUS,          /* dispatch 完成：按 plane 覆盖 cw_status（key = plane，arg = status 位） */
    FI_DATA_PATH_ENCODE_FAIL, /* fc_data_path_encode 强制失败（key = opcode 或 ANY） */
    FI_DATA_PATH_READ_FAIL,   /* fc_data_path_process_read 强制失败（key = opcode 或 ANY） */
    FI_CLOCK_NOW_FAIL,        /* delay_time_now_us 失败（无 key，按次数） */
    FI_JRN_REPLAY_APPLY_FAIL, /* Journal 回放应用遍页读失败（无 key，按次数） */
    FI_ONFI_READ_FAIL,        /* ONFI 参数页读失败（无 key，按次数） */
    FI_NAND_TIMING_FAIL,      /* NAND set_timing 失败（无 key，按次数） */
    FI_CRC_COMPUTE_FAIL,      /* fc_crc_compute 强制返回 0（无 key，按次数） */
    FI_FAULT_NUM
} fi_fault_e;

#if defined(FC_NAND_TEST_ENABLE)

/**
 * @brief 复位全部注入点（测试 setup/teardown 调用）。
 */
void fi_reset(void);

/**
 * @brief 武装一个故障，对任意 key 生效。
 *
 * @param f     故障类型
 * @param count 命中次数（0 = 不注入）
 */
void fi_arm(fi_fault_e f, DWORD count);

/**
 * @brief 武装一个故障，只对指定 key 生效。
 *
 * @param f     故障类型
 * @param key   plane / region / page LBA / opcode / phase（由故障类型决定）
 * @param count 命中次数
 */
void fi_arm_at(fi_fault_e f, DWORD key, DWORD count);

/**
 * @brief 武装一个故障，并携带故障参数（如 FC 状态位）。
 *
 * @param f     故障类型
 * @param key   匹配 key
 * @param count 命中次数
 * @param arg   故障参数（FI_FC_STATUS 时为要 OR 进 status 的位）
 */
void fi_arm_ex(fi_fault_e f, DWORD key, DWORD count, DWORD arg);

/**
 * @brief 武装一个故障，同时匹配两个 key（如 Directory 主/备副本）。
 *
 * @param f       故障类型
 * @param key     第一个匹配 key
 * @param key_alt 第二个匹配 key（FI_KEY_NONE = 不启用）
 * @param count   命中次数
 * @param arg     故障参数
 *
 * @note 每个 fault 仍只有一个槽；需要 3 个以上 key 时应改用 FI_KEY_ANY 或新增故障类型。
 */
void fi_arm_ex2(fi_fault_e f, DWORD key, DWORD key_alt, DWORD count, DWORD arg);

/**
 * @brief 命中判定：只匹配「无 key」的武装。
 *
 * @param f 故障类型
 * @return true = 本次命中且已消费
 */
BOOL fi_take(fi_fault_e f);

/**
 * @brief 命中判定：匹配「无 key 或同 key」的武装。
 *
 * @param f   故障类型
 * @param key 本次现场 key
 * @return true = 本次命中且已消费
 */
BOOL fi_take_at(fi_fault_e f, DWORD key);

/**
 * @brief 命中判定并取出故障参数。
 *
 * @param f        故障类型
 * @param key      本次现场 key
 * @param out_arg  输出：故障参数（可为 NULL）
 * @return true = 本次命中且已消费
 */
BOOL fi_take_ex(fi_fault_e f, DWORD key, DWORD *out_arg);

/**
 * @brief 某故障自上次 `fi_reset()` 以来被消费的次数。
 *
 * @param f 故障类型
 * @return 命中次数；0 = 注入从未到达注入点（用例应据此判失败，而不是只看结果）
 *
 * @note 用于证明「注入确实生效」（C8）：结果错误可能来自别的路径，命中计数是
 *       注入点可达性的直接证据。
 */
DWORD fi_hits(fi_fault_e f);

/**
 * @brief 在指定 cache program 页的命令完成上伪造 `FAIL_CACHE`（一次性）。
 *
 * @param page_lba 目标 cache program 成员的页基址 LBA（8 LBA 对齐）。
 *
 * @note 器件语义：第 N 条命令的 70h 状态报的是**上一页**的结论（op44/op48
 *       映射，op49 首页不映射）。故「让 page0 失败」= 在 **page1** 的完成上注入。
 */
void fi_cache_fail_arm(DWORD page_lba);

/**
 * @brief 命中判定：本页命令完成是否应伪造 `FAIL_CACHE`。
 *
 * @param page_lba 本成员的页基址 LBA。
 * @return true = 本次注入，且注入点已被消费。
 */
BOOL fi_cache_fail_take(DWORD page_lba);

/**
 * @brief 让接下来 @p pages 次 Journal 页写按失败结算。
 *
 * @param pages 失败次数（0 = 不注入）。
 */
void fi_journal_fail_arm(DWORD pages);

/**
 * @brief 命中判定：本次 Journal 页写是否按失败结算。
 *
 * @return true = 本次失败（剩余次数 -1）
 */
BOOL fi_journal_fail_take(void);

#else /* 产品固件：本头不参与生产构建（调用点已被宏裁掉）。 */

/* 仅为编译单元自洽保留最小声明；生产模块不得 include/调用。 */

#endif /* FC_NAND_TEST_ENABLE */

#endif /* FAULT_INJECT_H */
