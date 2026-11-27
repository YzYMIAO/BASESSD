/*
 * bbt.c — 坏块表（BBT）：出厂扫描 + 运行时标坏 + NAND 持久化 + Spare 池替换
 *
 * 职责：
 *   · 开机从 NAND 读回 BBT（无则出厂扫描）；运行时标坏块；
 *   · 把坏块图持久化到 NAND BBT 池（环形轮转）；坏块替换从 Spare Pool 补块；
 *   · 坏块图打印诊断。
 *
 * 关键机制：
 *   · 单一数据源：内存坏块 = BST block.state 的 BAD 位；NAND 持久化用紧凑 bad_map；
 *   · 两条持久化路径刻意并存：`bbt_store` 同步（开机/格式化，superloop 未建、无人 drain CQ），
 *     `bbt_handle` 异步（后台攒批）；两者 record 格式与游标推进规则逐字相同；
 *   · 池游标 / 序号 per-instance（收在 alc_s.bbt）；
 *   · 提交失败收敛：AGAIN 续跑，ERROR 标坏 ring 块并重建候选池。
 *
 * 与其他模块：
 *   · 上游：ftl_init（init）、metadata/pfr（同步 store）、bg 调度器（异步 handle）；
 *   · 下游：metadata_ring（池几何 / 地址）、block BST、Spare 池、fc/FTL 异步命令。
 */

#include "bbt.h"

#include "assertion.h"
#include "fc_interface.h"
#include "ftl_fc_interface.h"
#include "ftl_op.h" /* ftl_op_submit / FTL_OP_BBT */
#include "log.h"
#include "metadata.h" /* metadata_ring_*（BBT 池轮转原语）*/
#include "onfi_1_0.h"

#include <stdio.h>
#include <string.h>

// BBT 内部状态：**per-instance**，落在 alc_s.bbt（bbt_state_s）。
// 多实例（多 LUN）下每实例一份游标/序号，互不覆盖。
//
// 坏块状态存于 bst_block_s.state（Block State Table 单数据源；该字段是互斥枚举，
// BAD 为终态）。NAND 持久化用紧凑 bad_map[512] 格式（bbt_header_s，一字不改），
// bbt_store 时从 BST 重建 bitmap。
/* ── 模块常量 / 类型 / 文件级缓冲 ── */

// Spare/OP 双空日志节流：第 1 次立即打印，此后每 N 次一条（坏块风暴不刷屏）。
// N=64 是本仓观测折中：一次上电最多 64 条同类告警，足以判断趋势又不拖慢 UART。
#define BBT_RESERVE_LOW_LOG_EVERY (64U)

// 用户区抽样点数（每 plane）。见 bbt_medium_has_ftl_data：只在 BBT 载入失败时
// 判「介质是否被写过」，抽样而非全扫以控制开机路径成本。
#define BBT_USER_SAMPLE_COUNT (8U)

// 一排每 plane 的块数。受**终端宽度**约束：每格定宽 BBT_MAP_ENTRY_W，两 plane 并排
// ⇒ 行长 = 2 × (n × (ENTRY_W+1) − 1) + 3，再加 "[INF] " 前缀。取 8 ⇒ 87 字符，
// 在 ~96 列的终端上不折行（取 16 会到 167，必折）。
#define BBT_MAP_COLS_PER_PLANE (8U)

#define BBT_MAP_ENTRY_W (4U) /* 每格固定宽：最大块号 "2047" 也放得下 */

#define BBT_MAP_GROUP_W (BBT_MAP_COLS_PER_PLANE * (BBT_MAP_ENTRY_W + 1U) - 1U) /* 一组宽度：n 个定宽格 + n-1 空格 */

#define BBT_MAP_LINE_MAX (PLANE_NUM * (BBT_MAP_GROUP_W + 3U) + 1U)

// 探针/定位/写点共用的池 + 路由。
typedef struct BBT_PROBE_CONTEXT_S
{
    ftl_topo_s topo;
    md_ring_s  ring;
} bbt_probe_ctx_s;

static DWORD s_reserve_low_logged;

/* 前向声明：store / ring 的提交路径先于 ctx 定义（组环断点，取声明少的方向）。 */
static void bbt_ctx_init(const alc_s *alloc, bbt_probe_ctx_s *ctx);

_Static_assert((BLOCKS_PER_PLANE % BBT_MAP_COLS_PER_PLANE) == 0U, "每 plane 块数须被每排列数整除，否则末排漏块");

/* ── dirty ── */

/**
 * @brief 标记 BBT 有未落盘变更（唯一 dirty 闸门）
 *
 * 每次置位推进 dirty_gen，写 job 用它检测「写飞行期间又标坏」的丢更新。
 *
 * @param alloc Allocator 上下文
 */
void bbt_dirty_note(alc_s *alloc)
{
    alloc->bbt.dirty = true;
    alloc->bbt.dirty_gen++;
}

/* ── spare ── */

/**
 * @brief 从指定 plane 的 Spare Pool 补充 Free Pool
 *
 * 必须按 plane 取：补错 plane 不恢复该 plane 的配对能力。
 *
 * @param alloc Allocator 上下文
 * @param plane 坏块所属 plane
 * @return FTL_OK；FTL_ERROR 参数非法 / Spare 耗尽
 */
ftl_status_e bbt_spare_refill(alc_s *alloc, BYTE plane)
{
    // Spare Pool 坏块替换：出厂时 main.c 已为**每个 plane** 预留
    // FTL_SPARE_POOL_PER_PLANE=50 个好块到 alloc->spare_blocks[plane]。运行时
    // bbt_mark_bad 标坏一块后，从该块所属 plane 取一个好块 alc_free_push
    // 补进 free pool，保用户容量水位。
    // 只出不进，该 plane Spare Pool 耗尽后返回 FTL_ERROR（容量自然下降）。
    if ((alloc == NULL) || (alloc->spare_blocks == NULL) || (plane >= PLANE_NUM))
    {
        return FTL_ERROR;
    }

    WORD gblock;
    if (alc_spare_pop(alloc, plane, &gblock) == false)
    {
        return FTL_ERROR; /* 该 plane Spare Pool 耗尽 */
    }
    alc_free_push(alloc, gblock);

    return FTL_OK;
}

/* ── record ── */

/**
 * @brief BBT record 探针（见 metadata_ring_probe_fn_t）
 *
 * full 与廉价探针等价（record 只有一页）。镜像池：任一 plane 的副本
 * header 有效且 sequence 非 0 即命中。
 *
 * @param ctx          探针上下文（`bbt_probe_ctx_s *`：池 + 路由）
 * @param record_idx   池内槽下标
 * @param full         是否完整探测（本 record 只有一页，忽略）
 * @param out_sequence 输出：命中 record 的 sequence
 * @return FTL_OK 命中；FTL_ERROR 该槽无有效副本
 */
