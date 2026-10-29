#ifndef FTL_BBT_H
#define FTL_BBT_H

/*
 * BBT（Bad Block Table）— 坏块表持久化
 *
 * 三个阶段：
 *   1. 出厂扫描：遍历所有 Block，读第 0 页判断坏块标记
 *   2. 运行时隔离：PROGRAM/ERASE 失败 → BBT 标记 + allocator 跳过
 *   3. 替换机制：Block Sparing — 从 Spare Pool 取好块补充 Free Pool
 *
 * BBT 存储在固定元数据区的 BBT 池（`BBT_POOL_BLOCKS` 块轮转，
 * **一条 record = 1 页**，块满换池内下一块并先擦）——池首块/块数由
 * ftl/metadata.h 按几何推导（「固定元数据区布局」）。
 * bad_map 覆盖全部 FTL_BLOCK_TOTAL 个 Block（NAND 持久化格式，保持不变）。
 * RAM 运行时坏块位统一存于 bst_block_t.state（Block State Table 单数据源），
 * bbt_save 时从 BST 重建 bad_map。
 *
 * 写模型与 Anchor / Checkpoint 一致（见 ftl/metadata.h §元数据写模型）：
 * **只追加、环形轮转、最新有效胜**。对同一份 record 就地重写会在第二个坏块出现时
 * 损坏 BBT ⇒ bbt_load 失败 ⇒ 开机不知道哪些块坏 ⇒ 用到坏块丢数据。
 */

#include "allocator.h"
#include "assertion.h"
#include "ftl_module.h" /* bg_info_t / ftl_mod_status_t */
#include "metadata.h"   /* BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS / BBT_BITMAP_SIZE */

#include <stddef.h>
#include <stdint.h>

/* ── BBT 尺寸与块位置（BBT_BLOCK / BBT_POOL_BLOCKS / BBT_MAX_BLOCKS /
 *    BBT_BITMAP_SIZE）见 ftl/metadata.h「固定元数据区布局」── */

/* ── BBT Header 格式 ── */
#define BBT_MAGIC (0x424254ABUL) /* "BBT\xAB" */

/* reserved[0] 的格式进行中标志：两阶段格式化在擦除用户块之前先落 BBT 时置位，
 * 擦完并写完 CP 标记后清除。旧固件写 0（不进行中），语义兼容。 */
#define BBT_FLAG_FORMAT_IN_PROGRESS (1UL)

/* reserved[1] 的「下次开机必须 full metadata rebuild」标志（本仓自拟）。
 * Journal / L2P 日志块运行期变坏时置位：丢失的映射无法在当前上电周期内恢复，
 * 只能靠全量重建数据页 META 兜底。成功重建后清除。 */
#define BBT_FLAG_REBUILD_REQUIRED (1UL)

typedef struct
{
    DWORD magic;                        /* BBT_MAGIC */
    DWORD sequence;                     /* 单调递增，最新有效 */
    DWORD total_blocks;                 /* 总 Block 数 */
    DWORD bad_count;                    /* 坏块数 */
    DWORD reserved[3];                  /* [0] = FORMAT_IN_PROGRESS；[1] = REBUILD_REQUIRED；其余写 0 */
    BYTE  bad_map[BBT_BITMAP_SIZE];     /* 4096 bit = 512B */
} __attribute__((packed)) bbt_header_t; /* 4+4+4+4+12+512 = 540B（无独立 CRC，页完整性由 FC 保证）*/

_Static_assert(sizeof(bbt_header_t) == 540U, "BBT header 布局漂移（期望 540B）");
_Static_assert(sizeof(bbt_header_t) <= PAGE_MAIN_SIZE, "BBT header 必须装得进一页主区");

/* ── BBT 模块 handle 的上报结构（docs/archive/ftl_module_contracts.md §4.1）──
 *
 * `base` 是公共前缀（state / critical / wants_run / progress / next_ms / error）。
 * BBT 只做提交背压，**无周期** ⇒ next_ms 恒 0。
 */
typedef struct
{
    ftl_mod_status_t base;

    bbt_handle_phase_e phase;     /* 观测；外部不要据此驱动 */
    bool               dirty;     /* 是否有未落盘变更 */
    DWORD              dirty_gen; /* dirty 代际（写飞行期间的新变更） */
    DWORD              sequence;  /* 本池单调序号 */
    WORD               write_idx; /* 池内下一个待写槽 */
    WORD               bad_count; /* 当前坏块数 */
} bbt_status_t;

/* ── bitmap / 候选块原语 ── */

/**
 * @brief 查询 Block 是否坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 * @return true 坏块，false 好块
 */
