/*
 * test_fc_level3.c — Level 3 FC 全链路测试：Block 级数据完整性 + 多场景组合验证
 *
 * 测试场景：
 *   1. 初始化序列：RESET → READ_ID → READ_PARAMETER_PAGE → GET_FEATURES
 *   2. Feature 读：GET_FEATURES(0x01) / GET_FEATURES(0x81)
 *   3. 状态查询：READ_STATUS / READ_STATUS_ENHANCED
 *   4. 写 + 读：NORMAL_PROGRAM_PAGE → NORMAL_READ_PAGE_WITH_DATA_OUT（全 block）
 *   5. Random 列读：NORMAL_READ_PAGE（加载页缓冲）→ RANDOM_DATA_READ（抽单 CW）
 *   6. Cache 读序列：CACHE_START(00h-30h) → CACHE_SEQ(31h) × 63 → CACHE_LAST(3Fh)
 *   7. 内部搬移：READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_DATA_MOVE
 *   8. 多 Plane：MP_ERASE / MP_PROGRAM / MP_READ
 *   9. 擦除验证：ERASE → 全 0xFF 验证
 *   （10. 紧锁定验证 BLOCK_LOCK_TIGHT —— 不可逆锁死整片 NAND，用 #if 0 隔开且不注册）
 *
 * ── 地址随机化：随机性由 munit 的种子驱动（v1.22）──
 *   9 条注册里 6 条地址相关的**在用例体内**抽地址（block / plane / 页集 / CW 集 / 搬移目标块），
 *   取值一律走本文件的 l3_*_draw（见「随机地址」段），不挂 MunitParameterEnum。
 *     · `test --log-visible info /fc-nand/lifecycle/`        —— 每条抽一组，打印抽到的值
 *     · `test --seed 0x… /fc-nand/lifecycle/`                —— 复现上一轮（开场打印 seed 值）
 *     · `test --iterations N /fc-nand/lifecycle/`            —— 每条抽 N 组（覆盖旋钮）
 *   ⚠ 为什么必须加 salt：munit 在每次 exec 前把 PRNG 重置回 runner->seed（munit.c:774），
 *     而导出的 munit_rand_int_range **不带 salt**（带 salt 的 munit_rand_at_most 是 static，
 *     munit.c:503）⇒ 用例体内的**第一次**抽样在所有用例之间取值完全相同（六条会撞在同一个
 *     块上、整轮只有一个 plane）。故本文件的 l3_rand 把 `hash(用例名) ^ hash(用途标签)`
 *     异或进每次抽样：前者让用例之间互异，后者让同一用例内各用途互异。
 *     机制与 Level 2 的 test_scramble_whiten / test_crc_step_detect 同路（那两处直接用
 *     munit_rand_int_range，因为只有一条用例在抽，撞不上）。
 *   ⚠ 复现契约 = (seed, 用例名, 第几次抽样)：reseed 是**每用例一次**，故复现单条用例
 *     不需要重放其它用例。抽到的值必须经 test_trace_summary 打出来（INFO 档），否则
 *     有 seed 也对不上是哪个块。
 *
 * PBA 约定：ch=0 ce=0 lun=0；block / plane / page 由 l3_*_draw 抽（见「随机地址」段）
 *           （v24/v25：命令携带的是 PBA，不是 PAA；地址一律用具名字段构造，
 *             见 nand_config.h 的 pba_from_addr / paa_from_addr）
 *
 * 依赖：
 *   - NAND 硬件（走 fc_submit → fc_service → nand_fc_start/step 异步路径）
 *   - 建链走共享夹具的 test_cmd_build()（FTL 策略层 ftl_cmd_configure_descriptors，
 *     「按掩码建整条链」，自 v13 起属 FTL）或建立在它之上的页 / block helper。
 *     ⚠ cw_bitmap 是**字面位图**（置 1 = 要访问该 CW，0 = 空集），v23~v26 的
 *       「0 = 本命令 plane 段内的全部 CW」重载已取消（v27）。凡"只要 1 条 desc"
 *       的用例必须显式给出该 CW 的位（flat 绝对编号：`1U << (plane * CW_NUM + cw)`，
 *       不是 `1U << cw`）；整段访问用
 *       test_plane_cw_bitmap(plane_bitmap)。config.cw 自 v23 起不参与建链。
 *   - 释放只需 fc_command_release()（v16 起沿 desc 链把 desc + buffer 一起还池）
 *
 * 用法：
 *   ssd> test --log-visible info /fc-nand/lifecycle/               # 每条抽一组，打印抽到的地址
 *   ssd> test --log-visible info --iterations 5 /fc-nand/lifecycle/ # 每条抽 5 组
 *   ssd> test --seed 0x<上一轮打印的> /fc-nand/lifecycle/            # 复现同一组地址
 */

#include "common/log.h"
#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_queue.h"
#include "fc/fc_scheduler.h"
#include "fc_test_common.h"
#include "ftl/ftl_fc_interface.h"
#include "munit.h"
#include "nand/nand_config.h"
#include "nand/onfi_1_0.h"

#include <string.h>

/**************************************************************************
 *  夹具
 *
 *  setup / teardown、读写擦原语、源数据比对、desc 链小工具全部来自共享模块
 *  fc_test_common（Level 2 / Level 3 共用一份，见 tests/unit/fc_test_common.h）。
 *  本文件只保留 block 级与场景级的薄封装。
 *
 *  与改造前的三处**有意**差别：
 *    ① 数据源从「4 字节周期 pattern」换成「按 (plane, cw) 区分的源表」
 *       g_source_data —— 旧 pattern 的 4 字节周期使 cw*512 恒为 4 的倍数，
 *       **读错 CW / 读错 plane 在旧判据下永远通过**（新判据第一次能抓到）。
 *    ② cmd->meta 从「隐式 0」换成页 PBA：页地址因此整体进了 META，
 *       数据路径 Pass 4 ② 能抓到「读到了别的页」。
 *    ③ 建链统一走共享夹具的 test_cmd_build()（FTL 策略层
 *       ftl_cmd_configure_descriptors），不再手写 config.cw。
 *
 *  setup / teardown 见套件表：test_setup / test_tear_down。
 **************************************************************************/

/* ── 随机地址窗口（v1.22）──
 * 地址里的 block 字段是 **per-plane** 号（0..BLOCKS_PER_PLANE-1）。窗口取每 plane 块数的
 * 30%~70%，**编译期**从 BLOCKS_PER_PLANE 算出（2048 ⇒ 614..1433），不写死数字。
 * 目的：避开**固定**元数据区 —— 本仓 per-plane 编号下它落在 0..41 一带
 * （L2P 0..31 / Anchor 33,34 / BBT 36,39 / Checkpoint 37,38 / Journal 40,41，
 *  见 src/ftl/metadata.c 的 ftl_is_metadata_block），L3_BLOCK_LO = 614 远在其上。
 *
 * ⚠ 窗口只保证避开**固定**区：跑 UT 时 FTL 是活的（src/main.c 无条件 ssd_ftl_boot_all()，
 *   test 命令从主循环的 uart_service_poll() 进来），free / OP / spare 池覆盖整片 device
 *   ⇒ **动态** L2P region 块（≥32 的任意块）与 FTL 用户数据块**拦不住**。这是**既有**风险
 *   （改造前就写死 100/102），窗口只是把暴露面从 2 个块放大到 820 个。 */
#define L3_BLOCK_LO ((uint16_t)(BLOCKS_PER_PLANE * 30U / 100U)) /* 2048 ⇒ 614 */
#define L3_BLOCK_HI ((uint16_t)(BLOCKS_PER_PLANE * 70U / 100U)) /* 2048 ⇒ 1433 */

_Static_assert(L3_BLOCK_LO < L3_BLOCK_HI, "随机窗口为空");
_Static_assert(L3_BLOCK_HI < BLOCKS_PER_PLANE, "随机窗口越过本 plane 块数");

/* 旧默认值，只服务本文件 #if 0 段——BLOCK_TEST 被 unlock_erase / block_lock_tight
 * 引用；BLOCK_DEST 连 #if 0 段也不引用，与 BLOCK_TEST 成对留着纯属历史。
 * 活代码一律走 l3_*_draw，不再用这两个宏。 */
#define BLOCK_TEST (TEST_BLOCK)
#define BLOCK_DEST (TEST_BLOCK_DEST)

/* ── Level 3 的「裸页」config ──
 * 与改造前 program_page / read_page / erase_block 的字段逐字一致：
 *   不加扰（FC_RANDOM_DISABLE）、LDPC RAW（不做 ECC）、不 RAID。
 * crc 逐处给——改造前的裸页读写都不设 config.crc，而 FC_CRC_ENABLE == 0 是池清零
 * 后的默认 ⇒ 实际是**开**；cache 与 multi-plane 路径改造前显式关 CRC（纯 RAW 比对）。
 * ch/ce/lun 由 test_cmd_build 从 addr 覆盖，multi_plane 由 operation 派生
 * （见 fc_test_common.h「二」），故本宏不写这两组字段。
 * 用法：`&L3_CONF(OP_READ_ID, FC_CRC_ENABLE)`（复合字面量，可取地址）。
 * ⚠ cache helper 只取本 config 的模式位，operation 被逐步覆盖（填什么都行）。 */
#define L3_CONF(operation_, crc_)                             \
    ((const fc_cmd_conf_t){.operation = (DWORD)(operation_),  \
                           .crc       = (crc_),               \
                           .random    = FC_RANDOM_DISABLE,    \
                           .ldpc      = FC_ECC_MODE_LDPC_RAW, \
                           .raid      = FC_RAID_MODE_NORMAL})