static ftl_status_e bbt_record_probe(void *ctx, WORD record_idx, BOOL full, DWORD *out_sequence)
{
    (void)full;

    bbt_probe_ctx_s *c = (bbt_probe_ctx_s *)ctx;

    // 镜像池：任一 plane 的副本有效即可。
    for (BYTE pi = 0U; pi < md_ring_plane_count(c->ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(c->ring.plane_mask, pi);
        DWORD      addr  = md_ring_addr_plane(&c->ring, &c->topo, record_idx, 0U, plane);

        bbt_header_s hdr;
        if (ftl_sync_read_page(addr, &hdr, sizeof(hdr), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        // record 有效性 = magic + total_blocks。total_blocks 是布局兼容性的代理判据：
        // 换块数 / 换 bitmap 语义后旧 record 必须作废；无本层 CRC，页完整性由 FC 的
        // per-CW CRC + ECC 保证。sequence 从 1 起，0 = 未写。
        if ((hdr.magic != BBT_MAGIC) || (hdr.total_blocks != BBT_MAX_BLOCKS) || (hdr.sequence == 0U))
        {
            continue;
        }

        *out_sequence = hdr.sequence;
        return FTL_OK;
    }

    return FTL_ERROR;
}

/* ── probe ── */

/* ══════════════════════════════════════════════════════════════════
 * 坏块图打印（诊断；默认**只读**，加 --erase 才逐块擦除）
 * ══════════════════════════════════════════════════════════════════ */

/**
 * @brief 探测单个块是否坏块（**全仓唯一的「这块坏不坏」判据**）
 *
 * ① 读 Page 0 的 spare 首字节（**列 2048**，`PAGE_MAIN_SIZE`），非 0xFF ⇒ 出厂坏块；
 * **读失败也判坏块**（列读不通 = 该页不可读，保守归入坏块，与 ② 的擦除失败同调）。
 * **必须在擦除之前读**——器件手册（Micron AN1819）逐字：*"the bad block information is
 * erasable and cannot be recovered once erased"*，且 *"System software should check the
 * first spare area location on the first page of each block prior to performing any
 * PROGRAM or ERASE operations"*。标记**可擦除**、擦后不可恢复 ⇒ 必须先读后擦。
 * ② do_erase == true 时**无条件**擦除该块；擦除失败 ⇒ 坏块（stuck-at-0 / 不可擦，运行时坏块的子集）。
 * ⚠ 擦除**不能**因为 ① 命中就跳过——见下方函数体内的说明。
 *
 * 列 2048 只能靠 OP_READ_COLUMN（列地址走命令 param）够到：OP_READ_META 的列 =
 * cw × CW_SIZE（cw 取自 PBA 的 cw 字段），cw ∈ {0,1,2,3} ⇒ 列只能是 {0, 528, 1056, 1584}。
 * 4 字节 = spare[0..3]（2 字节坏块信息 + 2 字节 user metadata II），标记看 [0]。
 *
 * ⚠ **① 只在空片上才是「出厂标记」**。用过的块，spare 首字节是 FTL 自己的数据
 * （一次页编程写 2112B，CW3 尾巴 64 字节溢进备区）⇒ 本函数在用过的介质上回答的
 * 其实是「这块被写过没有」。故它**不可**用在任何会落盘的路径上，除非调用方已经
 * 确认介质是空片（bbt_medium_has_ftl_data）。
 *
 * 调用者：bbt_factory_scan（出厂扫描，do_erase=false，**调用方已确认空片**）/
 * bbt_print_bad_map（诊断，按 CLI——读图的人须知这不是坏块图）/
 * 注：bbt_format_erase **不**调用本函数（标记读取职责只在 bbt_init）。
 *
 * @param alloc    分配器（取 ch/ce/lun 与几何）。
 * @param plane    plane 号。
 * @param block    plane 内块号。
 * @param do_erase true = 探测时无条件擦除该块（擦除失败 ⇒ 判坏）。
 *
 * @return true = 坏块（列读失败 / 出厂标记非 0xFF / 擦除失败）。
 */
static BOOL bbt_probe_block_bad(alc_s *alloc, BYTE plane, WORD block, BOOL do_erase)
{
    // page / cw 恒 0：擦除不看它们；列地址由 ftl_sync_read_column 显式给，
    // 不依赖 PBA 的 cw 字段。LUN 必须取本分配器实例的坐标（多 LUN 下不能写 0）。
    DWORD addr = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);

    // ① 出厂标记（擦除前读，擦除会永久毁掉它）。读失败 ⇒ 坏块。
    BYTE mark[4];
    memset(mark, 0xFF, sizeof(mark));

    BOOL marked = (ftl_sync_read_column(addr, PAGE_MAIN_SIZE, mark) != FTL_OK) || (mark[0] != 0xFFU);

    // ② 可选擦除——**无条件发**。
    // 擦除不能因为 ① 命中就提前 return：命中可能只是「写过」——元数据区
    // （0..FTL_METADATA_AREA_END）里的 L2P/目录检查点/Checkpoint/BBT/Journal 块正是
    // 列 2048 非 0xFF 的那批。跳过它们 ⇒ 擦完断电重启，FTL 仍从原处读回旧
    // checkpoint/BBT，`free`/`cp_seq` 一个不少。
    if (do_erase == true)
    {
        if (ftl_sync_erase_block(addr) != FTL_OK)
        {
            return true; /* 擦不掉 ⇒ 坏块（stuck-at-0 / 不可擦） */
        }
    }

    return marked; /* 坏不坏仍按**擦除前**读到的出厂标记报（擦后标记已丢，不可再读） */
}

/* ── medium ── */

/**
 * @brief 介质固定元数据区里是否已有合法 FTL 页
 *
 * 只读；只在 bbt_load 失败时调用，用于区分空片与已格式化介质。判空走解码
 * 路径，不能用 RAW 读。调用者应先用 raw 读确认命令通路正常，否则「总线读死」
 * 也会返回 false。
 *
 * @param alloc Allocator 上下文（取 ch/ce/lun 与几何）
 * @return true 已格式化过；false 空片
 */
BOOL bbt_medium_has_ftl_data(alc_s *alloc)
{
    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < FTL_METADATA_AREA_END; block++)
        {
            if (md_block_is_metadata(NULL, plane, block) == false)
            {
                continue; /* 该 plane 的这个块号不是固定元数据（可能是用户数据） */
            }

            const DWORD addr  = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);
            BOOL        blank = false;

            if ((ftl_page_blank_check(addr, &blank) == FTL_OK) && (blank == false))
            {
                return true;
            }
        }
    }

    // 固定元数据区被擦、用户区仍有数据：只查元数据区会把介质误判为空片，
    // 随后的出厂扫描会覆盖已失效的坏块图。抽样用户区 Page 0，
    // 用 `ftl_page_is_blank` 判「是否从未编程」——抽样而不是全扫：本函数只在
    // BBT 载入失败时执行，全扫 4096 块 × 一次 NAND 读在开机路径上不可接受。
    // 残留风险：用户数据恰好全部落在抽样点之外 ⇒ 仍会误判；概率低，且该介质
    // 已丢失元数据，属于灾后场景。
    {
        const WORD start = FTL_METADATA_AREA_END;

        if (start < BLOCKS_PER_PLANE)
        {
            const WORD span = (BLOCKS_PER_PLANE - start);
            WORD       step = (span / BBT_USER_SAMPLE_COUNT);

            if (step == 0U)
            {
                step = 1U;
            }
            for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
            {
                for (WORD block = start; block < BLOCKS_PER_PLANE; block = (block + step))
                {
                    const DWORD addr = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);

                    // 判空走解码路径（ECC 是否成码字 + 整条 CW 的 1 比特占比），
                    // 不是「RAW 读数 FF」：RAW 读跳过 ECC，也看不到 META/parity。
                    BOOL blank = false;

                    if ((ftl_page_blank_check(addr, &blank) == FTL_OK) && (blank == false))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

/* ── store ── */

/**
 * @brief 将 BBT 追加写入 NAND BBT 池（同步）
 *
 * 游标未建立时先定位；无论成败都推进游标（半编程槽不可复用）；失败保留
 * dirty 供重试。
 *
 * @param alloc Allocator 上下文
 * @return FTL_OK（含 dirty==0 早退）；FTL_ERROR 定位/写失败
 */
ftl_status_e bbt_store(alc_s *alloc)
{
    if (alloc->bbt.dirty == false)
    {
        return FTL_OK;
    }

    // 同步路径与异步 job 共用同一个 write_idx / sequence：在飞时再写会二次编程同一槽
    // 。启动路径没有在飞 job；运行期只有 CLI format 能撞上——debug 停在
    // 现场，release 拒绝本次 save（调用者按失败处理，job 完成后再试）。
    // 这里同时复用了 H14 的口径：后果是静默介质破坏（两条 record 互相 AND），
    // 所以 ASSERT 之外必须有运行期拒绝。
    ASSERT(alloc->bbt.handle.waiting == false);
    if (alloc->bbt.handle.waiting == true)
    {
        LOGE(LOG_TAG_BBT "sync save while async job in flight — refused\r\n");
        return FTL_ERROR;
    }

    bbt_probe_ctx_s ctx;
    bbt_ctx_init(alloc, &ctx);

    // 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽。
    // ⚠ sequence 只取 max：候选池可能因 BAD/体检而**缩小**，被移出池的块上仍可能
    // 留着更高 sequence 的旧 record（bootstrap 扫描仍会读到它）。若这里把 sequence
    // 降到新池内的最大值，下一次开机可能让旧 record 胜出、读到旧坏块图。
    if (alloc->bbt.cursor_valid == false)
    {
        md_ring_locate_out_s loc;

        if (md_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &loc) != FTL_OK)
        {
            return FTL_ERROR;
        }
        alloc->bbt.write_idx = loc.write_idx;
        if (loc.sequence > alloc->bbt.sequence)
        {
            alloc->bbt.sequence = loc.sequence;
        }
        alloc->bbt.cursor_valid = true;
    }

    alloc->bbt.sequence++;

    // 快照本条 record 的 dirty 代际：同步写的等待泵里可能派发完成回调并再次标坏
    // （host 写失败），那条 record 不含新块 ⇒ 不能清 dirty（与异步路径
    // bbt_write_handle_callback 对称）。
    const DWORD gen = alloc->bbt.dirty_gen;

    // record 格式与异步路径 bbt_write_submit **必须逐字相同**（两条写路径共用同一个
    // 池，字段不一致会让另一条路径写出的 record 读回时判无效）。sequence 用本路径
    // 刚自增的值。
    bbt_header_s header;

    memset(&header, 0, sizeof(header));
    header.magic        = BBT_MAGIC;
    header.sequence     = alloc->bbt.sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count    = alloc->bad_count;
    header.reserved[0]  = alloc->bbt.format_in_progress ? BBT_FLAG_FORMAT_IN_PROGRESS : 0U;
    header.reserved[1]  = alloc->bbt.rebuild_required ? BBT_FLAG_REBUILD_REQUIRED : 0U;

    for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            bbt_set_bad(header.bad_map, gblock);
        }
    }

    // 追加写：池内下一槽（块满则先擦池内下一块）。
    // 无论成败都推进游标（已写过的槽可能半编程，不可复用）。
    DWORD addr;
    if (md_ring_prepare_write(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, &addr) != FTL_OK)
    {
        return FTL_ERROR;
    }

    // 开机/格式化：同步镜像写到 P0+P1（没有服务泵可等）。
    if (ftl_sync_write_page_mirror(addr, &header, sizeof(header), FTL_PAGE_CFG_META()) != 0)
    {
        alloc->bbt.write_idx = ((alloc->bbt.write_idx + 1U) % md_ring_total_records(&ctx.ring));
        return FTL_ERROR; /* dirty 保留 ⇒ 下次重试 */
    }

    alloc->bbt.write_idx = ((alloc->bbt.write_idx + 1U) % md_ring_total_records(&ctx.ring));

    if (alloc->bbt.dirty_gen == gen)
    {
        alloc->bbt.dirty = false;
    }
    else
    {
        LOGE(LOG_TAG_BBT "record written but dirty_gen advanced — 再落一条\r\n");
    }

    return FTL_OK;
}

