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
 *           （v24/v25：命令携带的是 PBA，不是 PAA；地址一律用具名字段构造 ——
 *             PAA 侧见 nand_config.h 的 paa_from_addr，PBA 侧见 fc_test_common.h 的
 *             pba_from_addr 薄壳，v0.73 起生产侧只剩 pba_make/pba_pack）
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

#include "fc.h"
#include "fc_command.h"
#include "fc_queue.h"
#include "fc_scheduler.h"
#include "fc_test_common.h"
#include "ftl_fc_interface.h"
#include "log.h"
#include "munit.h"
#include "nand_config.h"
#include "onfi_1_0.h"

#include <string.h>

/*
 * 高速 mode（CONFIG_NAND_TIMING_MODE != 0）下跳过「依赖器件默认 Mode 0」的用例：
 *   - init_sequence：发 RESET(FFh) ⇒ 器件回默认 Mode 0，而 FMC 仍按 Mode 5 时序；
 *   - feature_write_read / chain_priority_lock：直接改写 timing mode，并断言默认 0。
 * 这些用例在「启动低速、运行高速」配置下自洽性不成立；Mode 0 回归仍照常执行。
 */
#if (CONFIG_NAND_TIMING_MODE != 0U)
#    define L3_SKIP_IF_HIGH_SPEED() return MUNIT_SKIP
#else
#    define L3_SKIP_IF_HIGH_SPEED() ((void)0)
#endif

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
 * （L2P 0..31 / 目录检查点 33,34 / BBT 36,39 / Checkpoint 37,38 / Journal 40,41，
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
#define L3_CONF(operation_, crc_) ((const fc_cmd_conf_t){.operation = (DWORD)(operation_), .crc = (crc_), .random = FC_RANDOM_DISABLE, .ldpc = FC_ECC_MODE_LDPC_RAW, .raid = FC_RAID_MODE_NORMAL})

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
        test_trace_detail("chain[%hhu] plane=%lu cw=%lu PAA=0x%08lX USER==PAA", i, ((desc->paa >> 2U) & 1U), (desc->paa & 3U), desc->paa);
    }
}

/**************************************************************************
 *  辅助：读一页 + 校验
 *
 *  meta 取本页 PBA，与写路径同口径 ⇒ Pass 4 ② 能抓到「读错页」。
 *  erased == 0：逐 desc 与「该 desc 的 PAA」比对；erased != 0：逐 desc USER 区全 0xFF。
 *
 *  ⚠ 擦除页的 META 是 0xFF…，数据路径 Pass 4 ①（自洽）必然不过 ⇒ 该 CW 带 MATA_ERROR
 *    位；但 descriptor 成功 = FC_CW_STATUS_UNUSED(0)，错误位单独置位
 *    成立（与改造前一致，不是本轮引入的）。也正因如此，擦除路径用
 *    `verify_cw_status_erased`（允许 CRC|MATA|EMPTY），不能用聚合错误掩码判据——
 *    那是 test_prog_block / test_read_block 的判据。
 **************************************************************************/

/* 擦除页裸读（LDPC RAW、CRC 开）的期望状态：
 * 整页不是有效码字 ⇒ CRC/META 天然置位；ECC 被跳过 ⇒ 不会出现 EMPTY。
 * 只要没有 FAIL / UNC / RAID（介质或数据路径真错误），就认为擦除态读回成立；
 * 数据全 0xFF 由 test_verify_erased 另行逐字节断言。 */