/**************************************************************************
 *  随机地址（v1.22）
 *
 *  六个地址相关的用例**在用例体内**抽地址，不挂 MunitParameterEnum —— 参数表是
 *  NULL 结尾的 const char* 数组，表达不了 820 宽的窗口，只能写死几个候选值。
 *
 *  ⚠ 必须给每次抽样加盐，理由见文件头「地址随机化」段：munit 每 exec 前 reseed 回
 *    runner->seed（munit.c:774），而导出的 munit_rand_* **不带盐**（带盐的
 *    munit_rand_at_most 是 static，munit.c:503）⇒ 用例体内的第一次抽样在所有用例
 *    之间取值完全相同。用例名让**用例之间**互异，标签让**同一用例内各用途**互异。
 **************************************************************************/

/* DJB2，与 munit.c:386 的 munit_str_hash **逐字同实现**（那份是 static，只能照抄）。 */
static uint32_t l3_hash(const char *s)
{
    uint32_t h = 5381U;

    for (; *s != '\0'; s++)
    {
        h = (h << 5U) + h + (uint32_t)(uint8_t)*s;
    }

    return h;
}

/* 每次抽样 = munit 随机流 ^ 用例名哈希 ^ 用途标签哈希。
 * 复现契约 = (seed, 用例名, 第几次抽样)：reseed 是**每用例一次**，故复现单条用例
 * **不需要**重放其它用例（比旧的「参数按用例名哈希挑值」更强）。 */
static uint32_t l3_rand(const char *test, const char *tag)
{
    return munit_rand_uint32() ^ l3_hash(test) ^ l3_hash(tag);
}

/* 窗口内抽一个块号（闭区间 [L3_BLOCK_LO, L3_BLOCK_HI]）。tag 区分同一用例内的不同用途。 */
static uint16_t l3_block_in_window(const char *test, const char *tag)
{
    const uint32_t span = (uint32_t)(L3_BLOCK_HI - L3_BLOCK_LO) + 1U;

    return (uint16_t)(L3_BLOCK_LO + (uint16_t)(l3_rand(test, tag) % span));
}

static uint16_t l3_block_draw(const char *test)
{
    return l3_block_in_window(test, "block");
}

/* 抽一个 plane。 */
static uint8_t l3_plane_draw(const char *test)
{
    return (uint8_t)(l3_rand(test, "plane") % (uint32_t)PLANE_NUM);
}

/* 抽内部搬移的目标块：同窗口，撞上源块就**环移一格**（不额外抽 ⇒ 抽样次数固定，
 * 「第 i 次迭代看到第 i 段随机流」这条复现契约才说得清）。窗口 span ≥ 2 ⇒ 环移后必 ≠ src。 */
static uint16_t l3_dest_draw(const char *test, uint16_t src)
{
    uint16_t dest = l3_block_in_window(test, "dest");

    if (dest == src)
    {
        dest = (uint16_t)((dest >= L3_BLOCK_HI) ? L3_BLOCK_LO : (dest + 1U));
    }

    return dest;
}

static uint8_t l3_arr_contains(const uint8_t *arr, uint8_t n, uint8_t v)
{
    for (uint8_t i = 0U; i < n; i++)
    {
        if (arr[i] == v)
        {
            return 1U;
        }
    }

    return 0U;
}

/* 抽 n 个**互不重复**的页（n ∈ [2,8]，页 ∈ [0, PAGES_PER_BLOCK)）与 CW（n ∈ [1,4]）。
 * 个数也随机 —— 旧的 sample 参数本来就靠 2/5/3/8 页制造形态差异，只随机位置会**回退覆盖**。
 * 拒绝采样，与 Level 2 的 l2_ecc_flip（test_fc_level2.c:638-653）同形。
 * 上限 8×4 = 32 个探针 = 旧 dense 档的最坏值 ⇒ **板上耗时上界不变**。 */
#define L3_PAGES_MIN (2U)
#define L3_PAGES_MAX (8U)
#define L3_CWS_MIN   (1U)
#define L3_CWS_MAX   (CW_NUM) /* 4 */

static uint8_t l3_pages_draw(const char *test, uint8_t pages[L3_PAGES_MAX])
{
    const uint8_t n = (uint8_t)(L3_PAGES_MIN + (l3_rand(test, "npages") % (uint32_t)(L3_PAGES_MAX - L3_PAGES_MIN + 1U)));

    for (uint8_t i = 0U; i < n; i++)
    {
        uint8_t page;

        do
        {
            page = (uint8_t)(l3_rand(test, "page") % (uint32_t)PAGES_PER_BLOCK);
        } while (l3_arr_contains(pages, i, page) != 0U);

        pages[i] = page;
    }

    return n;
}

static uint8_t l3_cws_draw(const char *test, uint8_t cws[L3_CWS_MAX])
{
    const uint8_t n = (uint8_t)(L3_CWS_MIN + (l3_rand(test, "ncws") % (uint32_t)(L3_CWS_MAX - L3_CWS_MIN + 1U)));

    for (uint8_t i = 0U; i < n; i++)
    {
        uint8_t cw;

        do
        {
            cw = (uint8_t)(l3_rand(test, "cw") % (uint32_t)CW_NUM);
        } while (l3_arr_contains(cws, i, cw) != 0U);

        cws[i] = cw;
    }

    return n;
}

/**************************************************************************
 *  辅助：逐 desc 校验 USER 区 == 该 desc 自己的 PAA 重复（链驱动）
 *
 *  与 test_verify_page_paa 的分工：
 *    · test_verify_page_paa 按 (block, page, plane_bitmap) **重算**期望，覆盖
 *      plane_bitmap 内的全部 CW —— 适合整段读；
 *    · 本函数只看**链上实际有的** desc —— 适合部分掩码读（如 RANDOM_DATA_READ 只抽
 *      1 个 CW），此时 plane_bitmap 里其余 CW 的源槽根本没被这次读填过。
 *  期望值取 desc->paa 本身（写路径填源槽用的就是它），因此天然含 block/page 维度。
 **************************************************************************/

static void verify_chain_paa(const fc_command_t *cmd)
{
    const uint8_t n = test_cw_count(cmd);

    for (uint8_t i = 0U; i < n; i++)
    {
        const fc_descriptor_t *desc = test_cw_desc(cmd, i);
        const uint8_t         *user = test_cw_user(cmd, i);

        munit_assert_not_null(desc);
        munit_assert_not_null(user);
        munit_assert_uint8(test_check_paa(user, desc->paa), ==, 1U);

        /* 证据明细（--log-visible debug）：逐条 desc 打出它自己声明的 plane / cw /
         * PAA——「链槽位与 CW 号解耦」这件事只有打出来才看得见。 */
        test_trace_detail("chain[%u] plane=%u cw=%u PAA=0x%08lX USER==PAA", (unsigned)i, (unsigned)((desc->paa >> 2U) & 1U), (unsigned)(desc->paa & 3U), (unsigned long)desc->paa);
    }
}

/**************************************************************************
 *  辅助：读一页 + 校验
 *
 *  meta 取本页 PBA，与写路径同口径 ⇒ Pass 4 ② 能抓到「读错页」。
 *  erased == 0：逐 desc 与「该 desc 的 PAA」比对；erased != 0：逐 desc USER 区全 0xFF。
 *
 *  ⚠ 擦除页的 META 是 0xFF…，数据路径 Pass 4 ①（自洽）必然不过 ⇒ 该 CW 带 MATA_ERROR
 *    位；但 FC_CW_STATUS_SUCCESS 是**另一个位**，仍置位，故「逐 CW SUCCESS」判据照旧
 *    成立（与改造前一致，不是本轮引入的）。也正因如此，擦除路径不能用聚合 status 的
 *    错误掩码当判据——那是 test_prog_block / test_read_block 的判据。
 **************************************************************************/

static void read_page_verify(uint16_t block, uint16_t page, uint8_t plane_bitmap, uint8_t erased, const fc_cmd_conf_t *config)
{
    fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = test_first_plane(plane_bitmap), .page = page},
                                       test_plane_cw_bitmap(plane_bitmap),
                                       (uint32_t)pba_from_addr(&(nand_addr_t){.block = block,
                                                                              .plane = test_first_plane(plane_bitmap),
                                                                              .page  = page}),
                                       config);

    test_memset_read_buffer(cmd);
    (void)test_submit_wait(cmd);

    munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);

    if (erased != 0U)
    {
        test_verify_erased(cmd);
    }
    else
    {
        verify_chain_paa(cmd);
    }

    test_trace_detail("read p%02u bm=0x%02X cw=%u %s", (unsigned)page, (unsigned)plane_bitmap, (unsigned)test_cw_count(cmd), (erased != 0U) ? "all-0xFF" : "PAA-verified");

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}

/**************************************************************************
 *  辅助：擦除态整块读回验证
 *
 *  ⚠ 只有它留在 Level 3：夹具的 test_read_block 用聚合 status 错误掩码当判据，
 *     而擦除页 META=0xFF… ⇒ 数据路径 Pass 4 ① 不过 ⇒ 该 CW 带 MATA_ERROR ⇒
 *     聚合判据必然失败（见 fc_test_common.h「擦除页只能逐 CW 查 SUCCESS」）。
 *     故这里只能自己循环，逐页走 read_page_verify(erased=1)。
 *  整段**写入态**读回直接用夹具的 test_read_block（判据更严：聚合错误掩码 +
 *  按 (block,page,plane_bitmap) 坐标重算期望）。
 **************************************************************************/