/* ── mark ── */

/**
 * @brief 运行时标坏块（幂等，BAD 终态）
 *
 * 先驱逐 Free Pool 再置 BAD；不同步落盘（交后台 bbt_handle）；Spare 耗尽
 * 不影响标坏。
 *
 * @param alloc  Allocator 上下文
 * @param gblock 全局块号
 * @return FTL_OK（含已 BAD 的幂等早退）；FTL_ERROR 参数非法
 */
ftl_status_e bbt_mark_bad(alc_s *alloc, WORD gblock)
{
    if ((alloc == NULL) || (gblock >= BBT_MAX_BLOCKS) || (alloc->blocks == NULL))
    {
        return FTL_ERROR;
    }

    // BAD 是终态：重复标坏必须幂等——不重复计数、不重复弹 Spare、不重复置 dirty。
    // 多个调用点（host 写失败 / migrate 擦失败 / RR）可能对同一块重复调用。
    if (ftl_block_is_bad(&alloc->blocks[gblock]))
    {
        return FTL_OK;
    }

    // 驱逐出 Free Pool —— **必须先于置 BAD**：alc_free_remove 校验「在池 ⇒ state == FREE」
    // （allocator.h 不变量②），命中后置 NONE；随后置 BAD 抹掉这个中间态。
    // 不驱逐则留下「在池却不是 FREE」的反向不一致（运行时标坏：擦除失败 / 预测性标坏）。
    // 不在池内 ⇒ no-op（alc_free_remove 的文档化契约，判据 4）。
    // 注：出厂扫描 / BBT 加载路径跑在建池之前（池还是空的）⇒ 恒 no-op，无需特判。
    alc_free_remove(alloc, gblock);

    // BST 置 BAD（单数据源，后台 bbt_handle 持久化到 NAND）。BAD 是状态机**终态** ⇒
    // 一行赋值即抹掉 OPEN/CLOSED/FREE/LOG（原掩码漏位的风险消失）；温度字段不动（与状态正交）。
    // 同时清空有效计数——坏块后该块不会再产生有效 CW，
    // 残留的 valid_count 会让 GC/WL 误判其为可回收块。
    ftl_block_set_bad(&alloc->blocks[gblock]);

    // Closed SB 里的块被运行期标坏（host 读失败 / 擦失败 / 预测性退役）：同步把该
    // plane 从 Closed 条目摘掉。不摘的话，条目最后一个活 plane 被搬完时
    // mig_job_finish 按“victim 全 plane 原值”查不到它（该 plane 已是 NO_PLANE）⇒
    // 条目永久留下、plane_count 变 0 ⇒ GC/WL 永远跳过 ⇒ Closed 列表槽泄漏到满，
    // 之后关闭 SB 全部失败、写入报错。
    (void)alc_super_closed_drop_plane(alloc, ftl_gblock_plane(gblock), ftl_gblock_block(gblock));

    alloc->bad_count++;
    bbt_dirty_note(alloc);

    // 不同步调 bbt_store() — NAND BBT 持久化由后台 bbt_handle 批量执行。
    //
    // 坏块替换分两类（主流 BBT + sparing）：
    // · 元数据保留区（block < FTL_METADATA_AREA_END）：由 md_pool_service 重算
    // 候选池、从 shared metadata spare 补块；**不**弹用户 Spare（否则会把
    // 元数据的容量损失伪装成用户容量损失）。
    // · 用户区：Spare 是第一道替换；Spare 空时从 OP 借一块（第二道），
    // 避免运行期坏块直接吃用户可用块。两道都空 ⇒ 低水位告警，容量自然下降。
    const BYTE plane = ftl_gblock_plane(gblock);
    const WORD block = ftl_gblock_block(gblock);

    // 元数据块有两类：固定保留区（< AREA_END），以及**从 OP 借用的 L2P 日志块**
    // （物理块在用户区，但 Directory 的 l2p_live/当前日志块指向它）。后者不能
    // 当数据坏块处理（会错误弹用户 Spare、并漏掉 rebuild 标志）。
    const BOOL has_md_dir     = (alloc->md_dir != NULL);
    const BOOL is_l2p_live    = has_md_dir && (alloc->md_dir->l2p_live[gblock] != 0U);
    // gblock < FTL_BLOCK_TOTAL 由入口守卫保证（BBT_MAX_BLOCKS == FTL_BLOCK_TOTAL），不重复判。
    const BOOL is_current_log = has_md_dir && alloc->md_dir->cur.valid[plane] && (alloc->md_dir->cur.block[plane] == gblock);
    const BOOL borrowed_log   = is_l2p_live || is_current_log;

    if ((block < FTL_METADATA_AREA_END) || borrowed_log)
    {
        if (alloc->pools != NULL)
        {
            alloc->pools->dirty = true;
        }

        // Journal / L2P 日志块（固定或借用）丢失无法在当前上电周期内恢复其映射 ⇒
        // 下次开机 full metadata rebuild（本仓自拟的 BBT 持久标志）。目录检查点/CP/BBT
        // 不需要：候选池重算 + 下一次提交即可自愈。
        if (borrowed_log || ((block >= JOURNAL_BLOCK0) && (block < (JOURNAL_BLOCK0 + JOURNAL_BLOCK_COUNT))) || ftl_is_l2p_log_block(block))
        {
            alloc->bbt.rebuild_required = true;
            LOGE(LOG_TAG_BBT "metadata log block %hu lost — rebuild required\r\n", block);
        }
    }
    else if ((alloc->bbt.format_in_progress == false) && (bbt_spare_refill(alloc, plane) != FTL_OK))
    {
        if (alc_free_refill_from_op(alloc, plane) == FTL_GBLOCK_INVALID)
        {
            // 限速：保留区双空时每次坏块都打会刷屏；只报第 1/64 次。
            if ((s_reserve_low_logged++ % BBT_RESERVE_LOW_LOG_EVERY) == 0U)
            {
                LOGW(LOG_TAG_BLK "reserve low plane=%hhu spare=%hu op=%hu free=%hu count=%lu\r\n", plane, alc_spare_get_count(alloc, plane), alc_op_get_count(alloc, plane), alloc->free_count, s_reserve_low_logged);
            }
        }
    }

    return FTL_OK;
}