static uint8_t verify_cw_status_erased(const fc_command_t *cmd)
{
    const DWORD        allowed = FC_CW_STATUS_CRC_ERROR | FC_CW_STATUS_MATA_ERROR | FC_CW_STATUS_EMPTY;
    fc_descriptor_id_t desc_id = (fc_descriptor_id_t)cmd->desc_id;
    uint8_t            idx     = 0U;
    uint8_t            all_ok  = 1U;

    while ((desc_id != FC_DESCRIPTOR_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_descriptor_t *desc = fc_descriptor_id_to_descriptor(desc_id);

        munit_assert_not_null(desc);

        if ((desc->status & ~allowed) != 0U)
        {
            char stbuf[32];

            (void)fc_cw_status_str(desc->status, stbuf, sizeof(stbuf));
            munit_errorf("chain[%hhu] plane=%lu cw=%hu status=%s: erased-page read has unexpected error bits (allowed CRC|MATA|EMPTY)", idx, paa_to_plane(desc->paa), paa_to_cw(desc->paa), stbuf);
            all_ok = 0U;
        }

        desc_id = (fc_descriptor_id_t)desc->next_id;
        idx++;
    }

    return all_ok;
}

static void read_page_verify(uint16_t block, uint16_t page, uint8_t plane_bitmap, uint8_t erased, const fc_cmd_conf_t *config)
{
    fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = test_first_plane(plane_bitmap), .page = page}, test_plane_cw_bitmap(plane_bitmap), (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = test_first_plane(plane_bitmap), .page = page}), config);

    test_memset_read_buffer(cmd);
    (void)test_submit_wait(cmd);

    if (erased != 0U)
    {
        munit_assert_uint8(verify_cw_status_erased(cmd), ==, 1U);
        test_verify_erased(cmd);
    }
    else
    {
        munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);
        verify_chain_paa(cmd);
    }

    test_trace_detail("read p%02hu bm=0x%02hhX cw=%hhu %s", page, plane_bitmap, test_cw_count(cmd), (erased != 0U) ? "all-0xFF" : "PAA-verified");

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
    test_trace_summary("erased block=%hu bm=0x%02hhX: %d pages, %lu B all 0xFF (逐 CW SUCCESS)", block, plane_bitmap, PAGES_PER_BLOCK, (unsigned long)PAGES_PER_BLOCK * (unsigned long)test_plane_count(plane_bitmap) * (unsigned long)CW_NUM * (unsigned long)FC_BUFFER_USER_SIZE);
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
#    define CMD_FIELD_U(field)                                                                                            \
        do                                                                                                                \
        {                                                                                                                 \
            DWORD    before_v = before->field;                                                                            \
            DWORD    after_v  = after->field;                                                                             \
            uint32_t changed  = (before_v != after_v) ? 1U : 0U;                                                          \
            diffs += changed;                                                                                             \
            checked++;                                                                                                    \
            LOGW("[DBG]   %-18s %-10lu -> %-10lu%s\r\n", #field, before_v, after_v, (changed != 0U) ? "  <== DIFF" : ""); \
        } while (0)

static uint32_t cmd_diff_report(const fc_command_t *before, const fc_command_t *after, const char *tag)
{
    uint32_t diffs      = 0U;
    uint32_t checked    = 0U;
    uint32_t word_front = 0U;
    uint32_t word_back  = 0U;

    LOGW("[DBG] %s: cmd=%p id=%hu op=%hhu\r\n",
             tag,
             (const void *)after,
             after->command_id,
             after->config.operation);

    CMD_FIELD_U(command_id);
    CMD_FIELD_U(desc_id);
    CMD_FIELD_U(high_priority);
    CMD_FIELD_U(op_tag);
    CMD_FIELD_U(feature_addr);
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
 *    5. READ_UNIQUE_ID — 读取出厂唯一 ID（EDh，v1.36 补测）
 *
 *  验证点：
 *    - 各命令独立执行，生命周期完整
 *    - 读取的 ID 与预期值一致
 **************************************************************************/

static MunitResult test_init_sequence(const MunitParameter params[], void *data)
{
    uint8_t id_bytes[5]   = {0U, 0U, 0U, 0U, 0U};
    uint8_t onfi_bytes[4] = {0U, 0U, 0U, 0U};
    uint8_t sig_bytes[4]  = {0U, 0U, 0U, 0U};
    uint8_t uid_bytes[16] = {0U};

    (void)params;
    (void)data;

    L3_SKIP_IF_HIGH_SPEED();

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

    /* Step 4: READ_ONFI_SIGNATURE (90h + 地址 20h) — 与 Step 3 的 EC 路径**同名不同路**：
     * Step 3 走 ECh、有 desc 链、字节落 USER 缓冲；本步走 90h、**无 desc 链**（has_data = 0
     * ⇒ cw_bitmap = 0 走短路分支）、4 字节经控制器侧暂存 g_fc_return_staging 落 CQ 的 param。
     * 列地址 0x20 由 NAND 侧 req_onfi_signature 硬编码（手册 Figure 27）。 */
    {
        fc_command_t       *cmd        = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_READ_ONFI_SIGNATURE, FC_CRC_ENABLE));
        fc_command_status_t completion = test_submit_wait(cmd);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        munit_assert_uint8(completion.param.bytes[0], ==, 0x4FU); /* 'O' */
        munit_assert_uint8(completion.param.bytes[1], ==, 0x4EU); /* 'N' */
        munit_assert_uint8(completion.param.bytes[2], ==, 0x46U); /* 'F' */
        munit_assert_uint8(completion.param.bytes[3], ==, 0x49U); /* 'I' */

        for (uint8_t i = 0U; i < 4U; i++)
        {
            sig_bytes[i] = completion.param.bytes[i];
        }

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* Step 5: GET_FEATURES (address 0x01) — 寄存器值经 CQ 的 param 联合体返回
     * （completion.param.parameter0..3），不再原地回填命令体。
     * GET_FEATURES 不建链（ftl_cmd_needs_desc_chain 排除），pba 与掩码都不看
     * ⇒ cw_bitmap = 0。feature_addr 由调用者建链后自行覆盖。 */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_GET_FEATURES, FC_CRC_ENABLE));

        cmd->feature_addr = 0x01U;

        munit_assert_uint32(test_submit_wait(cmd).status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* Step 6: READ_UNIQUE_ID (EDh) — 与参数页共用同一 UOP 模板，只换命令字节；
     * 读 256B 进 CW0（只取 CW0，位图 0x01）。
     * ⚠ 唯一 ID 是**出厂烧录的 16 字节**，本器件手册未给死取值 ⇒ **不假设内容**
     *   （见「协议断言必须先查证」），硬判据只到「命令 + CW 状态全 SUCCESS」；
     *   16 字节走证据行打出，供人工比对（全 0xFF 即视为器件未实现该 ID）。 */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &L3_CONF(OP_READ_UNIQUE_ID, FC_CRC_ENABLE));

        (void)test_submit_wait(cmd);

        munit_assert_uint8(test_verify_cw_status_all_success(cmd), ==, 1U);

        uint8_t *buf = test_cw_user(cmd, 0U);

        munit_assert_not_null(buf);

        for (uint8_t i = 0U; i < 16U; i++)
        {
            uid_bytes[i] = buf[i];
        }

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    /* 证据摘要（--log-visible info）：六条命令的**实读字节**。
     * 裸 [ OK ] 只说明命令回来了；打出 ID / ONFI 签名 / 唯一 ID 才说明 NAND 真的答了话。
     * PARAM_PAGE 与 SIG 是**同一份签名的两条路径**（ECh 走 desc 链落 USER / 90h+20h 走 CQ param）。 */
    test_trace_summary("RESET ok; READ_ID=%02hhX %02hhX %02hhX %02hhX %02hhX; PARAM_PAGE=%02hhX %02hhX %02hhX %02hhX (%c%c%c%c); GET_FEATURES(0x01) ok; SIG(90h+20h)=%02hhX %02hhX %02hhX %02hhX (%c%c%c%c)", id_bytes[0], id_bytes[1], id_bytes[2], id_bytes[3], id_bytes[4], onfi_bytes[0], onfi_bytes[1], onfi_bytes[2], onfi_bytes[3], (char)onfi_bytes[0], (char)onfi_bytes[1], (char)onfi_bytes[2], (char)onfi_bytes[3], sig_bytes[0], sig_bytes[1], sig_bytes[2], sig_bytes[3], (char)sig_bytes[0], (char)sig_bytes[1], (char)sig_bytes[2], (char)sig_bytes[3]);

    test_trace_summary("UNIQUE_ID(16B)=%02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX", uid_bytes[0], uid_bytes[1], uid_bytes[2], uid_bytes[3], uid_bytes[4], uid_bytes[5], uid_bytes[6], uid_bytes[7], uid_bytes[8], uid_bytes[9], uid_bytes[10], uid_bytes[11], uid_bytes[12], uid_bytes[13], uid_bytes[14], uid_bytes[15]);

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
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
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
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

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

    L3_SKIP_IF_HIGH_SPEED();

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
        test_trace_summary("GET_FEATURES 0x%02hhX (%s) -> %02hhX %02hhX %02hhX %02hhX", addrs[i], names[i], readback[0], readback[1], readback[2], readback[3]);
    }

    /* Phase 2：01h（Timing mode）读-改-写回环。
     * 探针取**手册 Table 15 给死的模式 1（01h）**，不编造值。 */
    l3_feature_get(L3_FEATURE_TIMING_MODE, original);

    /* 手册：「The default timing mode is mode 0」——先钉住这个前提，否则下面
     * 「写 01h 再读回 01h」在器件本来就非模式 0 时是空转（探针恰好等于原值）。
     * 本用例只在 Mode 0 下执行（Mode 5 由 L3_SKIP_IF_HIGH_SPEED 跳过），所以这里必须是 00h。 */
    munit_assert_uint8(original[0], ==, L3_TIMING_MODE_0);

    l3_feature_set(L3_FEATURE_TIMING_MODE, probe);
    l3_feature_get(L3_FEATURE_TIMING_MODE, readback);

    /* 先还原，再断言——见文件头 ⚠。 */
    l3_feature_set(L3_FEATURE_TIMING_MODE, original);
    l3_feature_get(L3_FEATURE_TIMING_MODE, restored);

    munit_assert_uint8(readback[0], ==, L3_TIMING_MODE_1);
    munit_assert_uint8(restored[0], ==, original[0]);

    /* 证据摘要（--log-visible info）：四个值一列排开，回环的每一步都可见。 */
    test_trace_summary("0x01 回环: 原值 %02hhX -> SET %02hhX -> GET %02hhX -> 还原 %02hhX", original[0], probe[0], readback[0], restored[0]);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_chain_priority_lock — 高优先队列 × 命令链：链未结束期间独占该 LUN
 *
 *  背景：一条链可能长于 NAND per-LUN FIFO 深（FC_SCHEDULER_CHAIN_MAX = 4），会被拆成
 *  多批、分多次填充 pass 下发。批与批之间若没有锁，另一条队列（HSQ）的命令就会插进链的
 *  已下发部分与未下发部分之间。本用例钉死**链锁**：链一旦开始，该 LUN 的填充权归链所在
 *  队列独占，直到链的 END 下发（见 fc_scheduler.h 的 fc_chain_owner_t、fc.c 的
 *  fc_poll_fill_queue 准入/更新两处）。
 *
 *  流程（x = 01h、y = 02h，都在手册 Table 15 的 00h..05h 内）：
 *    Step 0  GET 原值，钉住默认模式 0
 *    Step 1  SQ 单命令基线：set0(x) → get0，读回 x
 *    Step 2  HSQ 真会插队：set0(x) → get0 → set1(y) 一起提交
 *              ⇒ y 先落盘、被随后的 set0(x) 覆盖 ⇒ get0 与终值都是 x
 *    Step 3  ★ 链锁：连续提交 8 条 SQ 链（前 7 条 chain=MORE、末条 chain=END，set/get
 *              交替），fc_service() 一次让前 4 条进 FIFO（chain_owner 转 SQ）；再往 HSQ
 *              投 set1(y)/get1(y)（同 LUN）——链没结束 ⇒ 必须被挡到链尾之后；抽干后
 *              get1 读到 y、终值也是 y
 *    Step 4  还原 01h 为原值
 *
 *  判据为什么有区分力：**没有链锁**时，HSQ 的 y 会插在链的第 4、5 条之间，链的后 4 条
 *  （含最后一次 set0(x)）在 y 之后执行 ⇒ 终值 **x**。有锁 ⇒ 终值 **y**。
 *
 *  ⚠ 还原的 SET 排在断言之前（同 test_feature_write_read）：feature 断电才复位，断言
 *    abort 时器件不该停在非默认时序。
 **************************************************************************/

/* ── 本用例的在途登记表 ──
 * 命令按提交顺序登记；抽干时按 command_id 配回槽位（CQ 完成项只带 id，不带指针），
 * GET 的读回值写进槽。与 v50 mixed_pipeline 的 CID 配对同路。 */