static inline bool bbt_is_bad(const BYTE *bad_map, WORD block)
{
    ASSERT(block < BBT_MAX_BLOCKS);

    if (block >= BBT_MAX_BLOCKS)
    {
        return true; /* 越界是编程错误：release 下保守判坏，避免把非法块当好块使用 */
    }
    if (bad_map == NULL)
    {
        return false;
    }
    return ((bad_map[block >> 3] >> (block & 7U)) & 1U) != 0U;
}

/**
 * @brief 在 bitmap 中将 Block 标记为坏块
 *
 * @param bad_map  BBT bitmap
 * @param block    Block 编号
 */
static inline void bbt_set_bad(BYTE *bad_map, WORD block)
{
    if ((bad_map != NULL) && (block < BBT_MAX_BLOCKS))
    {
        bad_map[block >> 3] |= (1U << (block & 7U));
    }
}

/**
 * @brief 该物理块是否属于当前 BBT 候选池
 *
 * BBT record 的候选池优先来自 `pools->bbt` 的实时候选表（可能含
 * shared metadata spare）；ring 未建立或不足 2 块时才退回名义区间
 * `[BBT_BLOCK, BBT_BLOCK + BBT_POOL_BLOCKS)`。
 *
 * 纯函数；格式化/重建路径按块调用，放头文件供 bbt.c 与 PFR 共用，避免三份手写
 * 条件在后续修改时漂移。
 */
static inline bool bbt_block_is_candidate(const md_ring_t *ring, WORD block)
{
    if ((ring != NULL) && (ring->block_count >= 2U))
    {
        return md_ring_has_block(ring, block);
    }

    return (block >= BBT_BLOCK) && (block < (BBT_BLOCK + BBT_POOL_BLOCKS));
}

/* ── 接口 ── */

/**
 * @brief BBT 初始化
 *
 * 从 NAND BBT 池加载 BBT，坏块位写入 alloc->blocks[].state。
 *
 * 无有效 BBT 时**不无条件出厂扫描**：
 *   · 介质是空片（固定元数据区没有一页是合法 FTL 页）⇒ 执行出厂扫描；
 *   · 介质已被格式化过 ⇒ **拒绝扫描** + LOGE，坏块表从空开始（bad_count = 0、
 *     sequence 保持 0、不置 dirty ⇒ 不落盘）。真坏块由运行时 bbt_mark_bad 兜底。
 *
 * ⚠ 为什么：用过的块 Page 0 的 spare 首字节是 FTL 自己的数据（页编程 2112B 把 CW3
 *   尾巴 64 字节溢进备区），不是出厂标记 ⇒ 重扫会把所有写过的块（含活的 Anchor/
 *   Checkpoint/Wear）判成坏块并 bbt_save 落盘。详见 bbt.c 的 bbt_medium_has_ftl_data。
 *
 * @param alloc  Allocator 上下文（blocks、bad_count 等输出）；**前置条件**（调用者传 &ftl->alloc）
 * @note 加载失败不是致命错误：走“拒绝重扫（已用过介质）”或“出厂扫描”回退；
 *       坏块数只走 `alloc->bad_count`（H38：恒为常量的返回码改 void）。
 */
void bbt_init(alc_t *alloc);

/**
 * @brief 介质固定元数据区里是否已有合法的 FTL 页
 *
 * 只读。用于两处判空：
 *   · `bbt_init`：无有效 BBT 时，只有空片才允许出厂扫描；
 *   · `cp_media_status_probe`：CP 池没有可定位 record 时，区分“空片”与
 *     “有元数据但 CP 缺失/损坏”。调用者应先用 raw 读确认命令通路正常，
 *     否则“总线读死”也会返回 false。
 *
 * @param alloc Allocator 上下文（取 ch/ce/lun 与几何）；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @return true = 至少一页是合法 FTL 页（介质已格式化过）
 *         false = 空片
 */
bool bbt_medium_has_ftl_data(alc_t *alloc);

/**
 * @brief 出厂坏块扫描
 *
 * 遍历所有 Block（**跳过 Block 0**）的 Page 0，读 **spare 区首字节**（列 2048，
 * `OP_READ_COLUMN`）的**出厂坏块标记**：
 *   - 标记非 0xFF → 出厂坏块
 *   - 标记 == 0xFF → 好块
 *   - 列读失败 → 坏块（保守）
 *
 * 判据与器件手册的建 BBT 流程一致（Micron AN1819：出厂标记在每块首页的 spare 区，
 * **擦除即永久丢失**，故必须在使用该块前先读）——见 `bbt.c:bbt_probe_block_bad` 的引文。
 * 本函数**只读不擦**（do_erase=0），擦除会毁掉标记。
 *
 * ⚠ **前置条件：介质必须是空片**。用过的块 spare 首字节是 FTL 自己的数据（页编程
 *   2112B 把 CW3 尾巴 64 字节溢进备区）⇒ 扫出来的不是坏块图，而是「哪些块被写过」图，
 *   且会 bbt_save 落盘。`bbt_init` 是唯一调用者，它用 bbt_medium_has_ftl_data 把关；
 *   其他调用方必须自己保证这一点。
 *
 * @param alloc   Allocator 上下文（blocks 输出）
 * @return 检测到的坏块数
 */