/* ── ring ── */

/* BBT 池：环形轮转（一条 record = 1 页）
 * 候选池已建立时用它（跳过 BAD）；`bbt_init` 自举期间池尚未建立 ⇒ 退回
 * 「名义 BBT 块 + 全部 shared spare」的物理扫描列表，record 自带 magic/sequence，
 * 坏块/空块读失败会被 md_ring_locate 自然忽略。 */

/**
 * @brief 建立本实例的 BBT 池轮转视图
 *
 * 候选池已建立（`alloc->pools->bbt` 至少 2 块）时直接取用；否则退回自举池
 * （名义 BBT 块 + 全部 shared spare），供池尚未建立的初始化路径扫描。
 *
 * @param ring  输出：BBT 池轮转描述
 * @param alloc Allocator 上下文（取 `alloc->pools->bbt`；NULL 时走自举池）
 */
static void bbt_ring_init(md_ring_s *ring, const alc_s *alloc)
{
    if ((alloc != NULL) && (alloc->pools != NULL) && (alloc->pools->bbt.block_count >= 2U))
    {
        *ring = alloc->pools->bbt;
        return;
    }

    // 自举池：此时 BAD 集未知，不做过滤。record 自带 magic/sequence，坏块/空块的
    // 读失败由 md_ring_locate 的探针自然忽略。
    ring->first_block      = BBT_BLOCK;
    ring->nominal_count    = BBT_POOL_BLOCKS;
    ring->block_count      = 0U;
    ring->pages_per_record = 1U;
    ring->plane_mask       = METADATA_MIRROR_PLANE_MASK;

    for (WORD b = BBT_BLOCK; b < (BBT_BLOCK + BBT_POOL_BLOCKS); b++)
    {
        ring->blocks[ring->block_count++] = b;
    }
    for (WORD s = MD_SPARE_BLOCK0; s < FTL_METADATA_AREA_END; s++)
    {
        ring->blocks[ring->block_count++] = s;
    }
}

/**
 * @brief 把当前写游标指向的 BBT ring 块标坏（ring 块永久失败的唯一恢复路径）
 *
 * 镜像池要求同号块在 plane_mask 的所有 plane 上都可用 ⇒ 逐 plane 标坏。走
 * 元数据块分支：置 `pools->dirty`（由 md_pool_service 重建候选池并作废游标），
 * 不消耗用户 Spare。
 *
 * @param alloc Allocator 上下文（ring 几何取当前候选池/引导环）
 *
 * @return true = 至少标坏了一个 plane（含"已经 BAD"的幂等命中）
 */