#define L3_PRIO_SLOTS (16U)

typedef struct
{
    fc_command_t *cmd;
    uint8_t       is_get; /* 1 = GET（收 param.bytes），0 = SET */
    uint8_t       out[4]; /* GET 的读回值 */
} l3_prio_slot_t;

static l3_prio_slot_t g_l3_prio_slots[L3_PRIO_SLOTS];
static uint8_t        g_l3_prio_used;

static void l3_prio_reset(void)
{
    (void)memset(g_l3_prio_slots, 0, sizeof(g_l3_prio_slots));
    g_l3_prio_used = 0U;
}

/* 建一条 feature 命令（SET/GET），设 high_priority 与 config.chain 后**提交**，登记进槽表。
 * 与 l3_feature_get/set 的区别：那两条是**同步**的（建完即 test_submit_wait + release），
 * 本组用例要先把多条命令按顺序排进队列、再一起抽干，所以「建+提交」与「收」必须分开。
 * 返回槽位下标。 */
static uint8_t l3_prio_submit(uint8_t operation, uint8_t addr, const uint8_t in[4], uint8_t priority, uint8_t chain)
{
    fc_cmd_conf_t conf = L3_CONF(operation, FC_CRC_ENABLE);
    fc_command_t *cmd;

    munit_assert_uint32(g_l3_prio_used, <, L3_PRIO_SLOTS);

    conf.chain = (DWORD)chain;
    cmd        = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &conf);

    cmd->feature_addr  = addr;
    cmd->high_priority = (DWORD)priority; /* 提交前设——fc_submit 按它选 SQ/HSQ */
    if ((operation == (uint8_t)OP_SET_FEATURES) && (in != NULL))
    {
        (void)memcpy(cmd->param.bytes, in, 4U);
    }

    munit_assert_int(fc_submit(cmd), ==, FC_STATUS_OK);

    g_l3_prio_slots[g_l3_prio_used].cmd    = cmd;
    g_l3_prio_slots[g_l3_prio_used].is_get = (operation == (uint8_t)OP_GET_FEATURES) ? 1U : 0U;
    return g_l3_prio_used++;
}

/* 抽干槽表里全部命令：fc_service() 反复推进，收到一条完成即按 command_id 配回槽位、
 * GET 存值、release（feature 无 desc 链，release 只还命令槽）。 */
static void l3_prio_drain(void)
{
    uint8_t outstanding = g_l3_prio_used;

    for (uint32_t i = 0U; (outstanding != 0U) && (i < TEST_SERVICE_LOOP_MAX); i++)
    {
        fc_command_status_t completion;

        (void)fc_service();

        while (fc_poll_completion(&completion) == FC_STATUS_OK)
        {
            uint8_t matched = 0U;

            munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

            for (uint8_t j = 0U; j < g_l3_prio_used; j++)
            {
                fc_command_id_t id = FC_COMMAND_ID_INVALID;

                if (g_l3_prio_slots[j].cmd == NULL)
                {
                    continue;
                }
                munit_assert_uint32(fc_command_to_id(g_l3_prio_slots[j].cmd, &id), ==, 1U);
                if (id == completion.command_id)
                {
                    if (g_l3_prio_slots[j].is_get != 0U)
                    {
                        (void)memcpy(g_l3_prio_slots[j].out, completion.param.bytes, 4U);
                    }
                    munit_assert_int(fc_command_release(g_l3_prio_slots[j].cmd), ==, FC_STATUS_OK);
                    g_l3_prio_slots[j].cmd = NULL;
                    outstanding--;
                    matched = 1U;
                    break;
                }
            }
            munit_assert_uint32(matched, ==, 1U);
        }
    }

    munit_assert_uint32(outstanding, ==, 0U);
}

static MunitResult test_chain_priority_lock(const MunitParameter params[], void *data)
{
    static const uint8_t x[4] = {0x01U, 0U, 0U, 0U}; /* 手册 Table 15 模式 1 */
    static const uint8_t y[4] = {0x02U, 0U, 0U, 0U}; /* 模式 2 */
    uint8_t              original[4];
    uint8_t              final[4];
    uint8_t              restored[4];
    uint8_t              hsq_get;
    uint8_t              sq_get;

    (void)params;
    (void)data;

    L3_SKIP_IF_HIGH_SPEED();

    test_trace_begin();

    /* ── Step 0：钉住默认模式 0（手册 "The default timing mode is mode 0"） ── */
    l3_feature_get(L3_FEATURE_TIMING_MODE, original);
    munit_assert_uint8(original[0], ==, L3_TIMING_MODE_0);

    /* ── Step 1：SQ 单命令基线 ── */
    l3_prio_reset();
    (void)l3_prio_submit(OP_SET_FEATURES, L3_FEATURE_TIMING_MODE, x, 0U, (uint8_t)FC_CHAIN_END);
    sq_get = l3_prio_submit(OP_GET_FEATURES, L3_FEATURE_TIMING_MODE, NULL, 0U, (uint8_t)FC_CHAIN_END);
    l3_prio_drain();
    munit_assert_uint8(g_l3_prio_slots[sq_get].out[0], ==, x[0]);
    test_trace_summary("step1 SQ 基线: set(%02hhX) -> get 读回 %02hhX", x[0], g_l3_prio_slots[sq_get].out[0]);

    /* ── Step 2：HSQ 真会插队（set1(y) 抢在 SQ 的 set0(x) 之前落盘） ── */
    l3_prio_reset();
    (void)l3_prio_submit(OP_SET_FEATURES, L3_FEATURE_TIMING_MODE, x, 0U, (uint8_t)FC_CHAIN_END);
    sq_get = l3_prio_submit(OP_GET_FEATURES, L3_FEATURE_TIMING_MODE, NULL, 0U, (uint8_t)FC_CHAIN_END);
    (void)l3_prio_submit(OP_SET_FEATURES, L3_FEATURE_TIMING_MODE, y, 1U, (uint8_t)FC_CHAIN_END);
    l3_prio_drain();
    munit_assert_uint8(g_l3_prio_slots[sq_get].out[0], ==, x[0]); /* y 先落、被 x 覆盖 ⇒ 读回 x */
    l3_feature_get(L3_FEATURE_TIMING_MODE, final);
    munit_assert_uint8(final[0], ==, x[0]);
    test_trace_summary("step2 HSQ 插队: get 读回 %02hhX（set1(%02hhX) 先落盘后被 set0(%02hhX) 覆盖）", g_l3_prio_slots[sq_get].out[0], y[0], x[0]);

    /* ── Step 3：★ 链锁 —— 8 命令 SQ 链（前 7 条 MORE、末条 END），HSQ 插不进 ── */
    l3_prio_reset();
    for (uint8_t k = 0U; k < 8U; k++)
    {
        const uint8_t chain_bit = (k < 7U) ? (uint8_t)FC_CHAIN_MORE : (uint8_t)FC_CHAIN_END;

        if ((k % 2U) == 0U)
        {
            (void)l3_prio_submit(OP_SET_FEATURES, L3_FEATURE_TIMING_MODE, x, 0U, chain_bit);
        }
        else
        {
            (void)l3_prio_submit(OP_GET_FEATURES, L3_FEATURE_TIMING_MODE, NULL, 0U, chain_bit);
        }
    }

    /* 一次 fc_service()：前 4 条进 per-LUN FIFO（chain_owner 转 SQ），后 4 条留在 SQ。 */
    (void)fc_service();

    /* 链没结束就往 HSQ 投同 LUN 的 set(y)/get(y) —— 链锁必须把它们挡到链尾之后。 */
    (void)l3_prio_submit(OP_SET_FEATURES, L3_FEATURE_TIMING_MODE, y, 1U, (uint8_t)FC_CHAIN_END);
    hsq_get = l3_prio_submit(OP_GET_FEATURES, L3_FEATURE_TIMING_MODE, NULL, 1U, (uint8_t)FC_CHAIN_END);

    l3_prio_drain();

    /* 链的后 4 条（含最后一次 set0(x)）先跑完，HSQ 的 set1(y)/get1(y) 才轮到。 */
    munit_assert_uint8(g_l3_prio_slots[hsq_get].out[0], ==, y[0]);

    /* ── Step 4：读终值 → **先还原** → 再断言（见用例头 ⚠） ── */
    l3_feature_get(L3_FEATURE_TIMING_MODE, final);
    l3_feature_set(L3_FEATURE_TIMING_MODE, original);
    l3_feature_get(L3_FEATURE_TIMING_MODE, restored);

    munit_assert_uint8(final[0], ==, y[0]); /* ★ 判据：链没被插入 ⇒ 终值是 y 不是 x */
    munit_assert_uint8(restored[0], ==, original[0]);

    test_trace_summary("step3 链锁: HSQ get 读回 %02hhX、终值 %02hhX（无锁时终值会是 %02hhX=x）", g_l3_prio_slots[hsq_get].out[0], final[0], x[0]);

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
        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

        /* 证据摘要（--log-visible info）：打出状态字的实际值。
         * op_idx = 项目 op 表下标，onfi = 真正发到线上的命令字节（0x70 / 0x78）。 */
        test_trace_summary("READ_STATUS op_idx=%hhu onfi=0x%02hhX -> completion.status=0x%hX", status_ops[i], status_cmds[i], completion.status);

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
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = plane, .page = page}, test_plane_cw_bitmap(TEST_PLANE_BM(plane)), (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}), &L3_CONF(OP_READ_FOR_INTERNAL_DATA_MOVE, FC_CRC_ENABLE));

            (void)test_submit_wait(cmd);
            munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        }

        /* 4b: PROGRAM_FOR_INTERNAL_DATA_MOVE (85h-10h) — 将内部缓存数据写入目标 block page */
        {
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = dest, .plane = plane, .page = page}, test_plane_cw_bitmap(TEST_PLANE_BM(plane)), (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}), &L3_CONF(OP_PROGRAM_FOR_INTERNAL_DATA_MOVE, FC_CRC_ENABLE));

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
        const uint32_t      tag        = (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page});
        fc_command_status_t completion = test_read_page(dest, TEST_PLANE_BM(plane), page, tag, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block, page, TEST_PLANE_BM(plane));
    }

    /* 证据摘要（--log-visible info）：搬了多少页、目标块的 USER 是按**哪套坐标**比的。
     * 这一句是本用例的核心判据——「按源块坐标比」而不是按目标块，打出来才看得出
     * Step 5 的期望值取对了（错取目标坐标会静默判成"数据错"，或更糟：恒真通过）。
     * plane / src / dest 都是抽出来的 ⇒ 必须打出来，否则有 seed 也对不上是哪个块。 */
    test_trace_summary("move plane=%hhu block=%hu -> dest=%hu (src≠dest): %d pages x %u CW; dest USER == 源块坐标 PAA (%lu B)", plane, block, dest, PAGES_PER_BLOCK, CW_NUM, (unsigned long)PAGES_PER_BLOCK * (unsigned long)CW_NUM * (unsigned long)FC_BUFFER_USER_SIZE);

    return MUNIT_OK;
}

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
    test_trace_summary("write_read block=%hu plane=%hhu: erase -> prog -> read 三段走通", block, plane);

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
    test_trace_summary("erase_verify block=%hu plane=%hhu: erase -> %d pages 全 0xFF", block, plane, PAGES_PER_BLOCK);

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
 *     ONFI 1.0 只有单 plane；本器件 MT29F4G08 另有 two-plane cache 读/写扩展，
 *     由 block_cache_multi_plane 单独覆盖。
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
    test_trace_summary("cache_ops block=%hu plane=%hhu: START_SEQ -> 31h x62 -> 3Fh 全 block 数据一致；00h-addr-31h 跳页入口（输出上一页、地址页下一步出）一致", block, plane);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_cache_multi_plane — 双 Plane cache 读写
 *
 *  流程：
 *    1. OP_ERASE_BLOCK_MULTI_PLANE 擦双 plane
 *    2. test_prog_block_cache_multi_plane：
 *       page 0..62 OP_TWO_PLANE_PROGRAM_CACHE（80h-11h-80h-15h）
 *       page 63    OP_PROGRAM_PAGE_MULTI_PLANE（80h-11h-80h-10h）
 *    3. test_read_block_cache_multi_plane：
 *       START_SEQ_MP(page0) -> SEQ_MP ×62 -> LAST_MP(page63)
 *       每步 06h P0/P1 分别输出两个 plane
 *
 *  验证点：
 *    - 双 plane cache 写的中间页/末页序列正确
 *    - 双 plane cache 读的 06h plane 选择正确
 *    - 两 plane 的 PAA pattern 各自独立校验（读串 plane 必被抓）
 **************************************************************************/