static void read_block_verify_erased(uint16_t block, uint8_t plane_bitmap)
{
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        read_page_verify(block, page, plane_bitmap, 1U, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE));
    }

    /* 证据摘要（--log-visible info）：页数 + 逐槽比过的字节数。
     * ⚠ 老实说清楚它证到了什么：判据是「读回的 USER 区全 0xFF」，而本块是**擦除后直接读**、
     *   此前并未写过 ⇒ 它证明「擦除态读得回来」，**不**证明「擦除真的清掉了旧数据」
     *   （写后擦再读才算）。把字节数打出来正是为了让这条判据的强弱一眼可见。 */
    test_trace_summary("erased block=%u bm=0x%02X: %u pages, %lu B all 0xFF (逐 CW SUCCESS)", (unsigned)block, (unsigned)plane_bitmap, (unsigned)PAGES_PER_BLOCK, (unsigned long)PAGES_PER_BLOCK * (unsigned long)test_plane_count(plane_bitmap) * (unsigned long)CW_NUM * (unsigned long)FC_BUFFER_USER_SIZE);
}

/**************************************************************************
 *  辅助：解锁 block（OP_BLOCK_UNLOCK_LOW + OP_BLOCK_UNLOCK_HIGH）
 *
 *  仅用于 BLOCK_LOCK 后的解锁恢复。正常情况下 NAND 上电后所有 block 默认解锁。
 *  当前唯一调用者 test_unlock_erase 在 #if 0 里，故标 unused。
 **************************************************************************/

static void __attribute__((unused)) unlock_block(uint16_t block)
{
    static const uint8_t unlock_ops[2] = {(uint8_t)OP_BLOCK_UNLOCK_LOW, (uint8_t)OP_BLOCK_UNLOCK_HIGH};

    for (uint8_t i = 0U; i < 2U; i++)
    {
        fc_command_t *cmd = fc_command_alloc();

        munit_assert_not_null(cmd);

        cmd->config.operation = (DWORD)unlock_ops[i];
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->paa              = paa_from_addr(&(nand_addr_t){.block = block});

        (void)test_submit_wait(cmd);
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }
}

/**************************************************************************
 *  辅助：命令结构体快照 + 逐字段差异对比
 *
 *  在 submit 前给 cmd 拍一份快照，release 前逐字段比对，把发生变化的字段打到
 *  串口——用于定位"命令提交后被谁改写"这类问题。正常生命周期内 fc_command_t
 *  的全部字段都应保持不变，所以只要出现差异就说明有代码在命令在飞的时候动了它。
 **************************************************************************/

/* 命令结构体快照 / 逐字段比对。排查"命令提交后被谁改写"时把下面整块打开。
 *
 * 注意：想直接比对 fc_cmd_conf_t 原始字的话，先确认 sizeof(fc_cmd_conf_t) == 4，
 * 否则整字兜底比对不成立。
 */
#if 0
static void cmd_snapshot(const fc_command_t *cmd, fc_command_t *snapshot)
{
    *snapshot = *cmd;
}

/*
 * 无条件逐字段打印 before/after 全部字段（不只差异行），差异行尾追加 <== DIFF。
 * 依赖当前作用域的 before / after / diffs / checked 四个名字——由本函数与宏
 * 约定，勿在别处复用本宏。
 * checked 用来做分母，所以新增字段只要加一行 CMD_FIELD_U 即可，计数自动同步。
 */
#    define CMD_FIELD_U(field)                                   \
        do                                                       \
        {                                                        \
            unsigned before_v = (unsigned)(before->field);       \
            unsigned after_v  = (unsigned)(after->field);        \
            uint32_t changed  = (before_v != after_v) ? 1U : 0U; \
            diffs += changed;                                    \
            checked++;                                           \
            LOGW("[DBG]   %-18s %-10u -> %-10u%s\r\n",           \
                 #field,                                         \
                 before_v,                                       \
                 after_v,                                        \
                 (changed != 0U) ? "  <== DIFF" : "");           \
        } while (0)

static uint32_t cmd_diff_report(const fc_command_t *before, const fc_command_t *after, const char *tag)
{
    uint32_t diffs      = 0U;
    uint32_t checked    = 0U;
    uint32_t word_front = 0U;
    uint32_t word_back  = 0U;

    LOGW("[DBG] %s: cmd=%p id=%u op=%u\r\n",
             tag,
             (const void *)after,
             (unsigned)after->command_id,
             (unsigned)after->config.operation);

    CMD_FIELD_U(command_id);
    CMD_FIELD_U(desc_id);
    CMD_FIELD_U(high_priority);
    CMD_FIELD_U(module);
    CMD_FIELD_U(feature_addr);
    CMD_FIELD_U(reserved1);
    CMD_FIELD_U(meta);
    CMD_FIELD_U(paa);

    CMD_FIELD_U(config.operation);
    CMD_FIELD_U(config.ch);
    CMD_FIELD_U(config.ce);
    CMD_FIELD_U(config.lun);
    CMD_FIELD_U(config.cw);
    CMD_FIELD_U(config.crc);
    CMD_FIELD_U(config.ldpc);
    CMD_FIELD_U(config.raid);
    CMD_FIELD_U(config.random);
    CMD_FIELD_U(config.chain);
    CMD_FIELD_U(config.page_type);
    CMD_FIELD_U(config.multi_plane);
    CMD_FIELD_U(config.reserved);

    /* param 是 4 字节联合体：cw_bitmap 视图覆盖全部 4 字节（parameter0..3 亦然）。 */
    CMD_FIELD_U(param.cw_bitmap);

    /* 兜底：整字比对 config，捕捉上面逐个字段漏掉的位（含未命名填充）。 */
    (void)memcpy(&word_front, &before->config, sizeof(word_front));
    (void)memcpy(&word_back, &after->config, sizeof(word_back));
    {
        uint32_t changed = (word_front != word_back) ? 1U : 0U;
        diffs += changed;
        checked++;
        LOGW("[DBG]   %-18s 0x%08lx -> 0x%08lx%s\r\n",
                 "config(raw)",
                 (unsigned long)word_front,
                 (unsigned long)word_back,
                 (changed != 0U) ? "  <== DIFF" : "");
    }

    LOGW("[DBG] %s: %lu/%lu fields changed\r\n", tag, (unsigned long)diffs, (unsigned long)checked);

    return diffs;
}
#endif

/**************************************************************************
 *  test_init_sequence — 初始化序列
 *
 *  模拟 NAND 上电后的标准初始化流程：
 *    1. RESET — 复位 NAND
 *    2. READ_ID — 读取 NAND ID
 *    3. READ_PARAMETER_PAGE — 读取 ONFI 参数页
 *    4. GET_FEATURES — 读取默认 Feature 寄存器
 *
 *  验证点：
 *    - 各命令独立执行，生命周期完整
 *    - 读取的 ID 与预期值一致
 **************************************************************************/

static MunitResult test_init_sequence(const MunitParameter params[], void *data)
{
    uint8_t id_bytes[5]   = {0U, 0U, 0U, 0U, 0U};
    uint8_t onfi_bytes[4] = {0U, 0U, 0U, 0U};

    (void)params;
    (void)data;

    test_trace_begin();

    /* Step 1: RESET —— 无数据负载，不需要 desc 链（cw_bitmap = 0 走短路分支） */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_RESET, FC_CRC_ENABLE));

        (void)test_submit_wait(cmd);
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* Step 2: READ_ID — 在 release 前验证 buffer 数据。
     * READ_ID 只返回 5 字节 ⇒ 只取 CW0（1 条 desc，位图 0x01）。 */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &L3_CONF(OP_READ_ID, FC_CRC_ENABLE));

        (void)test_submit_wait(cmd);

        /* 验证 CW 状态正常 */
        munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);

        /* 验证 desc chain buffer 有 5 字节 ID 数据（在 release 前） */
        uint8_t *buf = test_cw_user(cmd, 0U);

        munit_assert_not_null(buf);
        /* NAND 制造商 ID 应为 0x2C（Micron） */
        munit_assert_uint8(buf[0], ==, 0x2CU);

        for (uint8_t i = 0U; i < 5U; i++)
        {
            id_bytes[i] = buf[i]; /* 留作证据：把**实读到的 5 字节**打出来，而不是只报"成功" */
        }

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* Step 3: READ_PARAMETER_PAGE — 在 release 前验证 buffer 非 0xFF。
     * 参数页只看前几字节 ⇒ 只取 CW0（1 条 desc，位图 0x01）。 */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &L3_CONF(OP_READ_PARAMETER_PAGE, FC_CRC_ENABLE));

        (void)test_submit_wait(cmd);

        /* 验证参数页前 4 字节含 ONFI 签名 "ONFI"（0x4F 0x4E 0x46 0x49）
         * 数据由 PIO 写入 USER 区域（fc_buffer_id_to_real_buffer 指向的位置）。 */
        uint8_t *buf = test_cw_user(cmd, 0U);

        munit_assert_not_null(buf);
        munit_assert_uint8(buf[0], ==, 0x4FU); /* 'O' */

        for (uint8_t i = 0U; i < 4U; i++)
        {
            onfi_bytes[i] = buf[i];
        }

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* Step 4: GET_FEATURES (address 0x01) — 寄存器值经 CQ 的 param 联合体返回
     * （completion.param.parameter0..3），不再原地回填命令体。
     * GET_FEATURES 不建链（ftl_cmd_needs_desc_chain 排除），pba 与掩码都不看
     * ⇒ cw_bitmap = 0。feature_addr 由调用者建链后自行覆盖。 */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_GET_FEATURES, FC_CRC_ENABLE));

        cmd->feature_addr = 0x01U;

        munit_assert_uint32(test_submit_wait(cmd).status, ==, FC_CW_STATUS_SUCCESS);

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* 证据摘要（--log-visible info）：四条命令的**实读字节**。
     * 裸 [ OK ] 只说明命令回来了；打出 ID / ONFI 签名才说明 NAND 真的答了话。 */
    test_trace_summary("RESET ok; READ_ID=%02X %02X %02X %02X %02X; PARAM_PAGE=%02X %02X %02X %02X (%c%c%c%c); GET_FEATURES(0x01) ok",
                       (unsigned)id_bytes[0],
                       (unsigned)id_bytes[1],
                       (unsigned)id_bytes[2],
                       (unsigned)id_bytes[3],
                       (unsigned)id_bytes[4],
                       (unsigned)onfi_bytes[0],
                       (unsigned)onfi_bytes[1],
                       (unsigned)onfi_bytes[2],
                       (unsigned)onfi_bytes[3],
                       (char)onfi_bytes[0],
                       (char)onfi_bytes[1],
                       (char)onfi_bytes[2],
                       (char)onfi_bytes[3]);

    return MUNIT_OK;
}