int bbt_factory_scan(alc_t *alloc);

/**
 * @brief 格式化用的两阶段擦除：先落 BBT，再擦 BBT 池之外的块
 *
 * ① `bbt_dirty_note + bbt_save`：把当前 BST 坏块图（含工厂标记 / CP 快照并集）
 *    追加成一条 record，并置 `BBT_FLAG_FORMAT_IN_PROGRESS`。失败 ⇒ FTL_ERROR，
 *    **不擦任何用户块**（工厂标记仍完整）。
 * ② 擦除**除 BBT 池之外**的全部块（所有 plane）；擦失败的块标 BAD。
 * ③ 清 `FORMAT_IN_PROGRESS`，再次 `bbt_save` 把新 BAD 并入（best-effort；
 *    失败则 dirty 留给后台 bbt_handle；工厂坏块图已在 ① 落盘）。
 *
 * ⚠ **不读出厂坏块标记**：标记的读取职责唯一归 `bbt_init`（且只在空片时做）；
 *   因此「必须在 bbt_init 之后调用」是承重的。
 * ⚠ 掉电语义：① 成功后任何时候掉电，新 BBT record 都保留；② 中途掉电时
 *   `FORMAT_IN_PROGRESS` 让下次开机识别“格式化未完成”并继续格式化，而不是把
 *   “有 BBT、无 CP”误判成损坏介质。
 *
 * @param alloc  Allocator 上下文；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @return FTL_OK；FTL_ERROR = ① 的 BBT save 失败（未擦用户块，可重试）。
 */
ftl_status_e bbt_format_erase(alc_t *alloc);

/**
 * @brief 运行时标记坏块
 *
 * ① bst_block_t.state 置 BAD + 清有效页位图（BST 单数据源）
 * ② 只置 alloc->bbt.dirty —— 写回 NAND 由后台 bbt_handle 攒批执行
 *
 * @param alloc  Allocator 上下文
 * @param gblock 坏 Block 编号（全局 Block 号）
 * @return FTL_OK（含“已经是 BAD”的幂等早退）；FTL_ERROR 参数非法 / 越界。
 *         ⚠ 已 BAD 时**不重复** `bad_count++`、不重复消耗 Spare、不重复置 dirty。
 */
ftl_status_e bbt_mark_bad(alc_t *alloc, WORD gblock);

/**
 * @brief 标记 BBT 有未落盘变更（唯一 dirty 闸门）。
 * 每次置位推进 `dirty_gen`；写 job 用它检测“组 header 之后又标坏”的丢更新。
 */
void bbt_dirty_note(alc_t *alloc);

/**
 * @brief 将 BBT 追加写入 NAND BBT 池
 *
 * 池内下一槽追加一条 record（块满先擦池内下一块）。sequence 递增。
 *
 * @param alloc  Allocator 上下文（游标/序号在 alloc->bbt，per-instance）；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @return FTL_OK 成功（含 dirty==0 早退）；FTL_ERROR 池定位 / prepare / 写失败。
 */
ftl_status_e bbt_save(alc_t *alloc);

/**
 * @brief 从 NAND BBT 池加载 BBT
 *
 * 池内轮转扫描（md_ring_locate），选 magic 正确且 sequence 最大者。
 * 命中时同时恢复本实例的 sequence 计数与写游标（alloc->bbt）。
 *
 * @param alloc       Allocator 上下文（池路由取其 ch/ce/lun；游标/序号写回 alloc->bbt）；
 *                    alloc 由 ftl_contract_audit 证明；出参由调用者提供
 * @param bad_map     输出：bt_bitmap[512]
 * @param bad_count   输出：坏块数
 * @param sequence    输出：最新 sequence
 * @return FTL_OK 成功，FTL_ERROR 无有效 BBT
 */
ftl_status_e bbt_load(alc_t *alloc, BYTE *bad_map, DWORD *bad_count, DWORD *sequence);