static BOOL bbt_ring_block_mark_bad(alc_s *alloc)
{
    bbt_probe_ctx_s ctx;

    bbt_ctx_init(alloc, &ctx);

    const WORD rpb   = md_ring_records_per_block(&ctx.ring);
    const WORD total = md_ring_total_records(&ctx.ring);

    if ((rpb == 0U) || (alloc->bbt.write_idx >= total))
    {
        return false;
    }

    const WORD block = ctx.ring.blocks[alloc->bbt.write_idx / rpb];
    BOOL       any   = false;

    for (BYTE pi = 0U; pi < md_ring_plane_count(ctx.ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(ctx.ring.plane_mask, pi);

        if (bbt_mark_bad(alloc, ftl_gblock(plane, block)) == FTL_OK)
        {
            any = true;
        }
    }

    // 候选表将改变：下一次 save/job 必须在新池上重新定位（md_pool_build_all 也会置位，
    // 这里先置是给"重建被推迟"的窗口兜底）。
    alloc->bbt.cursor_valid = false;
    return any;
}

/* ── ctx ── */

/**
 * @brief 装配本实例的池 + 路由（探针/定位/写点都要这一对，四处共用）
 *
 * @param alloc Allocator 上下文（取实例路由与候选池）
 * @param ctx   输出：池轮转描述 + 拓扑路由
 */
static void bbt_ctx_init(const alc_s *alloc, bbt_probe_ctx_s *ctx)
{
    bbt_ring_init(&ctx->ring, alloc);
    ctx->topo = alc_topo(alloc);
}

/* ── write ── */

/**
 * @brief 把当前 BST 重建为一条 BBT record，**异步**提交写（phase = BBT_JOB_WRITE）。
 *
 * @param alloc 分配器（写点 = alloc->bbt.write_idx）。
 *
 * @return FTL_OK    已提交（waiting = 1）；
 *         FTL_AGAIN 瞬态背压（池空；waiting 保持 0）；
 *         FTL_ERROR 建链 / 路由永久失败（waiting 保持 0；由 bbt_write_kick 标坏并重建）。
 *
 * @note record 内容在**提交时**从 BST 重建（不是起步时快照）——换块擦的等待
 * 期间新标坏的块会被一并记进本条 record，是严格的信息超集。
 * header 是**栈上** 540 B：ftl_async_write_page 在 prepare 时已把它拷进
 * FC buffer 池（同 jrn_freeze 的做法，不需要为异步保留快照缓冲）。
 * 地址按 write_idx 重算（write_idx 写成功才推进 ⇒ 此刻它仍指向本槽）。
 */
static ftl_status_e bbt_write_submit(alc_s *alloc)
{
    bbt_probe_ctx_s ctx;
    bbt_ctx_init(alloc, &ctx);

    // record 格式与同步路径 bbt_store **必须逐字相同**；sequence 在 kick 起步时已自增。
    bbt_header_s header;

    memset(&header, 0, sizeof(header));
    header.magic        = BBT_MAGIC;
    header.sequence     = alloc->bbt.sequence;
    header.total_blocks = BBT_MAX_BLOCKS;
    header.bad_count    = alloc->bad_count;
    header.reserved[0]  = alloc->bbt.format_in_progress ? BBT_FLAG_FORMAT_IN_PROGRESS : 0U;
    header.reserved[1]  = alloc->bbt.rebuild_required ? BBT_FLAG_REBUILD_REQUIRED : 0U;

    for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        if (ftl_block_is_bad(&alloc->blocks[gblock]))
        {
            bbt_set_bad(header.bad_map, gblock);
        }
    }

    // 快照本条 record 的 dirty 代际：写飞行期间若又标坏（dirty_gen++），
    // 完成时不能清 dirty，必须再落一条 record。
    alloc->bbt.handle.dirty_gen = alloc->bbt.dirty_gen;

    DWORD addr = md_ring_addr(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, 0U);

    fc_cmd_s          *cmd;
    const ftl_status_e mirror_st = ftl_async_write_page_mirror(&cmd, addr, &header, sizeof(header), FTL_PAGE_CFG_META());

    if (mirror_st != FTL_OK)
    {
        return mirror_st; /* FTL_AGAIN 池空 / FTL_ERROR 建链失败 */
    }

    const ftl_status_e submit_st = ftl_op_submit(cmd, FTL_OP_BBT, alloc);

    if (submit_st != FTL_OK)
    {
        fc_command_release(cmd->cmd_id);
        return submit_st; /* FTL_AGAIN 背压 / FTL_ERROR 路由 / 编程错误 */
    }

    alloc->bbt.handle.phase   = BBT_JOB_WRITE;
    alloc->bbt.handle.waiting = true;
    return FTL_OK;
}

/**
 * @brief 起一次 BBT record 写并按提交状态收敛（3 个调用点共用）
 *
 * FTL_AGAIN（命令 / desc / buffer / SQ 池空）⇒ 停在 BBT_JOB_WRITE / waiting=0，
 * 下轮 bbt_handle 重试（write_idx 未动，同一槽重写）。
 * FTL_ERROR（建链 / 路由永久失败）⇒ 标坏当前 ring 块并回 IDLE：dirty 保留，
 * 下一轮 md_pool_service 重建候选池后在新块重写（与擦完成失败路径同口径）。
 *
 * @param alloc 分配器
 */
static void bbt_write_kick(alc_s *alloc)
{
    const ftl_status_e st = bbt_write_submit(alloc);

    if (st == FTL_OK)
    {
        return; /* submit 已置 WRITE / waiting=true */
    }
    if (st == FTL_AGAIN)
    {
        alloc->bbt.handle.phase   = BBT_JOB_WRITE;
        alloc->bbt.handle.waiting = false;
        return; /* 下轮重试 */
    }

    LOGE(LOG_TAG_BBT "record write submit failed — 标坏 ring 块并重建候选池\r\n");
    (void)bbt_ring_block_mark_bad(alloc);

    alloc->bbt.handle.phase   = BBT_JOB_IDLE;
    alloc->bbt.handle.waiting = false;
}

/**
 * @brief record 写完成的服务函数
 *
 * **无论成败都推进游标**：已写过的槽可能半编程，不可复用。
 * dirty 的清除还要比 dirty_gen —— 写飞行期间若又标了坏块，本条 record 是旧快照。
 *
 * @param alloc      Allocator 上下文
 * @param cmd        完成的单条命令（本函数负责 fc_command_release）
 * @param completion FC 完成状态（status 判成败）
 */
static void bbt_write_handle_callback(alc_s *alloc, fc_cmd_s *cmd, const fc_cmd_status_s *completion)
{
    bbt_probe_ctx_s rctx;

    bbt_ctx_init(alloc, &rctx);
    alloc->bbt.write_idx = ((alloc->bbt.write_idx + 1U) % md_ring_total_records(&rctx.ring));

    if (fc_cmd_status_has_error(completion->status))
    {
        LOGE(LOG_TAG_BBT "record write failed status=0x%02x — dirty 保留\r\n", completion->status);
    }
    else if (alloc->bbt.dirty_gen == alloc->bbt.handle.dirty_gen)
    {
        alloc->bbt.dirty = false;
    }
    else
    {
        LOGE(LOG_TAG_BBT "record written but dirty_gen advanced — 再落一条\r\n");
    }

    alloc->bbt.handle.phase   = BBT_JOB_IDLE;
    alloc->bbt.handle.waiting = false;
    fc_command_release(cmd->cmd_id);
}

/* ── erase ── */

/**
 * @brief 擦完成的服务函数
 *
 * 成功 ⇒ 起写 record；失败 ⇒ 标坏该 ring 块并回 IDLE（永久重试同一块
 * 会让 dirty 永不落盘）。
 *
 * @param alloc      Allocator 上下文
 * @param cmd        完成的单条命令（本函数负责 fc_command_release）
 * @param completion FC 完成状态（status 判成败）
 */
static void bbt_erase_handle_callback(alc_s *alloc, fc_cmd_s *cmd, const fc_cmd_status_s *completion)
{
    if (fc_cmd_status_has_error(completion->status))
    {
        LOGE(LOG_TAG_BBT "ring block erase failed status=0x%02x — 标坏该块并重建候选池\r\n", completion->status);

        // 擦不掉的 ring 块不能重试：标坏两个镜像 plane ⇒
        // pools->dirty ⇒ md_pool_service 重建候选池；下一次 job 在新池上重新定位。
        // 若重建也凑不齐（metadata 区耗尽），md_pool_service 会置 rebuild_required 并
        // 在日志里显性化，不会静默循环。
        (void)bbt_ring_block_mark_bad(alloc);

        alloc->bbt.handle.phase   = BBT_JOB_IDLE;
        alloc->bbt.handle.waiting = false;
        fc_command_release(cmd->cmd_id);
        return;
    }

    // 擦成功 ⇒ 起写；提交失败由 bbt_write_kick 收敛（AGAIN 续跑 / ERROR 标坏重建）。
    bbt_write_kick(alloc);
    fc_command_release(cmd->cmd_id);
}