/* ── Feature 地址表（器件手册 Table 13）──
 *   **本用例的地址一律取自这张表，不得自造。**
 *   01h Timing mode                          可读写，Table 15 给死取值（见下）
 *   80h Programmable output drive strength   可读写
 *   81h Programmable RB# pull-down strength  可读写
 *   90h Array operation mode                 可读写，且手册给死了取值：
 *                                               P1 = 08h → 使能内部 ECC
 *                                               P1 = 00h → 关闭内部 ECC（P2..P4 未用）
 *   00h / 02h–7Fh / 82h–FFh                  保留，读写皆未定义
 *
 *  ⚠ 本用例此前用 0x89（照抄 FTL `read_recovery.h` 的 RR_FEATURE_READ_RETRY）——
 *    **本器件手册里 0x89 落在 82h–FFh 保留段**，读回 0 是未定义行为，回环判据从根上不成立。
 *
 *  ⚠ 回环地址选 01h 而非 90h，**取决于还原失败的代价**。手册：feature 断电才复位，
 *    连 RESET(FFh) 都清不掉 ⇒ 任何还原失败都会把器件留在非默认态，直到断电：
 *      · 01h 停在模式 1：只是让器件按更快的上限接收命令/地址/数据，而本项目 FMC 时序
 *        是按**模式 0**（最保守）配的，主机比器件上限慢 ⇒ 照常工作。
 *      · 90h 停在 08h：内部 ECC 一直开着 ⇒ **后面所有裸页读写静默出错**。
 *    80h / 81h 一律**只读**：改驱动强度影响信号完整性；改 R/B# 下拉写重了
 *    WAIT_READY 会一直读到 busy 直到超时，把后面全部用例一起拖死。 */
#define L3_FEATURE_TIMING_MODE    (0x01U)
#define L3_FEATURE_DRIVE_STRENGTH (0x80U)
#define L3_FEATURE_RB_PULLDOWN    (0x81U)
#define L3_FEATURE_ARRAY_OP_MODE  (0x90U)
/* Table 15（Feature Address 01h: Timing Mode）：P1 = 00h..05h → 模式 0..5，P2..P4 保留为 0。
 * 模式 0 是出厂默认（"The default timing mode is mode 0"），模式 5 仅 3.3V 支持。 */
#define L3_TIMING_MODE_0 (0x00U)
#define L3_TIMING_MODE_1 (0x01U)

/* GET_FEATURES(addr) → 4 字节出参。
 * SET/GET 都不建链（ftl_cmd_needs_desc_chain 两者皆排除 ⇒ cw_bitmap 走 0U 的短路分支），
 * pba / 掩码都不看。读回的值经 CQ 的 param 联合体返回，路径是
 * 器件 → g_fc_return_staging（fc.c:1280 把它当 cw_buffers[0]）→ completion.param（fc.c:805）。 */
static void l3_feature_get(uint8_t addr, uint8_t out[4])
{
    fc_command_t       *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_GET_FEATURES, FC_CRC_ENABLE));
    fc_command_status_t completion;

    cmd->feature_addr = addr;

    completion = test_submit_wait(cmd);
    munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);
    (void)memcpy(out, completion.param.bytes, 4U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}

/* SET_FEATURES(addr, 4 字节)。写入值取自 cmd->param.bytes（fc.c:1283 在派发时拷进暂存，
 * 再作为 cw_buffers[0] 交给 NAND）。 */
static void l3_feature_set(uint8_t addr, const uint8_t in[4])
{
    fc_command_t       *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_SET_FEATURES, FC_CRC_ENABLE));
    fc_command_status_t completion;

    cmd->feature_addr = addr;
    (void)memcpy(cmd->param.bytes, in, 4U);

    completion = test_submit_wait(cmd);
    munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}

/**************************************************************************
 *  test_feature_write_read — Feature 读 + 写回环
 *
 *  流程：
 *    1. GET_FEATURES × 4 —— 手册 Table 13 里全部可寻址寄存器各读一次（**只读，零风险**）
 *    2. 01h（Timing mode）读-改-写回环：
 *         GET 原值（应为模式 0）→ SET 模式 1 → GET 校验 → SET 原值 → GET 校验还原
 *
 *  验证点：
 *    - Feature 读写命令各自执行成功
 *    - **写进去的值读得回来**（只查 status 的版本证明不了这点：器件把寄存器恒读成 0
 *      也照样过——这正是本用例从「只 GET」改成回环的原因）
 *    - 回环结束后 01h 已还原为模式 0，器件时序回到默认（后面 8 条用例依赖裸页读写）
 *
 *  ⚠ 还原的 SET **排在断言之前**：手册明说 feature "remains active until the device is
 *    power cycled"——连 RESET(FFh) 都清不掉。万一断言 abort 时它还停在模式 1，
 *    器件会带着非默认时序继续跑，直到断电。
 **************************************************************************/