/**
 * @brief BBT 后台持久化入口（typed handle；提交即返回）
 *
 * 当 alloc->bbt.dirty 置位时，装配一个 BBT job 并提交第一条命令：
 *   换块需擦 ⇒ 异步擦（phase = BBT_JOB_ERASE）→ 擦完成再起写；
 *   否则直接异步写（phase = BBT_JOB_WRITE）。
 * 成败由 g_op_callbacks[FTL_OP_BBT]（ftl_bbt_callback）收尾：写完成后推进池游标、
 * 成功清 dirty、失败保留 dirty 供下轮重试。
 *
 * `info` 当前不参与决策（BBT 是 dirty 驱动、无周期），保留参数是为了统一 typed 入口
 * （docs/archive/ftl_module_contracts.md §3.3）。
 *
 * ⚠ 本函数只保证「已起步」；成败判据 = job 走完且 dirty 清零（同步路径见 bbt_save）。
 * ⚠ **单飞行**：命令在飞 ⇒ 空操作（池内一条 record 一次写，重入会争同一槽）；
 *   若上一步提交被背压拒（job 停在 WRITE 但无命令在飞），本函数只**续提交写**，
 *   不重走换块判定——擦已成功，重擦纯属磨损。
 * ⚠ 只由后台调度器/B 档调用。开机/格式化路径仍走同步 bbt_save。
 *
 * @param alloc  Allocator 上下文；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param info   本轮动态信息（当前未用，可传 NULL）
 * @param out    出参：本轮结束后的上报（**非空**，由调用者提供）
 */
void bbt_handle(alc_t *alloc, const bg_info_t *info, bbt_status_t *out);

/**
 * @brief 从**指定 plane 的** Spare Pool 补充 Free Pool
 *
 * PROGRAM/ERASE 失败标记坏块后，从 Spare Pool 取一个好块
 * 加入 alloc->free_blocks 保容量水位。
 *
 * ⚠ **按 plane 取**（本次修复点）：Spare 池按 super block 粒度预留（每 plane 各一份），
 *   坏块属于哪个 plane 就必须补哪个 plane 的块——补错 plane 不恢复该 plane 的配对能力。
 *
 * @param alloc  Allocator 上下文
 * @param plane  坏块所属 plane（取同 plane 的替换块）
 * @return FTL_OK 补充成功；FTL_ERROR 参数非法 / 该 plane Spare Pool 耗尽（容量自然下降）
 */
ftl_status_e bbt_spare_refill(alc_t *alloc, BYTE plane);

/**
 * @brief 逐块探测并打印坏块图（**诊断用**）
 *
 * 默认**只读**：对每个块读 Page 0 的 spare 首字节（**列 2048** = `PAGE_MAIN_SIZE`），
 * 非 0xFF ⇒ 出厂坏块。不写介质，可反复调用。
 *
 * ⚠ do_erase != 0 时**额外擦除本实例每个 plane 的每一个块**（含
 *   0..FTL_METADATA_AREA_END 的元数据区）⇒ 会抹掉该实例的全部 FTL 元数据与用户数据。
 *   只在专门的坏块体检时用，绝不可放进开机路径。
 *   擦除是**无条件**的：即使 ① 已判定该块坏，也照擦（"全片擦除"必须连坏块一起抹掉，
 *   否则元数据块被跳过 ⇒ 擦完 FTL 仍读回旧状态）。
 *
 * 判定（两条任一成立即坏块）：
 *   ① **擦除前**读该块 Page 0 列 2048 的 spare 首字节，非 0xFF（出厂坏块标记，x8 为 00h）；
 *   ② do_erase 非 0 且擦除命令失败（stuck-at-0 / 不可擦）。
 *
 * ① 必须**先于任何 PROGRAM/ERASE** 读——器件手册（Micron AN1819）逐字：
 *   *"the bad block information is erasable and cannot be recovered once erased"*。
 *   必须先读后擦；列 2048 由 ftl_sync_read_column()（OP_READ_COLUMN）够到。
 *
 * 打印（每排 BBT_MAP_COLS_PER_PLANE 个块，各 plane 一组、组间留空；每格定宽 4）：
 *     CH=0  CE=0  LUN=0
 *                      PLANE=0                          PLANE=1
 *       FF   FF 2047   FF   FF   FF   FF   FF      FF   FF   FF   FF   FF   FF   FF   FF
 *   好块打 `FF`，坏块打**本 plane 的绝对块号**（0..BLOCKS_PER_PLANE-1，十进制）。
 *   ⚠ 不是排内列号——列号每排重新计数（0..15），读图的人得自己算 `排号×列数+列号`，
 *     且 1~2 位的数字与 `FF` 并排极易被误读成十六进制字节值。
 *
 * ⚠ 本片芯片若已被整片擦过，出厂标记即已丢失 ⇒ 只会读到全 0xFF（不是 bug）。
 *
 * @param alloc     Allocator 上下文（取 ch/ce/lun 与块几何）；**前置条件**（实例绑定由 ftl_contract_audit 证明）
 * @param do_erase  0 = 只读（非破坏性）；非 0 = 逐块擦除并额外判擦除失败
 */
void bbt_print_bad_map(alc_t *alloc, BYTE do_erase);

#endif /* FTL_BBT_H */