/*  test_block_cache_multi_plane_cross_block — 两 plane 分属不同 block 的双 plane cache 读写
 *
 *  与 block_cache_multi_plane 的唯一差别：plane0 / plane1 各写一个 block。
 *  这不是特例——生产 FTL（Policy B）的每个 pair 两 plane 都在不同 gblock 上；
 *  同块版覆盖不到「跨块时 05h-E0h plane 选择 + 31h 预取推进是否仍按页 +1」。
 *
 *  判据：两 plane 的 USER 区逐 CW 等于**各自 block** 的 PAA 图案（读串 plane / 读错页当场红）。
 */
static MunitResult test_block_cache_multi_plane_cross_block(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    uint16_t blocks[PLANE_NUM];

    blocks[0] = l3_block_draw("block_cache_multi_plane_cross_block");
    blocks[1] = l3_dest_draw("block_cache_multi_plane_cross_block", blocks[0]);

    test_trace_begin();

    test_erase_block(blocks[0], TEST_PLANE_BM(0), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_DISABLE));
    test_erase_block(blocks[1], TEST_PLANE_BM(1), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_DISABLE));

    test_prog_block_cache_multi_plane_pair(blocks, &L3_CONF(OP_TWO_PLANE_PROGRAM_CACHE, FC_CRC_DISABLE), NULL);
    test_read_block_cache_multi_plane_pair(blocks, &L3_CONF(OP_READ_PAGE_CACHE_START_SEQ_MP, FC_CRC_DISABLE));

    test_trace_summary("cache_multi_plane_cross_block: plane0 blk=%hu / plane1 blk=%hu; 63 x two-plane 15h + 1 x 10h; START_SEQ_MP + 62 x SEQ_MP + LAST_MP, both planes PAA-verified", blocks[0], blocks[1]);

    return MUNIT_OK;
}