/* ── factory ── */

/**
 * @brief 出厂坏块扫描（OOB 标记）
 *
 * 前置：介质必须是空片（用过的块 spare[0] 是 FTL 数据，扫出来是「哪些块被
 * 写过」）；只读不擦。扫描完置 dirty 并落盘。
 *
 * @param alloc Allocator 上下文（BST 输出）
 */
static void bbt_factory_scan(alc_s *alloc)
{
    WORD bad_idx = 0U;

    alloc->bbt.dirty = false;

    // 逐块扫描（**含 Block 0**）：不再假设「出厂首块必有效」——该假设没有仓内
    // 原文出处。多付 1 次列读换掉一个无依据的器件断言；若 Block 0
    // 确有出厂坏块标记，它会像其它块一样被标坏，由 shared spare 补位。
    for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
    {
        // 全局块号 → 各 Plane 的物理块号
        BYTE plane = ftl_gblock_plane(gblock);
        WORD block = ftl_gblock_block(gblock);

        // 只读标记（do_erase=false）：出厂扫描阶段**不得**擦块——擦除会永久毁掉标记
        if (bbt_probe_block_bad(alloc, plane, block, false))
        {
            ftl_block_set_bad(&alloc->blocks[gblock]);
            bad_idx++;
        }
    }

    alloc->bad_count = bad_idx;
    bbt_dirty_note(alloc);

    // 先按扫描结果建好候选池（BBT 本身可能落在 shared spare），再写 BBT record；
    // 否则 bbt_store 会沿用自举扫描列表，可能选中刚扫出的坏块。
    if (md_pool_build_all(alloc) == false)
    {
        LOGE(LOG_TAG_BBT "factory scan: metadata candidate pool build failed — BBT not saved\r\n");
        return;
    }

    // 将扫描结果写入 NAND BBT
    (void)bbt_store(alloc);
}

/* ── format ── */

/**
 * @brief 格式化用的两阶段擦除：先落 BBT，再擦 BBT 池之外的块
 *
 * ① bbt_dirty_note + bbt_store 落当前 BST 坏块图并置 FORMAT_IN_PROGRESS；失败
 *    ⇒ 不擦任何用户块（工厂标记仍完整）。
 * ② 擦除除 BBT 池之外的全部块（所有 plane）；擦失败的块标 BAD。
 * ③ 清 FORMAT_IN_PROGRESS，再次 bbt_store 并入新 BAD（best-effort）。
 *
 * 必须在 bbt_init 之后调用（出厂标记只在 bbt_init 读取）；FORMAT_IN_PROGRESS
 * 由调用者在 initial CP 落盘后清除。① 成功后任何时候掉电，新 BBT record 都保留；
 * ② 中途掉电时下次开机据 FORMAT_IN_PROGRESS 继续格式化，而不是误判介质损坏。
 *
 * @param alloc Allocator 上下文
 * @return FTL_OK；FTL_ERROR ① 的 BBT save 失败
 */
ftl_status_e bbt_format_erase(alc_s *alloc)
{
    // ① 先持久化当前坏块图（工厂标记 + CP 快照并集都在 BST），并置“格式化进行中”。
    // 失败 ⇒ 不擦任何用户块：工厂标记仍完整，可下次重试。
    alloc->bbt.format_in_progress = true;
    bbt_dirty_note(alloc);

    if (bbt_store(alloc) != FTL_OK)
    {
        LOGE(LOG_TAG_BBT "format: pre-erase BBT save failed — abort before erasing data\r\n");
        return FTL_ERROR;
    }

    // ② 擦除 BBT 候选池之外的块；擦失败的块标 BAD。候选池保留，以保存 ① 的 record。
    // 候选池可能含 shared metadata spare（名义 BBT 块坏掉时），因此不能只按
    // `[BBT_BLOCK, +BBT_POOL_BLOCKS)` 判断。
    const md_ring_s *bbt_ring = (alloc->pools != NULL) ? &alloc->pools->bbt : NULL;

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        for (WORD block = 0U; block < BLOCKS_PER_PLANE; block++)
        {
            const BOOL keep_bbt = bbt_block_is_candidate(bbt_ring, block);

            if (keep_bbt)
            {
                continue; /* 保留 BBT 候选池（新坏块图 record 在这里） */
            }

            const WORD  gblock = ftl_gblock(plane, block);
            const DWORD addr   = pba_make(alloc->ch, alloc->ce, alloc->lun, block, plane, 0U, 0U);

            if (ftl_sync_erase_block(addr) == FTL_OK)
            {
                continue;
            }

            // 擦不掉 ⇒ 坏块（已是坏块的不重复计数）
            if (ftl_block_is_bad(&alloc->blocks[gblock]) == false)
            {
                ftl_block_set_bad(&alloc->blocks[gblock]);
                alloc->bad_count++;
            }
        }
    }

    // ③ 把擦除失败新标坏的块并入 BBT（best-effort；工厂坏块图已在 ① 落盘，
    // 这里失败只留下 dirty 给后台 bbt_handle）。
    // **不清 format_in_progress**：清标志归 pfr_media_format 的提交点
    // （initial Checkpoint 已读回），否则擦除后复位会让下次开机拒绝格式化。
    bbt_dirty_note(alloc);
    (void)bbt_store(alloc);

    return FTL_OK;
}

/* ── load ── */

/**
 * @brief 从 NAND BBT 池加载最新有效 record
 *
 * 池内轮转扫描；命中后恢复本实例的 sequence 与写游标。找不到时保持 0/无效。
 *
 * @param alloc    Allocator 上下文（池路由与游标写回）
 * @param bad_map  输出：bitmap
 * @param sequence 输出：最新 sequence
 * @return FTL_OK；FTL_ERROR 无有效 BBT
 *
 * @note record 里的 `bad_count` 是冗余字段，**不输出**：唯一调用者按 bitmap 逐位
 *       重建并以重建结果为准（单数据源）；其余调用者不存在（本函数 static）。
 */