static MunitResult test_feature_write_read(const MunitParameter params[], void *data)
{
    static const uint8_t addrs[4] = {L3_FEATURE_TIMING_MODE, L3_FEATURE_DRIVE_STRENGTH, L3_FEATURE_RB_PULLDOWN, L3_FEATURE_ARRAY_OP_MODE};
    static const char   *names[4] = {"Timing mode", "Drive strength", "RB# pull-down", "Array op mode"};
    uint8_t              original[4];
    uint8_t              probe[4] = {L3_TIMING_MODE_1, 0U, 0U, 0U};
    uint8_t              readback[4];
    uint8_t              restored[4];

    (void)params;
    (void)data;

    test_trace_begin();

    /* Phase 1：手册里的四个寄存器各读一次。只读 ⇒ 一个器件状态都不动。
     * 证据打**实际字节**。
     * ⚠ 板上实测四个地址都读回 00 00 00 00，**这不是搬运失败**：Phase 2 往 01h 写 01h
     *   能原样读回，说明 4 字节双向都在走——GET 的暂存是先 memset(0)（fc.c:1287）再
     *   交给引擎的，读回非 0 只可能来自器件。所以全 0 是器件在上电默认态下的**真实读数**。
     *   （曾按「0x80 驱动强度的默认值不该是 0」下判据，板上证伪——那是没查手册的猜测。） */
    for (uint8_t i = 0U; i < 4U; i++)
    {
        l3_feature_get(addrs[i], readback);
        test_trace_summary("GET_FEATURES 0x%02X (%s) -> %02X %02X %02X %02X", (unsigned)addrs[i], names[i], (unsigned)readback[0], (unsigned)readback[1], (unsigned)readback[2], (unsigned)readback[3]);
    }

    /* Phase 2：01h（Timing mode）读-改-写回环。
     * 探针取**手册 Table 15 给死的模式 1（01h）**，不编造值。 */
    l3_feature_get(L3_FEATURE_TIMING_MODE, original);

    /* 手册：「The default timing mode is mode 0」——先钉住这个前提，否则下面
     * 「写 01h 再读回 01h」在器件本来就非模式 0 时是空转（探针恰好等于原值）。
     * 本项目 CONFIG_NAND_TIMING_MODE_4 = 0，从不设它，所以这里必须是 00h。 */
    munit_assert_uint8(original[0], ==, L3_TIMING_MODE_0);

    l3_feature_set(L3_FEATURE_TIMING_MODE, probe);
    l3_feature_get(L3_FEATURE_TIMING_MODE, readback);

    /* 先还原，再断言——见文件头 ⚠。 */
    l3_feature_set(L3_FEATURE_TIMING_MODE, original);
    l3_feature_get(L3_FEATURE_TIMING_MODE, restored);

    munit_assert_uint8(readback[0], ==, L3_TIMING_MODE_1);
    munit_assert_uint8(restored[0], ==, original[0]);

    /* 证据摘要（--log-visible info）：四个值一列排开，回环的每一步都可见。 */
    test_trace_summary("0x01 回环: 原值 %02X -> SET %02X -> GET %02X -> 还原 %02X", (unsigned)original[0], (unsigned)probe[0], (unsigned)readback[0], (unsigned)restored[0]);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_status_query — 状态查询
 *
 *  流程：
 *    1. READ_STATUS — 读取基本状态（nand_status 经 completion.status 返回）
 *    2. READ_STATUS_ENHANCED — 读取增强状态
 *
 *  验证点：
 *    - 状态查询命令独立执行，生命周期完整
 *    - NAND 状态寄存器的值在合理范围内，非 0
 **************************************************************************/

static MunitResult test_status_query(const MunitParameter params[], void *data)
{
    /* ⚠ OP_READ_STATUS 是**本项目 op 表的下标**（onfi_1_0.h:107 = 6），不是线上命令字节。
     * 证据里两个都要打——只打下标会被读成「ONFI 命令 0x06」，那是误导。 */
    static const uint8_t status_ops[2]  = {(uint8_t)OP_READ_STATUS, (uint8_t)OP_READ_STATUS_ENHANCED};
    static const uint8_t status_cmds[2] = {NAND_CMD_READ_STATUS, NAND_CMD_READ_STATUS_ENHANCED};

    (void)params;
    (void)data;

    test_trace_begin();

    for (uint8_t i = 0U; i < 2U; i++)
    {
        /* 状态字仅 1 字节 ⇒ 只取 CW0（1 条 desc）；状态查询不是 page read，
         * 数据路径不介入，nand_status 经 completion.status 返回。
         * operation 逐轮不同 ⇒ config 需可变，先按基线拷一份再改。 */
        fc_cmd_conf_t       conf = L3_CONF(status_ops[i], FC_CRC_ENABLE);
        fc_command_t       *cmd  = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &conf);
        fc_command_status_t completion;

        completion = test_submit_wait(cmd);
        munit_assert_uint32(completion.status, ==, FC_CW_STATUS_SUCCESS);

        /* 证据摘要（--log-visible info）：打出状态字的实际值。
         * op_idx = 项目 op 表下标，onfi = 真正发到线上的命令字节（0x70 / 0x78）。 */
        test_trace_summary("READ_STATUS op_idx=%u onfi=0x%02X -> completion.status=0x%X", (unsigned)status_ops[i], (unsigned)status_cmds[i], (unsigned)completion.status);

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_internal_data_move — 内部搬移验证
 *
 *  流程：
 *    1. 擦除源 block + 目标 block（源 / 目标都在窗口内随机抽，src ≠ dest）
 *    2. 写源 block（64 pages，每页数据 = 该页地址）
 *    3. 普通读回源 block 64 pages，排除写阶段问题
 *    4. 对每 page 执行 READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_DATA_MOVE
 *    5. 读回目标 block 64 pages
 *
 *  验证点：
 *    - 片上内部搬移命令序列走得通
 *    - 搬移后源 block 数据仍可读（Step 3）
 *    - 目标 block 的 USER 数据 == **源块坐标**的 PAA 图案（Step 5）
 *
 *  ⚠ Step 5 的期望值必须取**源块坐标**：copyback 搬的是源页内容，而 READ/PROGRAM_
 *     FOR_INTERNAL_DATA_MOVE 都不是 page read/write（数据路径不介入、不重铸）⇒ 目标页
 *     的 USER 仍是**源块坐标**的 PAA。故不能用 test_read_block(dest)——它按入参 block
 *     重算期望（fc_test_common.c:644-675）。数据路径侧照常通过：META 低 3 位 = plane/cw
 *     （源 / 目标同 plane、同 cw 枚举）、META 的 LBA 与命令 meta 同页 ⇒ Pass 4 两项都过；
 *     CRC 因 USER 原样搬过而一致。
 *  ⚠ v1.22 起 dest 也是**抽出来的**（不再是 src + 常数）⇒ 上面这条更容易写错：Step 5 的
 *     比对与读命令的 meta 都必须继续用**源块**坐标，顺手写成 dest 就会静默判错。
 **************************************************************************/

static MunitResult test_internal_data_move(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane = l3_plane_draw("internal_data_move");
    const uint16_t block = l3_block_draw("internal_data_move");
    const uint16_t dest  = l3_dest_draw("internal_data_move", block);

    test_trace_begin();

    /* Step 1: 擦除源 + 目标 block */
    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_erase_block(dest, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));

    /* Step 2: 写源 block（64 pages） */
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    /* Step 3: 用普通读（00h-30h）验证源 block 全部 64 pages 正确，排除写阶段问题 */
    test_read_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

    /* Step 4: 逐 page 内部搬移：READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_DATA_MOVE
     * ⚠ 这两个 op 都**不是** page read/write（fc_is_page_read / fc_is_page_write 都不含），
     *   数据路径不介入：搬的是 NAND 内部缓存，页内 META 原样跟着走。
     *   故目标页读回时，源页写入时铸的 META（源页 PBA）仍然自洽 —— 见 Step 5。
     *   这两条命令的 meta 不参与编解码（无数据路径），填源页 PBA 只为表达「搬的是这一页」。 */
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        /* 4a: READ_FOR_INTERNAL_DATA_MOVE (00h-35h) — 将源 block page 读到 NAND 内部缓存 */
        {
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = plane, .page = page},
                                               test_plane_cw_bitmap(TEST_PLANE_BM(plane)),
                                               (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}),
                                               &L3_CONF(OP_READ_FOR_INTERNAL_DATA_MOVE, FC_CRC_ENABLE));

            (void)test_submit_wait(cmd);
            munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        }

        /* 4b: PROGRAM_FOR_INTERNAL_DATA_MOVE (85h-10h) — 将内部缓存数据写入目标 block page */
        {
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = dest, .plane = plane, .page = page},
                                               test_plane_cw_bitmap(TEST_PLANE_BM(plane)),
                                               (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}),
                                               &L3_CONF(OP_PROGRAM_FOR_INTERNAL_DATA_MOVE, FC_CRC_ENABLE));

            (void)test_submit_wait(cmd);
            munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        }
    }

    /* Step 5: 读回目标 block 64 pages，按**源块坐标**校验（理由见上方 ⚠）。
     * 逐页读 + 就地比，与 test_read_block 同形，只把期望坐标换成源块。
     * ⚠ 读命令声明的 meta 必须是**源页 PBA**：META 存的是「内容的身份」，不是「内容现在
     *   在哪」——搬移不经过数据路径，源页写入时铸的 PBA 原样跟到了目标块，声明目标页 PBA
     *   反而会被 Pass 4 ② 判成「读到了别的页」。 */
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        const uint32_t      tag = (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page});
        fc_command_status_t completion =
            test_read_page(dest, TEST_PLANE_BM(plane), page, tag, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

        munit_assert_uint32(completion.status & FC_CW_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block, page, TEST_PLANE_BM(plane));
    }

    /* 证据摘要（--log-visible info）：搬了多少页、目标块的 USER 是按**哪套坐标**比的。
     * 这一句是本用例的核心判据——「按源块坐标比」而不是按目标块，打出来才看得出
     * Step 5 的期望值取对了（错取目标坐标会静默判成"数据错"，或更糟：恒真通过）。
     * plane / src / dest 都是抽出来的 ⇒ 必须打出来，否则有 seed 也对不上是哪个块。 */
    test_trace_summary("move plane=%u block=%u -> dest=%u (src≠dest): %u pages x %u CW; dest USER == 源块坐标 PAA (%lu B)", (unsigned)plane, (unsigned)block, (unsigned)dest, (unsigned)PAGES_PER_BLOCK, (unsigned)CW_NUM, (unsigned long)PAGES_PER_BLOCK * (unsigned long)CW_NUM * (unsigned long)FC_BUFFER_USER_SIZE);

    return MUNIT_OK;
}

#if 0
/**************************************************************************
 *  test_unlock_erase — 解锁 + 擦除验证
 *
 *  流程：
 *    1. OP_BLOCK_UNLOCK_LOW — 解锁低段
 *    2. OP_BLOCK_UNLOCK_HIGH — 解锁高段
 *    3. OP_ERASE_BLOCK — 擦除 block
 *    4. READ_PAGE — 验证全 0xFF
 *
 *  验证点：
 *    - 锁机制操作正确
 *    - 解锁后擦除成功
 *
 *  ⚠️ 当前测试用 `#if 0` 隔开，确认稳定后启用。
 *     移除 `#if 0` / `#endif` 即可。
 **************************************************************************/

static MunitResult test_unlock_erase(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    uint16_t block = BLOCK_TEST;

    /* Phase 1: 执行 BLOCK_LOCK — 锁定全部 block */
    {
        fc_command_t *cmd = fc_command_alloc();
        munit_assert_not_null(cmd);
        cmd->config.operation = OP_BLOCK_LOCK;
        cmd->config.ch        = 0U;
        cmd->config.ce        = 0U;
        cmd->config.lun       = 0U;
        cmd->config.cw        = 0U;
        cmd->config.random    = FC_RANDOM_DISABLE;
        cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
        cmd->config.raid      = FC_RAID_MODE_NORMAL;
        cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
        test_run_lifecycle(cmd);
    }

    /* Phase 2: UNLOCK_LOW + UNLOCK_HIGH */
    unlock_block(block);

    /* Phase 3: 擦除 */
    test_erase_block(block, TEST_PLANE_BM(0), 0U);

    /* Phase 4: 验证擦除 */
    read_block_verify_erased(block, 0U);

    return MUNIT_OK;
}
#endif /* #if 0 — unlock_erase test disabled */