static MunitResult test_block_cache_multi_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    const uint16_t block = l3_block_draw("block_cache_multi_plane");

    test_trace_begin();

    test_erase_block(block, TEST_PLANE_BM_ALL, &L3_CONF(OP_ERASE_BLOCK_MULTI_PLANE, FC_CRC_DISABLE));

    test_prog_block_cache_multi_plane(block, &L3_CONF(OP_TWO_PLANE_PROGRAM_CACHE, FC_CRC_DISABLE), NULL);

    test_read_block_cache_multi_plane(block, &L3_CONF(OP_READ_PAGE_CACHE_START_SEQ_MP, FC_CRC_DISABLE), NULL);

    test_trace_summary("cache_multi_plane block=%hu: 63 x two-plane 15h + 1 x 10h; START_SEQ_MP + 62 x SEQ_MP + LAST_MP, both planes PAA-verified", block);

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
    test_trace_summary("multi_plane block=%hu bm=0x%02hhX: %d pages x %d plane x %u CW x %u B, 两 plane 图案互异", block, TEST_PLANE_BM_ALL, PAGES_PER_BLOCK, PLANE_NUM, CW_NUM, FC_BUFFER_USER_SIZE);

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
            fc_command_t  *cmd    = test_cmd_build(&(nand_addr_t){.block = block, .plane = plane, .page = page}, cw_bit, (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}), &L3_CONF(OP_RANDOM_DATA_READ, FC_CRC_ENABLE));

            /* 证据明细（--log-visible debug）：把**实际用的 flat 位**打出来——v35 的
             * plane 盲位图缺陷（plane 1 的 CW0 写成 0x01 而非 0x10）正是靠这一类自证才能一眼看穿。 */
            test_trace_detail("probe page=%hu cw=%hhu cw_bitmap=0x%08lX (flat = plane*CW_NUM + cw)", page, cw, (unsigned long)cw_bit);

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
     * ⚠ 页掩码是 64 位 ⇒ 拆高/低两个 32 位半字分别打印。 */
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

        test_trace_summary("random block=%hu plane=%hhu: %hhu pages x %hhu cw = %d probes; page_mask=0x%08lX%08lX cw_mask=0x%08lX, 逐 desc 对 desc->paa", block, plane, n_pages, n_cws, (n_pages * n_cws), page_mask_hi, page_mask_lo, cw_mask);
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
    test_trace_summary("load_fetch b=%hu plane=%hhu: op33(无desc)->op8 单 plane 一致；b=%hu: op29(无desc)->op41 双 plane 一致", block, plane, block_mp);

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
 *    - **链内 META 复核（v1.36）**：另走一条**普通读**（有 desc 链），读回后从 CW 基址
 *      offset 0 经 `fc_buffer_get_meta(cmd, cw)` 取 META，与上面 op 11 的结果逐位一致——
 *      这正是 gc.c 的真实消费路径（读页 → get_meta(cmd,0) → 4 字节扇区地址 → >> CW_WIDTH）。
 *      两条路径互为交叉检查：一条「无链只读回读」，一条「链内 buffer」。
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
            const uint32_t expected = (base & ~PLANE_CW_MASK) | ((uint32_t)plane << PLANE_CW_SHIFT) | (uint32_t)cw;
            uint32_t       got_meta;

            test_read_meta(block, plane, page, cw, got);

            got_meta = (uint32_t)got[0] | ((uint32_t)got[1] << 8U) | ((uint32_t)got[2] << 16U) | ((uint32_t)got[3] << 24U);

            test_trace_summary("read_meta b=%hu plane=%hhu page=%hu cw=%hhu: got=0x%08lX expect=0x%08lX", block, plane, page, cw, (unsigned long)got_meta, (unsigned long)expected);

            munit_assert_uint32(got_meta, ==, expected);
        }

        /* ── 链内 META 复核（v1.36）：普通读（有 desc 链）→ fc_buffer_get_meta ──
         * 走 gc.c 的真实消费路径：读页完成后从 CW 基址 offset 0 取 META。与上面 op 11
         * 的「无链只读」结果互为交叉检查；同一页、同一 expected 公式。 */
        {
            fc_command_t       *cmd        = test_cmd_build(&(nand_addr_t){.block = block, .plane = plane, .page = (uint8_t)page}, test_plane_cw_bitmap(TEST_PLANE_BM(plane)), base, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE));
            fc_command_status_t completion = test_submit_wait(cmd);

            munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

            for (uint8_t cw = 0U; cw < CW_NUM; cw += 2U) /* CW0 与 CW2 */
            {
                const uint32_t expected = (base & ~PLANE_CW_MASK) | ((uint32_t)plane << PLANE_CW_SHIFT) | (uint32_t)cw;
                uint8_t       *meta_ptr = fc_buffer_get_meta(cmd, cw);
                uint32_t       got_meta;

                munit_assert_not_null(meta_ptr);

                got_meta = (uint32_t)meta_ptr[0] | ((uint32_t)meta_ptr[1] << 8U) | ((uint32_t)meta_ptr[2] << 16U) | ((uint32_t)meta_ptr[3] << 24U);

                test_trace_summary("buffer_get_meta b=%hu plane=%hhu page=%hu cw=%hhu: got=0x%08lX expect=0x%08lX", block, plane, page, cw, (unsigned long)got_meta, (unsigned long)expected);

                munit_assert_uint32(got_meta, ==, expected);
            }

            /* 契约：cw_index 不在链上 → NULL（本命令只覆盖该 plane 的 CW 0..CW_NUM-1，
             * 传 CW_NUM 必然落空）。 */
            munit_assert_null(fc_buffer_get_meta(cmd, (uint8_t)CW_NUM));

            munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        }
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_lock_ops — 块锁族合并用例（op 21/22/23/25，**不含**永久锁 op 24）
 *
 *  覆盖：
 *    - OP_BLOCK_LOCK_STATUS（25，7Ah）—— 读锁定状态字节（Lock# / LT# / LT）
 *    - OP_BLOCK_LOCK（23，2Ah）—— 锁定（无地址周期 ⇒ 锁全片）
 *    - OP_BLOCK_UNLOCK_LOW（21，23h）/ OP_BLOCK_UNLOCK_HIGH（22，24h）—— 按块地址解锁
 *    - OP_BLOCK_LOCK_TIGHT（24，2Ch）**不做**：永久锁到断电、资源不可恢复（同 OTP 口径）
 *
 *  ⚠ 断言强度**刻意只到「可逆」**。手册（MT29F4G08）的 LOCK 引脚：上电为低则所有
 *    BLOCK LOCK 命令被禁用；为高则默认锁死全部 block。本板现有用例一直能擦除 ⇒ 块未被
 *    保护 ⇒ 引脚几乎肯定是**低** ⇒ 2Ah 大概率**空转**。故「LOCK 是否真的挡住擦除」
 *    「状态字节前后怎么变」只**打印证据、不断言**——硬断言「LOCK 挡住擦除」在本板会当场红，
 *    而红的原因不是代码错。**唯一硬判据 = 锁序列之后块仍然完全可用（擦/写/读全通过）**。
 *
 *  ⚠ 若本板 LOCK 引脚其实为高（块锁启用），2Ah 会锁死全器件：Phase 4 会看到擦除被拒
 *    （正是要观察的现象），Phase 5 的 23h@block（解锁 0..block）+ 24h@block（解锁
 *    block..末块）合起来把全器件恢复为未锁，Phase 7 应恢复通过。
 *    **Phase 7 若红 ⇒「锁不可逆」是真结论，不绕过。**
 *
 *  抽样顺序固定为 ① plane ② block —— 顺序即复现契约。
 **************************************************************************/

/* 「块可用」判据：erase → prog 单页 → 读回 → 逐 CW PAA 校验，三段都硬断言无错误位。
 * 它就是本用例「LOCK→UNLOCK 之后块仍然完全可用」这条唯一硬判据的载体。 */