static ftl_status_e bbt_load(alc_s *alloc, BYTE *bad_map, DWORD *sequence)
{
    // 复位本实例池状态：无有效 record 时保持 0/无效（后续 bbt_store 从池首开始）
    alloc->bbt.sequence           = 0U;
    alloc->bbt.write_idx          = 0U;
    alloc->bbt.cursor_valid       = false;
    alloc->bbt.format_in_progress = false;

    bbt_probe_ctx_s ctx;
    bbt_ctx_init(alloc, &ctx);

    md_ring_locate_out_s loc;

    if (md_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &loc) != FTL_OK)
    {
        return FTL_ERROR;
    }
    alloc->bbt.sequence  = loc.sequence;
    alloc->bbt.write_idx = loc.write_idx;
    if (loc.newest_idx == UINT16_MAX)
    {
        alloc->bbt.sequence  = 0U;
        alloc->bbt.write_idx = 0U;
        return FTL_ERROR; /* 池内无有效 record */
    }

    alloc->bbt.cursor_valid = true;

    // 重读胜者（locate 只返回序号，Header 本体要再读一次）。
    // 镜像池：逐个 plane 试；sequence 必须等于 locate 选中的 record sequence。
    bbt_header_s best;
    BOOL         found = false;
    for (BYTE pi = 0U; pi < md_ring_plane_count(ctx.ring.plane_mask); pi++)
    {
        const BYTE plane = md_ring_plane_at(ctx.ring.plane_mask, pi);
        DWORD      addr  = md_ring_addr_plane(&ctx.ring, &ctx.topo, loc.newest_idx, 0U, plane);

        if (ftl_sync_read_page(addr, &best, sizeof(best), FTL_PAGE_CFG_META()) != 0)
        {
            continue;
        }
        // 与 bbt_record_probe 同一有效性判据（magic + total_blocks）。
        if ((best.magic != BBT_MAGIC) || (best.total_blocks != BBT_MAX_BLOCKS) ||
            (best.sequence != alloc->bbt.sequence))
        {
            continue;
        }
        found = true;
        break;
    }
    if (found == false)
    {
        return FTL_ERROR;
    }

    alloc->bbt.format_in_progress = ((best.reserved[0] & BBT_FLAG_FORMAT_IN_PROGRESS) != 0U);
    alloc->bbt.rebuild_required   = ((best.reserved[1] & BBT_FLAG_REBUILD_REQUIRED) != 0U);

    // 输出到 caller（record 的 bad_count 是冗余字段，调用者按 bitmap 重建）
    memcpy(bad_map, best.bad_map, BBT_BITMAP_SIZE);
    *sequence = best.sequence;

    return FTL_OK;
}

/* ── init ── */

/**
 * @brief 从 NAND BBT 池加载坏块表并重建 BST BAD 位
 *
 * 无有效 BBT 时只有空片才允许出厂扫描；已用介质拒绝重扫，坏块表从空开始。
 * 用过的块 Page 0 的 spare 首字节是 FTL 自己的数据（页编程 2112B 把 CW3 尾巴
 * 64 字节溢进备区），不是出厂标记 ⇒ 重扫会把所有写过的块判成坏块并落盘。
 *
 * @param alloc Allocator 上下文（blocks / bad_count 输出）
 */
void bbt_init(alc_s *alloc)
{
    alloc->bbt.sequence           = 0U;
    alloc->bbt.dirty              = false;
    alloc->bbt.dirty_gen          = 0U;
    alloc->bbt.format_in_progress = false;
    alloc->bbt.rebuild_required   = false;
    alloc->bbt.flush_due          = 0U;
    // 清异步 job（alc_init 已零化，此处再钉一次）
    alloc->bbt.handle.phase       = BBT_JOB_IDLE;
    log_heartbeat_end(&alloc->bbt.handle.hb);
    alloc->bbt.handle.waiting = false;

    // 尝试从 NAND 加载已存在的 BBT
    {
        BYTE tmp_bad_map[BBT_BITMAP_SIZE];

        DWORD seq;

        const ftl_status_e ret = bbt_load(alloc, tmp_bad_map, &seq);

        if (ret == FTL_OK)
        {
            WORD bad_idx = 0U;

            alloc->bbt.sequence = seq;

            // 从 NAND bitmap 重建 BST BAD 位（单数据源）；bad_count 以重建结果为准，
            // 不信任 record 里的冗余字段。
            for (WORD gblock = 0U; gblock < BBT_MAX_BLOCKS; gblock++)
            {
                if (bbt_is_bad(tmp_bad_map, gblock))
                {
                    ftl_block_set_bad(&alloc->blocks[gblock]);
                    bad_idx++;
                }
            }
            alloc->bad_count = bad_idx;

            return;
        }
    }

    // 无有效 BBT → **只有空片**才允许出厂扫描。
    //
    // ⚠ 为什么必须拦：用过的块，其 Page 0 的 spare 首字节（列 2048）**不是**出厂标记，
    // 而是 FTL 自己的数据——FC 一次页编程写 PAGE_SIZE = 2112（主区 2048 + 备区 64），
    // 页缓冲按 4 × CW(528B) 拼 ⇒ CW3 的尾巴 64 字节正好落进备区，覆盖掉出厂标记。
    // 于是「列 2048 非 0xFF ⇒ 坏块」这条判据在用过的介质上退化成「哪些块被写过」，
    // 重扫会把**所有写过的块**（含活的目录检查点/Checkpoint/Wear）判成坏块，随后
    // bbt_store 把这份垃圾落盘 —— 真坏块反而丢了，活块全废。
    //
    // 判据 = bbt_medium_has_ftl_data()（元数据区里有没有合法的 FTL 页）。
    // 空片路径不受影响：那时列 2048 确实就是出厂标记。
    //
    // 拒绝扫描后的状态：坏块表从空开始（bad_count = 0、sequence 仍 0、**不置 dirty**
    // ⇒ 不落盘，免得把「零坏块」写成既成事实）。真坏块由运行时 bbt_mark_bad 兜底
    // （PROGRAM/ERASE 失败即标）。
    if (bbt_medium_has_ftl_data(alloc))
    {
        LOGE(LOG_TAG_BBT "无有效 BBT，但介质已被格式化过 —— **拒绝出厂扫描**：\r\n");
        LOGE("      用过的块 spare[0] 是 FTL 数据而非出厂标记，重扫会把写过的块全判坏。\r\n");
        LOGE("      坏块表从空开始；真坏块由运行时 bbt_mark_bad 兜底。\r\n");

        alloc->bad_count = 0U;
        return;
    }

    // 空片 → 执行出厂扫描（坏块数只写 alloc->bad_count）。
    bbt_factory_scan(alloc);
}

/* ── handle ── */

/**
 * @brief BBT 后台持久化入口（提交即返回）
 *
 * 单飞行；换块先异步擦再写。sequence 在起步时自增，续跑不再自增；擦失败 ⇒
 * 标坏该 ring 块并重建候选池，dirty 保留等下一次 kick。
 * 只保证「已起步」；成败判据 = job 走完且 dirty 清零。提交被背压拒时（job 停在
 * WRITE 但无命令在飞）只续提交写，不重走换块判定——擦已成功，重擦纯属磨损。
 * 只由后台调度器调用；开机 / 格式化路径走同步 bbt_store。
 *
 * @param alloc Allocator 上下文
 */