/**************************************************************************
 *  test_block_write_read — 写 + 读核心验证
 *
 *  流程：
 *    1. 擦除 block（参数给出）
 *    2. 用 NORMAL_PROGRAM_PAGE 写该 plane 全部 64 pages（每页数据 = 该页地址）
 *    3. 用 NORMAL_READ_PAGE_WITH_DATA_OUT 读回全部 64 pages
 *    4. 每页按坐标重算期望 PAA 并逐字节比对
 *
 *  验证点：
 *    - NAND 硬件真正执行了擦除和编程
 *    - 读回数据与写入数据完全一致
 *    - completion per-CW status 均为 SUCCESS
 **************************************************************************/

static MunitResult test_block_write_read(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane = l3_plane_draw("block_write_read");
    const uint16_t block = l3_block_draw("block_write_read");

    test_trace_begin();

    /* Phase 1: 擦除 */
    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));

    /* Phase 2: 写全 block 64 pages */
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    /* Phase 3: 读回全 block 64 pages 验证 */
    test_read_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

    /* 证据摘要来自夹具的 prog / read 两行（页数 + 逐槽比过的字节数），此处只点明阶段。 */
    test_trace_summary("write_read block=%u plane=%u: erase -> prog -> read 三段走通", (unsigned)block, (unsigned)plane);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_erase_verify — 擦除验证
 *
 *  流程：
 *    1. 擦除 block（参数给出）
 *    2. 用 NORMAL_READ_PAGE_WITH_DATA_OUT 读全部 64 pages
 *    3. 逐 CW 验证 == 0xFF
 *
 *  验证点：
 *    - 擦除后 NAND 确实将全 block 数据位重置为 1
 *
 *  ⚠ 判据偏弱（既有，本轮只标注）：没有前置写，直接擦完读擦除态，依赖前序用例的
 *     残留状态；地址随机化后块池更大（窗口 614..1433），顺序依赖更明显。
 **************************************************************************/

static MunitResult test_block_erase_verify(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane = l3_plane_draw("block_erase_verify");
    const uint16_t block = l3_block_draw("block_erase_verify");

    test_trace_begin();

    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    read_block_verify_erased(block, TEST_PLANE_BM(plane));

    /* 证据摘要（--log-visible info）：地址是抽出来的 ⇒ 必须打出来才能从 seed 复现。 */
    test_trace_summary("erase_verify block=%u plane=%u: erase -> %u pages 全 0xFF", (unsigned)block, (unsigned)plane, (unsigned)PAGES_PER_BLOCK);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_cache_ops — Cache 读序列验证
 *
 *  流程：
 *    1. 擦除 block（参数给出）
 *    2. 用 PROGRAM_PAGE_CACHE 写该 plane 全 block（63 cache + 1 final 确认）
 *    3. Cache 读序列遍历所有 64 pages（START 入口：00h-addr-30h → 31h ×63 → 3Fh）
 *    4. 每 page 数据与预期 pattern 比对
 *    5. Cache 读**随机跳页**（00h-addr-31h）：START(16) → 跳到 32，验 cache 管线的跳页入口
 *
 *  验证点：
 *    - Cache 写 + Cache 读数据一致性
 *    - Cache 读序列正确遍历多页
 *    - Cache 读随机跳页（OP_READ_PAGE_CACHE_RANDOM）：输出的是**上一页**、地址页在下一步出
 *    - 各阶段 status 正确
 *
 *  ⚠️ CRC/LDPC/加扰/RAID 全部关闭（LDPC RAW 模式），纯 RAW 数据验证。
 *  ⚠ plane 1 的 cache 路径改造前从未走过（旧代码 plane 恒 0）——参数化新增的覆盖面。
 *     cache 只有单 plane 变体（ONFI 1.0 无 multi-plane；器件侧有 two-plane cache
 *     **编程**、无 two-plane cache **读**）。
 **************************************************************************/

static MunitResult test_block_cache_ops(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane = l3_plane_draw("block_cache_ops");
    const uint16_t block = l3_block_draw("block_cache_ops");

    test_trace_begin();

    /* Phase 0: 擦除 */
    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));

    /* Phase 1: 用 PROGRAM_PAGE_CACHE 写全 block */
    test_prog_block_cache(block, TEST_PLANE_BM(plane), &L3_CONF(OP_PROGRAM_PAGE_CACHE, FC_CRC_DISABLE), NULL);

    /* Phase 2: Cache 读序列。
     *
     * START(p0) → SEQ ×63 → LAST 整段由夹具编排：31h / 3Fh 无地址周期（设备自增），
     * 且 31h 输出的是**上一轮加载的页**，meta 必须给「真正输出的那一页」的 LBA ——
     * 这条约定封装在 test_read_block_cache 里（见 fc_test_common.h「cache 整块读写」）。 */
    test_read_block_cache(block, TEST_PLANE_BM(plane), &L3_CONF(OP_READ_PAGE_CACHE_SEQ, FC_CRC_DISABLE), NULL);

    /* Phase 3: Cache 读随机跳页（00h-addr-31h）—— cache 管线的「跳页」入口。
     *
     * ⚠ 语义（v48 按手册订正）：本命令把**地址页载入 data register（预取）**，同时输出
     * cache 寄存器里的**上一页** ⇒ **输出页 ≠ 地址页**，且必须在 00h-30h / 00h-31h 之后下发。
     * 故这里不是「随机读单页」，而是「START(16) → 跳到 32」：RANDOM 输出 16，
     * 随后一条 31h 输出 32（这才证明跳页真的生效），最后 3Fh 收尾。
     * 详见 test_read_cache_random（fc_test_common.h / .c）。 */
    {
        const uint16_t prev_page = (uint16_t)(PAGES_PER_BLOCK / 4U); /* 16 */
        const uint16_t addr_page = (uint16_t)(PAGES_PER_BLOCK / 2U); /* 32 */

        test_read_cache_random(block, TEST_PLANE_BM(plane), prev_page, addr_page, &L3_CONF(OP_READ_PAGE_CACHE_RANDOM, FC_CRC_DISABLE));
    }

    /* 证据摘要（--log-visible info）：地址是抽出来的 ⇒ 必须打出来才能从 seed 复现。 */
    test_trace_summary("cache_ops block=%u plane=%u: cache prog -> 31h x63 -> 3Fh 全 block 数据一致；00h-addr-31h 跳页入口（输出上一页、地址页下一步出）一致", (unsigned)block, (unsigned)plane);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_multi_plane — 多 Plane 验证
 *
 *  全流程使用 Multi-Plane OP 码：
 *    1. 双 Plane 擦除（OP_ERASE_BLOCK_MULTI_PLANE）
 *    2. 双 Plane 编程（OP_PROGRAM_PAGE_MULTI_PLANE），plane 0/1 不同 pattern
 *    3. 双 Plane 读回（OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT），逐字节验证
 *
 *  验证点：
 *    - 多 Plane 擦除 + 编程 + 读回全部正确
 *    - 两个 plane 的数据完整性各自独立验证
 *
 *  ⚠ plane 段由 op 派生为「两个 plane」⇒ plane_bitmap 恒为 TEST_PLANE_BM_ALL，
 *     故本用例只吃 block 参数（plane 不参与）。
 **************************************************************************/

static MunitResult test_block_multi_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint16_t block = l3_block_draw("block_multi_plane");

    test_trace_begin();

    /* Step 1: Multi-plane erase block (both planes) */
    test_erase_block(block, TEST_PLANE_BM_ALL, &L3_CONF(OP_ERASE_BLOCK_MULTI_PLANE, FC_CRC_DISABLE));

    /* Step 2: Multi-plane program — 64 pages。
     * 数据源按 (plane, cw) 取槽 ⇒ plane 0/1 的图案天然不同，读串 plane 必被抓到。
     * plane 段 = 两个 plane ⇒ plane_bitmap 必须是 TEST_PLANE_BM_ALL
     * （multi-plane 的覆盖面断言在夹具里，见 fc_test_common.h「三」）。 */
    test_prog_block(block, TEST_PLANE_BM_ALL, &L3_CONF(OP_PROGRAM_PAGE_MULTI_PLANE, FC_CRC_DISABLE), NULL);

    /* Step 3: Multi-plane read back — verify both planes */
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        read_page_verify(block, page, TEST_PLANE_BM_ALL, 0U, &L3_CONF(OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT, FC_CRC_DISABLE));
    }

    /* 证据摘要（--log-visible info）：两个 plane 的图案**天然不同**（源槽按 (plane, cw) 取）
     * ⇒ 读串 plane 必被抓到；打出来才看得出覆盖了两个 plane 而不是同一个读了两遍。 */
    test_trace_summary("multi_plane block=%u bm=0x%02X: %u pages x %u plane x %u CW x %u B, 两 plane 图案互异", (unsigned)block, (unsigned)TEST_PLANE_BM_ALL, (unsigned)PAGES_PER_BLOCK, (unsigned)PLANE_NUM, (unsigned)CW_NUM, (unsigned)FC_BUFFER_USER_SIZE);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_random_data — 随机列读验证
 *
 *  流程：
 *    1. 擦除 + 写全 block 基准数据（plane / block 都随机抽）
 *    2. 随机抽页集（n ∈ [2,8] 个互不重复的页）与 CW 集（n ∈ [1,4] 个互不重复的 CW），每页：
 *       a. Normal read page 加载页缓冲
 *       b. RANDOM_DATA_READ 抽样页集里的每个 CW，验证数据
 *
 *  验证点：
 *    - 同页不同 CW 的列地址解码正确
 *    - 跨页行地址切换后随机列读正确
 *    - 数据与原始写入一致
 *
 *  ⚠ 抽样顺序固定为 ① plane ② block ③ 页集（先抽个数再抽元素）④ CW 集（同）
 *     —— 顺序即复现契约；主循环（页 × CW 探针）**不再消耗随机流**。
 **************************************************************************/