static void lock_verify_block_usable(uint16_t block, uint8_t plane, uint16_t page)
{
    const uint32_t      meta = (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = (uint8_t)page});
    fc_command_status_t completion;

    completion = test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    completion = test_prog_page(block, TEST_PLANE_BM(plane), page, &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    completion = test_read_page(block, TEST_PLANE_BM(plane), page, meta, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    test_verify_page_paa(g_source_data, block, page, TEST_PLANE_BM(plane));
}

static MunitResult test_block_lock_ops(const MunitParameter params[], void *data)
{
    fc_command_status_t erase_st;
    uint8_t             raw_before;
    uint8_t             raw_locked;
    uint8_t             raw_after;

    (void)params;
    (void)data;

    const uint8_t  plane = l3_plane_draw("block_lock_ops");
    const uint16_t block = l3_block_draw("block_lock_ops");
    const uint16_t page  = (uint16_t)(PAGES_PER_BLOCK / 2U);

    test_trace_begin();

    /* Phase 0：基线——块可用、内容已知（Phase 7 与它对照） */
    lock_verify_block_usable(block, plane, page);

    /* Phase 1：锁定前状态（证据） */
    raw_before = test_block_lock_status(block, plane, &L3_CONF(OP_BLOCK_LOCK_STATUS, FC_CRC_ENABLE));
    test_trace_summary("lock_status b=%hu plane=%hhu BEFORE: raw=0x%02hhX lock#=%u lt#=%u lt=%u", block, plane, raw_before, ((raw_before >> 2) & 1U), ((raw_before >> 1) & 1U), (raw_before & 1U));

    /* Phase 2：BLOCK_LOCK（2Ah，无地址周期 ⇒ 锁全片） */
    test_block_lock_cmd(block, plane, &L3_CONF(OP_BLOCK_LOCK, FC_CRC_ENABLE));

    /* Phase 3：锁定后状态（证据） */
    raw_locked = test_block_lock_status(block, plane, &L3_CONF(OP_BLOCK_LOCK_STATUS, FC_CRC_ENABLE));
    test_trace_summary("lock_status b=%hu plane=%hhu LOCKED: raw=0x%02hhX lock#=%u lt#=%u lt=%u", block, plane, raw_locked, ((raw_locked >> 2) & 1U), ((raw_locked >> 1) & 1U), (raw_locked & 1U));

    /* Phase 4：LOCK 是否真的挡住擦除——**只观察、不断言**（见用例头注释） */
    erase_st = test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_trace_summary("lock_status b=%hu plane=%hhu LOCK 后擦除 status=0x%hX => %s", block, plane, erase_st.status, ((erase_st.status & FC_CMD_STATUS_ERROR_MASK) != 0U) ? "LOCK 生效（擦除被拒）" : "LOCK 空转（本板 LOCK 引脚为低）");

    /* Phase 5：UNLOCK_LOW（23h）+ UNLOCK_HIGH（24h）——硬断言无错误位。
     * 23h@block 解锁 0..block、24h@block 解锁 block..末块 ⇒ 合起来把全器件恢复为未锁。 */
    test_block_lock_cmd(block, plane, &L3_CONF(OP_BLOCK_UNLOCK_LOW, FC_CRC_ENABLE));
    test_block_lock_cmd(block, plane, &L3_CONF(OP_BLOCK_UNLOCK_HIGH, FC_CRC_ENABLE));

    /* Phase 6：解锁后状态（证据） */
    raw_after = test_block_lock_status(block, plane, &L3_CONF(OP_BLOCK_LOCK_STATUS, FC_CRC_ENABLE));
    test_trace_summary("lock_status b=%hu plane=%hhu AFTER UNLOCK: raw=0x%02hhX lock#=%u lt#=%u lt=%u", block, plane, raw_after, ((raw_after >> 2) & 1U), ((raw_after >> 1) & 1U), (raw_after & 1U));

    /* Phase 7：可逆——块仍然完全可用（本用例唯一的硬判据） */
    lock_verify_block_usable(block, plane, page);

    test_trace_summary("block_lock_ops b=%hu plane=%hhu: 21/22/23/25 走通，锁序列后块仍可用（可逆）", block, plane);

    return MUNIT_OK;
}

/**************************************************************************
 *  test_mixed_pipeline — 压力：流式提交 / 资源耗尽即抽干 / 整块先写后读（v1.33）
 *
 *  与其余用例「发一条 → poll 一条 → 释放」不同：本条**不等完成**——连续下发，
 *  **每 4 条（或下发失败时）才做一次 polling + 取 CQ**，在途命令数因此涨到池上限
 *  （命令 / desc / buffer 池任一耗尽 ⇒ 建链失败），完成项又不断回流腾资源
 *  —— 这是真实主控的背压节奏，也是本用例的核心判据。
 *
 *  形状：
 *    Phase 0  擦除目标块（multi-plane，一次覆盖两个 plane）
 *    Phase 1  按 ch→ce→lun→page 遍历整块，流式提交 multi-plane PROGRAM
 *             每下发 4 条、或本条下发失败（池满 / SQ 满）⇒ polling + 取 CQ 处理释放
 *             下发成功才进下一页；遍历完再把剩余 CQ 一起处理完
 *    Phase 2  同样遍历，流式提交 multi-plane READ，逐 CW 校验 PAA（硬判据）
 *
 *  ── 为什么用 multi-plane ──
 *    不遍历 plane：一条命令覆盖 PLANE_NUM 个 plane（每条占 PLANE_NUM*CW_NUM 份
 *    desc/buffer），遍历维度因此只剩 ch→ce→lun→page。这既压到 multi-plane 那条
 *    数据路径（全仓此前只有 block_multi_plane 覆盖它），又让每条命令吃更多资源。
 *
 *  ── 拓扑无关 ──
 *    遍历维度全部来自 CH_NUM/CE_NUM/LUN_NUM/PAGES_PER_BLOCK；plane 由 op 的多 plane
 *    变体一次覆盖。收 CQ 的批量上限 batch = CH*CE*LUN 也在测试里算，FC 侧不写死任何
 *    批量常量。本板 LUN_NUM=1 ⇒ batch=1、NAND 最多 1 条在途（串行）；多 die 器件同一份
 *    代码自动展开成真并行。
 *
 *  ── 无数据冒险 ──
 *    先写完整块、再读回同一块 ⇒ 读的每一页必然已写完（Phase 1 已抽干到空）。
 *
 *  抽样：base block 抽一次；ch/ce/lun/page 全遍历（顺序固定 ⇒ 即复现契约）。
 **************************************************************************/

#define L3_STRESS_BATCH      ((uint16_t)(CH_NUM * CE_NUM * LUN_NUM))
#define L3_STRESS_SLOTS      (FC_COMMAND_POOL_DEPTH) /* 在途上限不可能超过命令池 */
#define L3_STRESS_BUDGET     (TEST_SERVICE_LOOP_MAX)
#define L3_STRESS_POLL_EVERY (4U) /* 每成功下发几条命令做一次 polling + 取 CQ */
#define L3_STRESS_OP_PROG    (0U)
#define L3_STRESS_OP_READ    (1U)

typedef struct
{
    fc_command_t *cmd; /* NULL = 空槽 */
    uint8_t       operation;
    uint16_t      block;
    uint8_t       plane_bm; /* 该命令覆盖的 plane 位图（本用例恒为 ALL） */
    uint16_t      page;
} l3_stress_slot_t;

/* 在途命令的登记表：容量 = 命令池（在途数不可能超过它），未用槽 cmd == NULL。
 * 完成项只带 command_id，靠它在这张表里配回命令 + 地址（CID 配对的土办法）。 */
static l3_stress_slot_t g_l3_stress_slots[L3_STRESS_SLOTS];

static void l3_stress_slots_reset(void)
{
    for (uint16_t i = 0U; i < L3_STRESS_SLOTS; i++)
    {
        g_l3_stress_slots[i].cmd = NULL;
    }
}

static l3_stress_slot_t *l3_stress_slot_alloc(void)
{
    for (uint16_t i = 0U; i < L3_STRESS_SLOTS; i++)
    {
        if (g_l3_stress_slots[i].cmd == NULL)
        {
            return &g_l3_stress_slots[i];
        }
    }
    return NULL; /* 表满 —— 只可能是登记逻辑漏了 release，munit_assert 会当场抓住 */
}

/* 按 completion.command_id 找槽位（cmd 无缓存 id，v15 起由指针派生）。 */
static l3_stress_slot_t *l3_stress_slot_find(uint16_t command_id)
{
    for (uint16_t i = 0U; i < L3_STRESS_SLOTS; i++)
    {
        fc_command_id_t id = FC_COMMAND_ID_INVALID;

        if (g_l3_stress_slots[i].cmd == NULL)
        {
            continue;
        }
        if ((fc_command_to_id(g_l3_stress_slots[i].cmd, &id) != 0U) && ((uint16_t)id == command_id))
        {
            return &g_l3_stress_slots[i];
        }
    }
    return NULL;
}

/* 建一条压力命令（只建链、不提交）；资源不足返回 NULL（背压信号，命令已内部 release）。
 * **multi-plane 变体**：一条命令覆盖 plane_bm 里的全部 plane。PROGRAM 填源数据；
 * READ 预填 0xCC 残留哨兵（读没真发生时能一眼看出）。 */
static fc_command_t *l3_stress_try_build(uint8_t operation, const nand_addr_t *addr, uint8_t plane_bm)
{
    const uint32_t cw_bm = test_plane_cw_bitmap(plane_bm);
    const uint32_t meta  = (uint32_t)pba_from_addr(addr);
    fc_command_t  *cmd;

    if (operation == L3_STRESS_OP_PROG)
    {
        cmd = test_cmd_try_build(addr, cw_bm, meta, &L3_CONF(OP_PROGRAM_PAGE_MULTI_PLANE, FC_CRC_ENABLE));
        if (cmd != NULL)
        {
            test_fill_user_paa(cmd);
        }
        return cmd;
    }

    cmd = test_cmd_try_build(addr, cw_bm, meta, &L3_CONF(OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT, FC_CRC_ENABLE));
    if (cmd != NULL)
    {
        test_memset_read_buffer(cmd);
    }
    return cmd;
}

/* 从 CQ 取一批完成项并处理：配槽 → 断言无错误位 → READ 先捕获再校验 → 最后 release。
 * 返回本次处理条数。**必须在 release 之前**捕获读回数据（release 会还 buffer）。 */
static uint16_t l3_stress_reap(void)
{
    fc_command_status_t buf[L3_STRESS_BATCH];
    uint16_t            n = 0U;
    uint16_t            j;

    (void)fc_poll_completions(buf, L3_STRESS_BATCH, &n);
    for (j = 0U; j < n; j++)
    {
        l3_stress_slot_t *slot = l3_stress_slot_find((uint16_t)buf[j].command_id);

        munit_assert_not_null(slot);
        munit_assert_uint32(buf[j].status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        if (slot->operation == L3_STRESS_OP_READ)
        {
            test_capture_cmd(slot->cmd);
            test_verify_page_paa(g_source_data, slot->block, slot->page, slot->plane_bm);
        }
        munit_assert_int(fc_command_release(slot->cmd), ==, FC_STATUS_OK);
        slot->cmd = NULL;
    }
    return n;
}

/* 流式跑完一个 phase：遍历 ch→ce→lun→page，逐页下发 multi-plane 命令。
 * **每成功下发 L3_STRESS_POLL_EVERY 条、或本条下发失败时**才 polling + 取 CQ——
 * 批量下发才能把在途数顶到池上限；下发成功才进下一页。退出循环后再把剩余 CQ 处理完。 */
static void l3_stress_stream(uint8_t operation, uint16_t block)
{
    const uint8_t plane_bm   = TEST_PLANE_BM_ALL;
    uint32_t      submitted  = 0U;
    uint32_t      drained    = 0U;
    uint32_t      guard      = 0U;
    uint16_t      since_poll = 0U;

    l3_stress_slots_reset();

    for (uint16_t ch = 0U; ch < CH_NUM; ch++)
    {
        for (uint16_t ce = 0U; ce < CE_NUM; ce++)
        {
            for (uint16_t lun = 0U; lun < LUN_NUM; lun++)
            {
                for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
                {
                    const nand_addr_t addr = (nand_addr_t){.ch = (uint8_t)ch, .ce = (uint8_t)ce, .lun = (uint8_t)lun, .block = block, .plane = test_first_plane(plane_bm), .page = (uint8_t)page};
                    uint8_t           sent = 0U;

                    while (sent == 0U)
                    {
                        fc_command_t *cmd    = l3_stress_try_build(operation, &addr, plane_bm);
                        uint8_t       polled = 0U;

                        if (cmd == NULL)
                        {
                            /* 池满：本页还没发出去，本轮只 polling */
                        }
                        else if (fc_submit(cmd) != FC_STATUS_OK)
                        {
                            /* SQ 满：还命令，本页还没发出去 */
                            (void)fc_command_release(cmd);
                        }
                        else
                        {
                            l3_stress_slot_t *slot = l3_stress_slot_alloc();

                            munit_assert_not_null(slot);
                            slot->cmd       = cmd;
                            slot->operation = operation;
                            slot->block     = block;
                            slot->plane_bm  = plane_bm;
                            slot->page      = page;
                            sent            = 1U;
                            since_poll++;
                        }

                        /* 每下发 L3_STRESS_POLL_EVERY 条、或本轮没发出去 ⇒ polling + 取 CQ：
                         * 完成项不断被处理、资源不断回流 */
                        if ((sent == 0U) || (since_poll >= L3_STRESS_POLL_EVERY))
                        {
                            (void)fc_service();
                            drained += l3_stress_reap();
                            since_poll = 0U;
                            polled     = 1U;
                        }

                        /* 证据档（--log-visible debug）逐条打印各资源池用量：
                         * sub/retry = 本条下发成功 / 没发出去（池满·SQ 满）；+poll = 本轮做了 polling。
                         * 能直接看出「发到多少条时开始 retry、retry 后完成项回流把哪个池腾空」。 */
                        {
                            fc_resource_stats_t rs;

                            (void)fc_get_resource_stats(&rs);
                            test_trace_detail("p%02hu %s%s submitted=%lu drained=%lu | cmd=%hu/%u desc=%hu/%u buf=%hu/%u sq=%hu cq=%hu", page, (sent != 0U) ? "sub  " : "retry", (polled != 0U) ? " +poll" : "      ", submitted, drained, rs.command_used, FC_COMMAND_POOL_DEPTH, rs.descriptor_used, FC_DESCRIPTOR_POOL_DEPTH, rs.buffer_used, FC_BUFFER_POOL_DEPTH, rs.sq_used, rs.cq_used);
                        }

                        munit_assert_uint32(++guard, <, L3_STRESS_BUDGET);
                    }

                    submitted++;
                }
            }
        }
    }

    /* 退出循环后：剩余 CQ 一起处理完（本 phase 提交的必须全部完成） */
    while (drained < submitted)
    {
        (void)fc_service();
        drained += l3_stress_reap();
        munit_assert_uint32(++guard, <, (L3_STRESS_BUDGET * 4U));
    }

    /* 收尾证据：本 phase 全部完成并 release 后，命令 / desc / buffer 三池应回到 0
     * （SQ 也应为 0）。打印而非断言——池没归零是要人看的信号，不是本用例的判据。 */
    {
        fc_resource_stats_t rs;

        (void)fc_get_resource_stats(&rs);
        test_trace_summary("phase done: submitted=%lu drained=%lu | cmd=%hu desc=%hu buf=%hu sq=%hu cq=%hu (应为全 0)", submitted, drained, rs.command_used, rs.descriptor_used, rs.buffer_used, rs.sq_used, rs.cq_used);
    }
}

/* Phase 0 前置命令的硬判据：无错误位。 */
static void l3_stress_expect_ok(fc_command_status_t completion)
{
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
}

static MunitResult test_mixed_pipeline(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    const uint16_t block = l3_block_draw("mixed_pipeline");

    test_trace_begin();
    test_trace_summary("mixed_pipeline block=%hu ch=%d ce=%d lun=%d plane=%d page=%d batch=%hu poll_every=%u", block, CH_NUM, CE_NUM, LUN_NUM, PLANE_NUM, PAGES_PER_BLOCK, L3_STRESS_BATCH, L3_STRESS_POLL_EVERY);

    /* ── Phase 0：整块擦净（multi-plane 一次覆盖两个 plane）——给 Phase 1 一个干净起点 ── */
    l3_stress_expect_ok(test_erase_block(block, TEST_PLANE_BM_ALL, &L3_CONF(OP_ERASE_BLOCK_MULTI_PLANE, FC_CRC_ENABLE)));

    /* ── Phase 1：流式写完整块（遍历 ch→ce→lun→page，multi-plane 命令），写完全部抽空 ── */
    l3_stress_stream(L3_STRESS_OP_PROG, block);
    test_trace_summary("phase 1 done: PROGRAM 全块 %d 页 × %d plane", PAGES_PER_BLOCK, PLANE_NUM);

    /* ── Phase 2：流式读回整块并逐 CW 校验（读在写全部落盘之后，无数据冒险） ── */
    l3_stress_stream(L3_STRESS_OP_READ, block);
    test_trace_summary("phase 2 done: READ 全块 %d 页 × %d plane 逐 CW 校验通过", PAGES_PER_BLOCK, PLANE_NUM);

    test_trace_summary("mixed_pipeline block=%hu: 整块先写后读（流式提交 / 池满即抽干）通过", block);

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
 *  buffer 归属位域（v53）：谁申请 / 谁释放
 *
 *  判据 = fc_get_resource_stats().buffer_used 的**增量**：建链 +1、归还回基线。
 *  载体 = OP_READ_ID 单 CW 链（单 plane、has_data=1、无需建块，同 init_sequence Step 2）。
 *  四档全覆盖：
 *    00 FC 申请 / 上层释放（默认，既有生产行为，必须逐字不变）
 *    01 FC 申请 / FC 释放
 *    10 上层申请 / 上层释放
 *    11 上层申请 / FC 释放
 *  ⚠ 本用例只验**资源账**，不比对介质内容（数据正确性由 init_sequence 覆盖）。
 **************************************************************************/

/* 当前 buffer 池占用（满 = FC_BUFFER_POOL_DEPTH）。测试不 include fc_internal.h，
 * 只能经公开统计接口观测。 */
static uint16_t l3_buf_used(void)
{
    fc_resource_stats_t stats;

    (void)fc_get_resource_stats(&stats);
    return stats.buffer_used;
}

/* 模式 10/11 的建链：上层先 fc_buffer_alloc()，再把 id 传进 5 参入口。
 * ⚠ 不能用 test_cmd_build —— 它走 ftl_cmd_configure_descriptors（4 参入口，即
 *   buf_own = FC 申请那条路）。此处照 test_cmd_try_build 的样子手工建。 */
static fc_command_t *l3_buf_own_build_upper(fc_buffer_id_t buffer_id, uint8_t buf_own)
{
    fc_command_t *cmd  = fc_command_alloc();
    fc_cmd_conf_t conf = L3_CONF(OP_READ_ID, FC_CRC_ENABLE);

    munit_assert_not_null(cmd);

    conf.buf_own = (DWORD)buf_own;
    cmd->config  = conf;

    cmd->config.ch          = 0U;
    cmd->config.ce          = 0U;
    cmd->config.lun         = 0U;
    cmd->config.multi_plane = 0U;
    cmd->op_tag             = (DWORD)FTL_OP_HOST_IO;
    cmd->paa                = paa_from_addr(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U});
    cmd->meta               = 0U;

    munit_assert_int(fc_command_configure_descriptor_with_buffer(cmd, 0U, 0U, cmd->paa, buffer_id), ==, FC_STATUS_OK);

    return cmd;
}

static MunitResult test_buf_own_release(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    const uint16_t base = l3_buf_used();

    /* ── Step 1：00（默认）——FC 申请，上层 fc_command_release 归还 ── */
    {
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &L3_CONF(OP_READ_ID, FC_CRC_ENABLE));

        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U)); /* 建链即申请 */
        (void)test_submit_wait(cmd);
        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U)); /* 00 完成不自动归还 */
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        munit_assert_uint32(l3_buf_used(), ==, base); /* release 归还 */
    }

    /* ── Step 2：01 —— FC 申请，完成时 FC 当场归还（上层只要命令状态） ── */
    {
        fc_cmd_conf_t conf = L3_CONF(OP_READ_ID, FC_CRC_ENABLE);
        fc_command_t *cmd;

        conf.buf_own = (DWORD)FC_BUF_OWN_FC_ALLOC_FC_FREE;
        cmd          = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &conf);

        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U));
        (void)test_submit_wait(cmd);
        munit_assert_uint32(l3_buf_used(), ==, base); /* 01 完成即归还 */
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        munit_assert_uint32(l3_buf_used(), ==, base); /* 幂等：不双重归还 */
    }

    /* ── Step 3：11 —— 上层申请（先 fc_buffer_alloc），完成时 FC 归还 ── */
    {
        fc_buffer_id_t ubuf = fc_buffer_alloc();
        fc_command_t  *cmd;

        munit_assert_uint32(ubuf, !=, (uint32_t)FC_BUFFER_ID_INVALID);
        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U));

        cmd = l3_buf_own_build_upper(ubuf, (uint8_t)FC_BUF_OWN_UPPER_ALLOC_FC_FREE);
        (void)test_submit_wait(cmd);
        munit_assert_uint32(l3_buf_used(), ==, base); /* 11 完成即由 FC 归还上层的 buffer */
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        munit_assert_uint32(l3_buf_used(), ==, base);
    }

    /* ── Step 4：10 —— 上层申请，上层释放（完成不归还，读法 A 的回归守卫） ── */
    {
        fc_buffer_id_t ubuf = fc_buffer_alloc();
        fc_command_t  *cmd;

        munit_assert_uint32(ubuf, !=, (uint32_t)FC_BUFFER_ID_INVALID);
        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U));

        cmd = l3_buf_own_build_upper(ubuf, (uint8_t)FC_BUF_OWN_UPPER_ALLOC_UPPER_FREE);
        (void)test_submit_wait(cmd);
        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U)); /* 10 完成不归还 */
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        munit_assert_uint32(l3_buf_used(), ==, base); /* release 恒归还链上未归还的 buffer */
    }

    /* ── Step 5：建了链但**没提交** —— 模式 01 走 fc_command_release 也不泄漏 ── */
    {
        fc_cmd_conf_t conf = L3_CONF(OP_READ_ID, FC_CRC_ENABLE);
        fc_command_t *cmd;

        conf.buf_own = (DWORD)FC_BUF_OWN_FC_ALLOC_FC_FREE;
        cmd          = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &conf);

        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)(base + 1U));
        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        munit_assert_uint32(l3_buf_used(), ==, base);
    }

    /* ── Step 6：空闲链完整性守卫 —— 双重释放会让同一 id 被链入 free list 两次，
     *            这里连续申请满池，断言 id 两两不同 ──
     * 申请次数 = 池深 − 基线（本用例正常时基线为 0；用增量写是为了不受前面用例的
     * 残留影响，让本用例独立成立）。 */
    {
        const uint32_t avail = (uint32_t)FC_BUFFER_POOL_DEPTH - (uint32_t)base;
        fc_buffer_id_t ids[FC_BUFFER_POOL_DEPTH];

        for (uint32_t i = 0U; i < avail; i++)
        {
            ids[i] = fc_buffer_alloc();
            munit_assert_uint32(ids[i], !=, (uint32_t)FC_BUFFER_ID_INVALID);

            for (uint32_t j = 0U; j < i; j++)
            {
                munit_assert_uint32(ids[i], !=, (uint32_t)ids[j]);
            }
        }

        munit_assert_uint32(l3_buf_used(), ==, (uint32_t)FC_BUFFER_POOL_DEPTH);

        for (uint32_t i = 0U; i < avail; i++)
        {
            munit_assert_int(fc_buffer_release(ids[i]), ==, FC_STATUS_OK);
        }

        munit_assert_uint32(l3_buf_used(), ==, base);
    }

    test_trace_summary("buf_own_release base=%hu: 00 上层释放 / 01 FC释放 / 10 上层申请+释放 / 11 上层申请+FC释放 —— 四档账目全部闭合", base);

    return MUNIT_OK;
}

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
 *    - block_lock_ops（块锁族 21/22/23/25 合并用例，抽 plane / block）
 *    - buf_own_release（buffer 归属位域 00/01/10/11，只验资源账，地址不参与，不随机化）
 *    - block_lock_tight（永久锁芯片验证，**被 #if 0 隔开**，不可逆故不启用）
 *
 *  上面这些行（含 buf_own_release）用 MUNIT_TEST_OPTION_NONE（**保留**，不要换成 SINGLE_ITERATION）：
 *  --iterations N 才是覆盖旋钮（每条抽 N 组）；换成 SINGLE_ITERATION 会把旋钮锁死。
 *  runner.iterations 默认 0 ⇒ 迭代次数回落到套件级 iterations（ut_main.c 里 = 1），
 *  与是否带 SINGLE_ITERATION 无关（munit.c:1375 / :706-709）。
 *  ⚠ --iterations 是 **runner 级全局量**，且 runner->suite **恒指根套件**（munit.c:1359）
 *    ⇒ 子套件自己的 iterations 字段无效，**别去改 ut_main.c**（改了会把 Level 1/2 也乘 N）。
 *    它按调用范围生效：只跑 /fc-nand/lifecycle/ 就只乘 Level 3。
 **************************************************************************/