void bbt_handle(alc_s *alloc)
{
    // 心跳（见 log.h / timer.h）：只在 age >= 5 s 时打一行 phase/age
    if (alloc->bbt.handle.phase == BBT_JOB_IDLE)
    {
        log_heartbeat_end(&alloc->bbt.handle.hb);
    }
    else if (alloc->bbt.handle.hb.active == false)
    {
        log_heartbeat_begin(&alloc->bbt.handle.hb);
    }
    else if ((log_heartbeat_age_ms(&alloc->bbt.handle.hb) >= LOG_RATE_SLOW_AFTER_MS) && log_heartbeat_due(&alloc->bbt.handle.hb))
    {
        LOGI(LOG_TAG_BBT "job phase=%d waiting=%d seq=%lu idx=%u age=%lums\r\n", alloc->bbt.handle.phase, alloc->bbt.handle.waiting, alloc->bbt.sequence, alloc->bbt.write_idx, log_heartbeat_age_ms(&alloc->bbt.handle.hb));
    }

    if (alloc->bbt.handle.waiting == true)
    {
        return; /* ① 命令在飞 */
    }
    if (alloc->bbt.handle.phase == BBT_JOB_WRITE)
    {
        bbt_write_kick(alloc); /* ② 背压后续提交（擦已完成） */
        return;
    }
    if (alloc->bbt.handle.phase != BBT_JOB_IDLE)
    {
        return; /* ③ ERASE 在飞（waiting 必为 1，此处只是防御） */
    }

    if (alloc->bbt.dirty == false)
    {
        alloc->bbt.flush_due = 0U; /* 无变更：清掉过期节拍 */
        return;                    /* ④ 无变更 */
    }

    if (alloc->bbt.flush_due == 0U)
    {
        return; /* ⑤ 节拍未到：保留 dirty，等 timer 任务置位 */
    }
    alloc->bbt.flush_due = 0U;

    bbt_probe_ctx_s ctx;
    bbt_ctx_init(alloc, &ctx);

    // 游标未建立 ⇒ 先定位池内最新有效 record 的下一槽
    if (alloc->bbt.cursor_valid == false)
    {
        md_ring_locate_out_s loc;

        if (md_ring_locate(&ctx.ring, bbt_record_probe, &ctx, &loc) != FTL_OK)
        {
            LOGE(LOG_TAG_BBT "pool locate failed — 下轮重试\r\n");
            return;
        }
        alloc->bbt.sequence     = loc.sequence;
        alloc->bbt.write_idx    = loc.write_idx;
        alloc->bbt.cursor_valid = true;
    }

    alloc->bbt.sequence++;

    DWORD addr;
    BOOL  need_erase = false;
    if (md_ring_prepare_addr(&ctx.ring, &ctx.topo, alloc->bbt.write_idx, &addr, &need_erase) != FTL_OK)
    {
        LOGE(LOG_TAG_BBT "prepare failed idx=%hu\r\n", alloc->bbt.write_idx);
        return;
    }

    if (need_erase)
    {
        fc_cmd_s *cmd;

        // 换块：先擦目标块（池内轮转 ⇒ 该块持全池最旧 record，擦掉不丢信息）。
        // 镜像池 ⇒ multi-plane 擦，两个副本一起清。
        if (ftl_async_erase_block_multi_plane(&cmd, addr) != FTL_OK)
        {
            return; /* 命令池空 / 建链失败 ⇒ 下轮重试（write_idx 未动） */
        }
        if (ftl_op_submit(cmd, FTL_OP_BBT, alloc) != FTL_OK)
        {
            fc_command_release(cmd->cmd_id);
            return;
        }
        alloc->bbt.handle.phase   = BBT_JOB_ERASE;
        alloc->bbt.handle.waiting = true;
        return;
    }

    // 不必换块 ⇒ 直接起写；提交失败由 bbt_write_kick 收敛。
    bbt_write_kick(alloc);
}

/* ── print ── */

/**
 * @brief 逐块探测并打印坏块图（诊断）
 *
 * 默认只读；do_erase != 0 时额外逐块擦除（会抹掉该实例全部元数据/用户数据）。
 * ① 必须先于任何 PROGRAM/ERASE 读 Page 0 列 2048 的 spare 首字节（器件手册：
 * "bad block information is erasable and cannot be recovered once erased"）。
 * 打印每格定宽 4：好块 FF，坏块打本 plane 的绝对块号。
 *
 * @param alloc    Allocator 上下文
 * @param do_erase false = 只读；true = 逐块擦除并判擦除失败
 */
void bbt_print_bad_map(alc_s *alloc, BOOL do_erase)
{
    char line[BBT_MAP_LINE_MAX];

    LOGI("CH=%hhu  CE=%hhu  LUN=%hhu\r\n", alloc->ch, alloc->ce, alloc->lun);
    {
        int n = 0;

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            const int label_w = 7; /* "PLANE=k" */
            const int pad     = (int)((BBT_MAP_GROUP_W - label_w) / 2U);
            const int trail   = (int)BBT_MAP_GROUP_W - label_w - pad;

            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }
            n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*sPLANE=%hhu%*s", pad, "", plane, trail, "");
        }
        LOGI("%s\r\n", line);
    }
    WORD rows = (BLOCKS_PER_PLANE / BBT_MAP_COLS_PER_PLANE);
    for (WORD row = 0U; row < rows; row++)
    {
        int n = 0;

        for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
        {
            if (plane > 0U)
            {
                n += snprintf(&line[n], sizeof(line) - (size_t)n, "   ");
            }

            for (WORD col = 0U; col < BBT_MAP_COLS_PER_PLANE; col++)
            {
                const WORD block = ((row * BBT_MAP_COLS_PER_PLANE) + col);

                if (col > 0U)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, " ");
                }

                if (block >= BLOCKS_PER_PLANE)
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*s", (int)BBT_MAP_ENTRY_W, ""); /* 末排不满：留空 */
                }
                else if (bbt_probe_block_bad(alloc, plane, block, do_erase))
                {
                    // 坏块打**本 plane 的绝对块号**（0..BLOCKS_PER_PLANE-1），不是排内列号：
                    // 列号每排重新计数，读图的人得自己算 排号×列数+列号，且 1~2 位的数字
                    // 与 `FF` 并排极易被误读成十六进制字节值。
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*hu", (int)BBT_MAP_ENTRY_W, block);
                }
                else
                {
                    n += snprintf(&line[n], sizeof(line) - (size_t)n, "%*s", (int)BBT_MAP_ENTRY_W, "FF"); /* 好块 */
                }
            }
        }

        LOGI("%s\r\n", line);
    }
}

/* ── callback ── */

/**
 * @brief BBT 命令完成服务
 *
 * 只收尾 / 续提交，不调 bbt_handle（避免在完成泵里嵌套泵）：
 * ERASE 成功 → 起写 record；失败 → 游标不推进、dirty 保留；
 * WRITE 完成 → 无论成败都推进池游标（半编程槽不可复用），成功清 dirty。
 * 每条命令自己 release。
 *
 * @param ftl        实例
 * @param completion FC 完成状态
 * @param ctx        alc_s *
 */
void ftl_bbt_op_callback(ftl_ctx_s *ftl, const fc_cmd_status_s *completion, void *ctx)
{
    alc_s *alloc = (alc_s *)ctx;

    (void)ftl; /* 实例无关：ctx 就是本实例的 allocator */

    fc_cmd_s *cmd = fc_command_lookup(completion->cmd_id);

    // 孤儿 / 迟到完成：当前没有在飞命令 ⇒ 只释放，不改状态。
    if (alloc->bbt.handle.waiting == false)
    {
        LOGE(LOG_TAG_BBT "orphan completion phase=%d — ignored\r\n", alloc->bbt.handle.phase);
        fc_command_release(cmd->cmd_id);
        return;
    }

    if (alloc->bbt.handle.phase == BBT_JOB_ERASE)
    {
        bbt_erase_handle_callback(alloc, cmd, completion);
    }
    else if (alloc->bbt.handle.phase == BBT_JOB_WRITE)
    {
        bbt_write_handle_callback(alloc, cmd, completion);
    }
    else
    {
        // 不该发生：一条命令对应一个 job，phase 必为 ERASE / WRITE。
        LOGE(LOG_TAG_BBT "unexpected completion phase=%d\r\n", alloc->bbt.handle.phase);
        alloc->bbt.handle.phase   = BBT_JOB_IDLE;
        alloc->bbt.handle.waiting = false;
        fc_command_release(cmd->cmd_id);
    }
}