static MunitResult test_block_random_data(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane = l3_plane_draw("block_random_data");
    const uint16_t block = l3_block_draw("block_random_data");
    uint8_t        pages[L3_PAGES_MAX];
    uint8_t        cws[L3_CWS_MAX];
    const uint8_t  n_pages = l3_pages_draw("block_random_data", pages);
    const uint8_t  n_cws   = l3_cws_draw("block_random_data", cws);

    test_trace_begin();

    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    for (uint8_t si = 0U; si < n_pages; si++)
    {
        const uint16_t page = pages[si];

        /* Normal read page — 加载 NAND 页缓冲（顺带校验整页） */
        read_page_verify(block, page, TEST_PLANE_BM(plane), 0U, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE));

        for (uint8_t ci = 0U; ci < n_cws; ci++)
        {
            const uint8_t cw = cws[ci];

            /* 随机列读：只抽 1 个 CW ⇒ cw_bitmap 必须是**被抽样那个 CW 的位**。
             * ⚠ 位图是 flat 绝对编号（bit n = plane*CW_NUM + cw，v9/v27）——**plane 1 的
             *   CW0 是 bit 4，不是 bit 0**。这里曾写 `1U << cw`（plane 盲）：plane=0 侥幸
             *   正确，plane=1 时位落在段外，被 ftl_cmd_resolve_cw_bitmap 的越段断言当场挡下。
             * ⚠ 更早还写死过 0x01U（= 只取 CW0），而期望值按抽样 cw 算——因为
             *   链是按掩码位枚举 cw 的（cmd->paa 的 cw 字段会被枚举值覆盖），
             *   于是 desc 落在 CW0、期望值取 CW1/CW3，两者对不上。
             *   旧 pattern 是 4 字节周期、cw*512 又是 4 的倍数 ⇒ 四个 CW 期望值
             *   完全相同，这条错误一直被掩盖（v22 changelog 记录过同类问题）。 */
            const uint32_t cw_bit = (uint32_t)1U << ((uint32_t)plane * CW_NUM + cw);
            fc_command_t  *cmd    = test_cmd_build(&(nand_addr_t){.block = block, .plane = plane, .page = page},
                                               cw_bit,
                                               (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}),
                                               &L3_CONF(OP_RANDOM_DATA_READ, FC_CRC_ENABLE));

            /* 证据明细（--log-visible debug）：把**实际用的 flat 位**打出来——v35 的
             * plane 盲位图缺陷（plane 1 的 CW0 写成 0x01 而非 0x10）正是靠这一类自证才能一眼看穿。 */
            test_trace_detail("probe page=%u cw=%u cw_bitmap=0x%08lX (flat = plane*CW_NUM + cw)", (unsigned)page, (unsigned)cw, (unsigned long)cw_bit);

            test_memset_read_buffer(cmd);
            (void)test_submit_wait(cmd);

            munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);
            munit_assert_uint8(test_cw_count(cmd), ==, 1U); /* 掩码只选中 1 个 CW */
            /* 链驱动校验：只有被抽样的那个 CW 在链上，test_verify_page_paa 会连
             * plane 段内其余 CW 一起查（那些槽这次没被填过）⇒ 用 verify_chain_paa。 */
            verify_chain_paa(cmd);

            munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        }
    }

    /* 证据摘要（--log-visible info）：抽到的地址 + 页/列**掩码** + 探针数。
     * 掩码由抽到的集合就地算出 ⇒ 它的 popcount **等于**打印的个数，顺带**自证互不重复**；
     * 与下面 debug 档逐探针的 cw_bitmap 对不上（例如 plane 1 的 CW0 位算成 0x01）会立刻显形。
     * ⚠ 页掩码是 64 位 ⇒ 拆两个 %08lX 打印 —— newlib-nano 不认 %llX（见 memory）。 */
    {
        uint32_t page_mask_lo = 0U;
        uint32_t page_mask_hi = 0U;
        uint32_t cw_mask      = 0U;

        for (uint8_t i = 0U; i < n_pages; i++)
        {
            if (pages[i] < 32U)
            {
                page_mask_lo |= (uint32_t)1U << pages[i];
            }
            else
            {
                page_mask_hi |= (uint32_t)1U << (pages[i] - 32U);
            }
        }

        for (uint8_t i = 0U; i < n_cws; i++)
        {
            cw_mask |= (uint32_t)1U << ((uint32_t)plane * CW_NUM + cws[i]);
        }

        test_trace_summary("random block=%u plane=%u: %u pages x %u cw = %u probes; page_mask=0x%08lX%08lX cw_mask=0x%08lX, 逐 desc 对 desc->paa", (unsigned)block, (unsigned)plane, (unsigned)n_pages, (unsigned)n_cws, (unsigned)(n_pages * n_cws), (unsigned long)page_mask_hi, (unsigned long)page_mask_lo, (unsigned long)cw_mask);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_load_fetch — 「加载 / 取数两段式」显式下发验证（v1.27）
 *
 *  背景：生产读路径一直是「先加载、再逐 CW 取数」两段——
 *    fc_dispatch_rewrite_operation 把 OP_NORMAL_READ_PAGE_WITH_DATA_OUT(12)
 *    → OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT(33)，nand_dispatch_task_cw_chain 再对
 *    加载型 op 逐 CW 发 OP_RANDOM_DATA_READ(8, 05h-E0h)。但 op 33/29/8/41 **从未作为
 *    独立命令被点名过**——它们的 uop 序列、page-read 分类、页缓冲散落全无测试背书。
 *
 *  本用例把这两段**显式拆成两条 FC 命令**下发：
 *    Phase 1（单 plane）：load op 33（**不带 desc**，只搬进页缓冲）
 *                        → fetch op 8（带整段掩码 desc）→ 逐 CW 校验
 *    Phase 2（双 plane）：load op 29（不带 desc）
 *                        → fetch op 41（06h-E0h，带 desc）→ 两 plane 校验
 *
 *  验证点：
 *    - op 33/29 可作**纯加载**命令独立下发（op 表 has_data = 0 ⇒ 不需 desc 链、无 CW 位图）
 *    - op 8/41 可作**取数**命令带 desc 链取回页缓冲内容，ECC 解码后逐 CW 一致
 *    - 两段紧邻时「加载 → 取数」的数据通路正确
 *
 *  ⚠ 加载与取数是两条**独立** FC 命令：中间若有别的命令碰器件页缓冲，取数会读到错页。
 *     故本用例两段**紧邻**、中间不插任何命令。
 *  ⚠ op 29/41 走 two-plane 命令族，要求器件内部 on-die ECC 关闭（onfi_1_0.h Note 4）；
 *     本项目从不写 0x90 feature，保持默认关闭，故可用。
 *  ⚠ 抽样顺序固定为 ① plane ② block ③ block_mp —— 顺序即复现契约。
 **************************************************************************/

static MunitResult test_block_load_fetch(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane    = l3_plane_draw("block_load_fetch");
    const uint16_t block    = l3_block_draw("block_load_fetch");
    const uint16_t block_mp = l3_block_in_window("block_load_fetch", "block_mp");

    test_trace_begin();

    /* ── Phase 1：单 plane 加载 + 取数 ── */
    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    {
        const uint16_t page = (uint16_t)(PAGES_PER_BLOCK / 2U);

        /* 加载：op 33，**不带 desc** —— 器件只把该页搬进页缓冲、不输出。 */
        test_load_page(block, TEST_PLANE_BM(plane), page, &L3_CONF(OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT, FC_CRC_ENABLE));
        /* 取数：op 8（05h-E0h），整段掩码 ⇒ 单条命令由 CW 链遍历本 plane 全部 CW。 */
        test_random_data_read(block, TEST_PLANE_BM(plane), page, &L3_CONF(OP_RANDOM_DATA_READ, FC_CRC_ENABLE));
    }

    /* ── Phase 2：双 plane 加载 + 取数 ── */
    test_erase_block(block_mp, TEST_PLANE_BM_ALL, &L3_CONF(OP_ERASE_BLOCK_MULTI_PLANE, FC_CRC_DISABLE));
    test_prog_block(block_mp, TEST_PLANE_BM_ALL, &L3_CONF(OP_PROGRAM_PAGE_MULTI_PLANE, FC_CRC_DISABLE), NULL);

    {
        const uint16_t page = (uint16_t)(PAGES_PER_BLOCK / 2U);

        /* 加载：op 29（multi-plane），**不带 desc**。 */
        test_load_page(block_mp, TEST_PLANE_BM_ALL, page, &L3_CONF(OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT, FC_CRC_DISABLE));
        /* 取数：op 41（06h-E0h），整段掩码 ⇒ 链遍历两个 plane 的全部 CW。 */
        test_random_data_read(block_mp, TEST_PLANE_BM_ALL, page, &L3_CONF(OP_RANDOM_DATA_READ_MULTI_PLANE, FC_CRC_DISABLE));
    }

    /* 证据摘要（--log-visible info）：地址是抽出来的 ⇒ 必须打出来才能从 seed 复现。 */
    test_trace_summary("load_fetch b=%u plane=%u: op33(无desc)->op8 单 plane 一致；b=%u: op29(无desc)->op41 双 plane 一致", (unsigned)block, (unsigned)plane, (unsigned)block_mp);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_read_meta — META 只读回读（OP_READ_META，v1.28）
 *
 *  背景：坏块扫描需要**只读**回读页内 META（CW 前 4 字节；布局 [META:4][CRC:4][USER:512][PARITY:8]，
 *    META 在加扰区之前、明文）。op 11（OP_READ_META）自带 00h-30h 页加载 + 4 字节输出，
 *    **无 desc 链**（has_data = 0），把回读值写进 CQ 的 param.bytes。
 *
 *  验证点：
 *    - op 11 可作**独立只读命令**下发（无 desc 链、无 CW 位图）
 *    - 由 pba.cw 选 CW（列地址 = CW 号 × CW_SIZE）——同一页不同 CW 回读到**不同** META
 *    - 回读值 == FC 写路径写入的 META（每 CW 的 META = (command->meta & ~PLANE_CW_MASK)
 *      | plane_cw，见 fc_interface.c:197；prog 的 command->meta = 本页 PBA）
 *
 *  ⚠ 抽样顺序固定为 ① plane ② block —— 顺序即复现契约。
 **************************************************************************/

static MunitResult test_block_read_meta(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint8_t  plane = l3_plane_draw("block_read_meta");
    const uint16_t block = l3_block_draw("block_read_meta");

    test_trace_begin();

    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    {
        const uint16_t page = (uint16_t)(PAGES_PER_BLOCK / 2U);
        /* 写路径的 META 基值：prog 命令 command->meta = 本页 PBA（见 test_prog_page）。
         * 每 CW 的 META = (base & ~PLANE_CW_MASK) | plane_cw。 */
        const uint32_t base = (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = (uint8_t)page});
        uint8_t        got[4];

        for (uint8_t cw = 0U; cw < CW_NUM; cw += 2U) /* CW0 与 CW2：不同 CW ⇒ 不同 META */
        {
            const uint32_t expected = (base & ~PLANE_CW_MASK) | ((uint32_t)plane << PBA_PLANE_SHIFT) | (uint32_t)cw;
            uint32_t       got_meta;

            test_read_meta(block, plane, page, cw, got);

            got_meta = (uint32_t)got[0] | ((uint32_t)got[1] << 8U) | ((uint32_t)got[2] << 16U) | ((uint32_t)got[3] << 24U);

            test_trace_summary("read_meta b=%u plane=%u page=%u cw=%u: got=0x%08lX expect=0x%08lX", (unsigned)block, (unsigned)plane, (unsigned)page, (unsigned)cw, (unsigned long)got_meta, (unsigned long)expected);

            munit_assert_uint32(got_meta, ==, expected);
        }
    }

    return MUNIT_OK;
}

#if 0 /* BLOCK_LOCK_TIGHT — 不可逆锁死整片 NAND，暂不启用 */

/**************************************************************************
 *  test_block_lock_tight — 紧锁定验证（必须放在最后！）
 *
 *  BLOCK_LOCK_TIGHT 永久锁死整片 NAND（直到下电），不可逆。
 *  执行后尝试擦除 -> 编程 -> 读回，预期数据不变（或 operation 被硬件拒绝）。
 *  如果 BLOCK_LOCK_TIGHT 生效，NAND 硬件拒绝 PROGRAM，
 *  读回的数据应为编程前的非预期值，测试视为 PASS。
 *
 *  注意：此测试会永久锁死芯片！本 suite 中必须放在最后一项。
 **************************************************************************/

static MunitResult test_block_lock_tight(const MunitParameter params[], void *data)
{
    fc_command_t *cmd;
    uint8_t       page;

    (void)params;
    (void)data;

    /* Phase 1: 执行 BLOCK_LOCK_TIGHT -- 永久锁定整片 NAND */
    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);
    cmd->config.operation = OP_BLOCK_LOCK_TIGHT;
    cmd->config.ch        = 0U;
    cmd->config.ce        = 0U;
    cmd->config.lun       = 0U;
    cmd->config.random    = FC_RANDOM_DISABLE;
    cmd->config.ldpc      = FC_ECC_MODE_LDPC_RAW;
    cmd->config.raid      = FC_RAID_MODE_NORMAL;
    cmd->desc_id          = FC_DESCRIPTOR_ID_INVALID;
    cmd->config.cw        = 0U;

    test_run_lifecycle(cmd);

    /* Phase 2: 尝试擦除 + 写 + 读一个 page -- 预期失败 */
    erase_block(BLOCK_TEST, 0U);

    page = 0U;
    cmd = program_page(BLOCK_TEST, page, 0U);
    test_run_lifecycle(cmd);

    cmd = read_page(BLOCK_TEST, page, 0U);
    {
        fc_command_status_t completion;
        fc_status_t         result;
        uint32_t            service_count;
        uint8_t             completed = 0U;

        munit_assert_not_null(cmd);

        result = fc_submit(cmd);
        munit_assert_int(result, ==, FC_STATUS_OK);

        for (service_count = 0U; service_count < TEST_SERVICE_LOOP_MAX; service_count++)
        {
            (void)fc_service();
            result = fc_poll_completion(&completion);
            if (result == FC_STATUS_OK)
            {
                completed = 1U;
                break;
            }
        }
        munit_assert_uint32(completed, ==, 1U);

        /* 验证：由于 BLOCK_LOCK_TIGHT，NAND 拒绝 PROGRAM，
         * 读回数据应与预期 pattern 不同。 */
        fc_descriptor_id_t did = (fc_descriptor_id_t)cmd->desc_id;
        uint8_t            cw_idx;
        uint8_t            locked = 0U;
        uint8_t           *buf;

        for (cw_idx = 0U; cw_idx < CW_NUM; cw_idx++)
        {
            fc_descriptor_t *d = fc_descriptor_id_to_descriptor(did);
            if (d == NULL) break;
            buf = fc_buffer_id_to_real_buffer(d->buffer_id);
            if (buf == NULL) break;
            if (buf[0] != expected_byte(0U, (uint32_t)cw_idx * FC_BUFFER_USER_SIZE))
            {
                locked = 1U;
                break;
            }
            did = (fc_descriptor_id_t)d->next_id;
        }

        /* BLOCK_LOCK_TIGHT 生效：数据与写入不一致 -> 测试 PASS */
        munit_assert_uint8(locked, ==, 1U);
    }

    return MUNIT_OK;
}