MunitTest g_fc_lifecycle_tests[] = {
    {                      "init_sequence",                       test_init_sequence, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                 "feature_write_read",                  test_feature_write_read, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                "chain_priority_lock",                 test_chain_priority_lock, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                       "status_query",                        test_status_query, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                 "internal_data_move",                  test_internal_data_move, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                 "block_erase_verify",                  test_block_erase_verify, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                   "block_write_read",                    test_block_write_read, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                    "block_cache_ops",                     test_block_cache_ops, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {            "block_cache_multi_plane",             test_block_cache_multi_plane, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {"block_cache_multi_plane_cross_block", test_block_cache_multi_plane_cross_block, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                  "block_multi_plane",                   test_block_multi_plane, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                  "block_random_data",                   test_block_random_data, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                   "block_load_fetch",                    test_block_load_fetch, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                    "block_read_meta",                     test_block_read_meta, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                     "block_lock_ops",                      test_block_lock_ops, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                     "mixed_pipeline",                      test_mixed_pipeline, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
    {                    "buf_own_release",                     test_buf_own_release, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, NULL},
#if 0  /* block_lock_tight 被 #if 0 隔开，此处不注册 */
    { "block_lock_tight",    test_block_lock_tight,    test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
#endif
    {                                 NULL,                                     NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