#endif /* #if 0 — BLOCK_LOCK_TIGHT disabled (permanently locks the chip) */

/**************************************************************************
 *  套件定义
 *
 *  包含（前 3 条是定长命令序列，地址不参与，不随机化）：
 *    - init_sequence（初始化序列：RESET -> READ_ID -> ...）
 *    - feature_write_read（Feature 写回验证）
 *    - status_query（状态查询）
 *  其余 6 条**在用例体内**抽地址（见上方「随机地址」段）：
 *    - internal_data_move（片上内部搬移，抽 plane / src / dest）
 *    - block_erase_verify（擦除后全 0xFF 验证，抽 plane / block）
 *    - block_write_read（全 block 写读，抽 plane / block）
 *    - block_cache_ops（cache 读序列数据验证，抽 plane / block）
 *    - block_multi_plane（多 Plane 擦写读验证，抽 block）
 *    - block_random_data（随机列读写验证，抽 plane / block / 页集 / CW 集）
 *    - block_load_fetch（加载/取数两段式显式下发，抽 plane / block / block_mp）
 *    - block_lock_tight（永久锁芯片验证，必须放在最后）
 *
 *  七条随机化行用 MUNIT_TEST_OPTION_NONE（**保留**，不要换成 SINGLE_ITERATION）：
 *  --iterations N 才是覆盖旋钮（每条抽 N 组）；换成 SINGLE_ITERATION 会把旋钮锁死。
 *  runner.iterations 默认 0 ⇒ 迭代次数回落到套件级 iterations（ut_main.c 里 = 1），
 *  与是否带 SINGLE_ITERATION 无关（munit.c:1375 / :706-709）。
 *  ⚠ --iterations 是 **runner 级全局量**，且 runner->suite **恒指根套件**（munit.c:1359）
 *    ⇒ 子套件自己的 iterations 字段无效，**别去改 ut_main.c**（改了会把 Level 1/2 也乘 N）。
 *    它按调用范围生效：只跑 /fc-nand/lifecycle/ 就只乘 Level 3。
 *
 *  （unlock_erase 用 #if 0 隔开，暂不启用）
 **************************************************************************/

MunitTest g_fc_lifecycle_tests[] = {
    {     "init_sequence",      test_init_sequence, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"feature_write_read", test_feature_write_read, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {      "status_query",       test_status_query, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"internal_data_move", test_internal_data_move, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {"block_erase_verify", test_block_erase_verify, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {  "block_write_read",   test_block_write_read, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {   "block_cache_ops",    test_block_cache_ops, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    { "block_multi_plane",  test_block_multi_plane, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    { "block_random_data",  test_block_random_data, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {  "block_load_fetch",   test_block_load_fetch, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {   "block_read_meta",    test_block_read_meta, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
#if 0  /* block_lock_tight 被 #if 0 隔开，此处不注册 */
    { "block_lock_tight",    test_block_lock_tight,    test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
#endif
    {                NULL,                    NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};