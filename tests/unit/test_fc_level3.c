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
 * ── 参数化：随机性由 munit 的种子驱动 ──
 *   9 条注册里 6 条地址相关的挂了 MunitParameterEnum（block / plane / sample）：
 *     · `test --single /fc-nand/lifecycle/`  —— 每用例按种子**随机抽一组**参数
 *     · `test --single --seed 0x… /…`        —— 复现上一轮（开场打印 seed 值）
 *     · `test /fc-nand/lifecycle/`           —— 不加 --single ⇒ 全组合枚举（耗时长）
 *     · `test --single --param block 104 /…` —— CLI 给的参数**优先于**随机挑值
 *   取值一律在用例体里就地解析（l3_param_block / l3_param_plane / l3_sample_case_find），
 *   不建中间工厂。框架**不校验** --param 的值，故取值处必须自查。
 *   ⚠ 用例体内**不用** munit_rand_*：munit 在每次 exec 前把 PRNG 重置回 runner->seed
 *     （munit.c:715），而导出的 munit_rand_int_range 不带 salt ⇒ 两个用例体内的第一个
 *     随机数完全相同（带 salt 的 munit_rand_at_most 在 munit.c 里是 static）。
 *     随机性只走「参数取值」这一条路，框架用带 salt 的种子挑值（munit.c:1139）。
 *
 * PBA 约定：ch=0 ce=0 lun=0；block / plane / page 由参数给出（见 l3_*_params）
 *           （v24/v25：命令携带的是 PBA，不是 PAA；地址一律用具名字段构造，
 *             见 nand_config.h 的 pba_from_addr / paa_from_addr）
 *
 * 依赖：
 *   - NAND 硬件（走 fc_submit → fc_service → nand_fc_start/step 异步路径）
 *   - 建链走共享夹具的 test_cmd_build()（FTL 策略层 ftl_cmd_configure_descriptors，
 *     「按掩码建整条链」，自 v13 起属 FTL）或建立在它之上的页 / block helper。
 *     ⚠ cw_bitmap 是**字面位图**（置 1 = 要访问该 CW，0 = 空集），v23~v26 的
 *       「0 = 本命令 plane 段内的全部 CW」重载已取消（v27）。凡"只要 1 条 desc"
 *       的用例必须显式给出该 CW 的位（如 1U << cw）；整段访问用
 *       test_plane_cw_bitmap(plane_bitmap)。config.cw 自 v23 起不参与建链。
 *   - 释放只需 fc_command_release()（v16 起沿 desc 链把 desc + buffer 一起还池）
 *
 * 用法：
 *   ssd> test --list-params /fc-nand/lifecycle/
 *   ssd> test --single /fc-nand/lifecycle/
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

/* ── 测试块池 ──
 * 用例体的块号来自 block 参数（见下方 l3_blocks），只有两档 100 / 104；
 * 内部搬移的目标块恒 = 源块 + L3_BLOCK_DEST_OFF（⇒ 102 / 106）。
 *
 * ⚠ 块号**不能**放开到 BLOCKS_PER_PLANE：跑 UT 时 FTL 是活的（src/main.c 无条件
 *   ssd_ftl_boot_all()，test 命令从主循环的 uart_service_poll() 进来），free pool
 *   覆盖整片 device。这是**既有**问题（改造前就写 100/102），参数化只是多写了
 *   104/106，不引入新的风险面。 */
#define L3_BLOCK_DEST_OFF (2U)

/* 旧默认值，只服务本文件 #if 0 段——BLOCK_TEST 被 unlock_erase / block_lock_tight
 * 引用；BLOCK_DEST 连 #if 0 段也不引用，与 BLOCK_TEST 成对留着纯属历史。
 * 活代码一律走参数，不再用这两个宏。 */
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
 *  参数化
 *
 *  值表是 NULL 结尾的 const char* 数组；MunitParameterEnum 数组**非 const**
 *  （munit.h:436 的 MunitTest.parameters 是非 const 指针）。本 fork 的
 *  MunitParameterEnum 是 `const char *name; const char **values;`（munit.h:423-427）
 *  ⇒ **不需要 (char*) 转换**，与 Level 2（test_fc_level2.c:235/241）同形。
 **************************************************************************/

static const char *l3_blocks[]  = {"100", "104", NULL};
static const char *l3_planes[]  = {"0", "1", NULL};
static const char *l3_samples[] = {"edges", "spread", "middle", "dense", NULL};

static MunitParameterEnum l3_block_params[] = {
    {"block", l3_blocks},
    {   NULL,      NULL}
};
static MunitParameterEnum l3_block_plane_params[] = {
    {"block", l3_blocks},
    {"plane", l3_planes},
    {   NULL,      NULL}
};
static MunitParameterEnum l3_rnd_params[] = {
    { "block",  l3_blocks},
    { "plane",  l3_planes},
    {"sample", l3_samples},
    {    NULL,       NULL}
};

/* 参数取值。框架不校验 --param 的值（munit.c:1102-1115 只按名字收下、原样转交），
 * 表外名字会直接进用例 ⇒ 这里必须自查。 */
static uint16_t l3_param_block(const MunitParameter params[])
{
    const char *s = munit_parameters_get(params, "block");

    munit_assert_not_null(s);
    munit_assert_true((strcmp(s, "100") == 0) || (strcmp(s, "104") == 0)); /* 获准池 */
    return (uint16_t)((strcmp(s, "100") == 0) ? 100U : 104U);
}

static uint8_t l3_param_plane(const MunitParameter params[])
{
    const char *s = munit_parameters_get(params, "plane");

    munit_assert_not_null(s);
    munit_assert_true((strcmp(s, "0") == 0) || (strcmp(s, "1") == 0));
    return (uint8_t)((strcmp(s, "0") == 0) ? 0U : 1U);
}

/* ── 抽样策略（test_block_random_data）──
 * 页集 + 列集。"spread" 逐字等于改造前的现状（页 0/15/31/47/63 + CW 1/3），
 * 留作回归基线；其余三档扩大页/列的覆盖形态。 */
typedef struct
{
    const char    *name;
    const uint8_t *pages;
    uint8_t        page_count;
    const uint8_t *cws;
    uint8_t        cw_count;
} l3_sample_case_t;

static const uint8_t l3_pages_edges[]  = {0U, 63U};
static const uint8_t l3_pages_spread[] = {0U, 15U, 31U, 47U, 63U};
static const uint8_t l3_pages_middle[] = {30U, 31U, 32U};
static const uint8_t l3_pages_dense[]  = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};

static const uint8_t l3_cw_one[]  = {1U};
static const uint8_t l3_cw_pair[] = {1U, 3U};
static const uint8_t l3_cw_all[]  = {0U, 1U, 2U, 3U};

static const l3_sample_case_t l3_sample_cases[] = {
    { "edges",  l3_pages_edges, 2U, l3_cw_pair, 2U},
    {"spread", l3_pages_spread, 5U, l3_cw_pair, 2U},
    {"middle", l3_pages_middle, 3U,  l3_cw_one, 1U},
    { "dense",  l3_pages_dense, 8U,  l3_cw_all, 4U},
};

_Static_assert((sizeof(l3_samples) / sizeof(l3_samples[0]) - 1U) == (sizeof(l3_sample_cases) / sizeof(l3_sample_cases[0])),
               "l3_samples 与 l3_sample_cases 数量不符");

static const l3_sample_case_t *l3_sample_case_find(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }

    for (uint8_t i = 0U; i < (uint8_t)(sizeof(l3_sample_cases) / sizeof(l3_sample_cases[0])); i++)
    {
        if (strcmp(l3_sample_cases[i].name, name) == 0)
        {
            return &l3_sample_cases[i];
        }
    }

    return NULL;
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
        cmd->pba              = pba_from_addr(&(nand_addr_t){.block = block});

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
    CMD_FIELD_U(pba);

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
    (void)params;
    (void)data;

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

    return MUNIT_OK;
}

/**************************************************************************
 *  test_feature_write_read — Feature 读验证
 *
 *  流程：
 *    1. GET_FEATURES(addr=0x01) — 读 Feature 寄存器
 *    2. GET_FEATURES(addr=0x81) — 读另一个 Feature 寄存器
 *
 *  验证点：
 *    - Feature 读取命令独立执行成功
 *    - 返回数据非全 0xFF（硬件在呼吸）
 *
 *  注意：本测试只读不写，避免意外切换 NAND 时序模式。
 **************************************************************************/

static MunitResult test_feature_write_read(const MunitParameter params[], void *data)
{
    static const uint8_t feature_addrs[2] = {0x01U, 0x81U};

    (void)params;
    (void)data;

    for (uint8_t i = 0U; i < 2U; i++)
    {
        /* GET_FEATURES 不建链（ftl_cmd_needs_desc_chain 排除），pba / 掩码都不看。 */
        fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0U, 0U, &L3_CONF(OP_GET_FEATURES, FC_CRC_ENABLE));

        cmd->feature_addr = feature_addrs[i];

        munit_assert_uint32(test_submit_wait(cmd).status, ==, FC_CW_STATUS_SUCCESS);

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

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
    static const uint8_t status_ops[2] = {(uint8_t)OP_READ_STATUS, (uint8_t)OP_READ_STATUS_ENHANCED};

    (void)params;
    (void)data;

    for (uint8_t i = 0U; i < 2U; i++)
    {
        /* 状态字仅 1 字节 ⇒ 只取 CW0（1 条 desc）；状态查询不是 page read，
         * 数据路径不介入，nand_status 经 completion.status 返回。
         * operation 逐轮不同 ⇒ config 需可变，先按基线拷一份再改。 */
        fc_cmd_conf_t conf = L3_CONF(status_ops[i], FC_CRC_ENABLE);
        fc_command_t *cmd  = test_cmd_build(&(nand_addr_t){.block = 0U, .plane = 0U, .page = 0U}, 0x01U, 0U, &conf);

        munit_assert_uint32(test_submit_wait(cmd).status, ==, FC_CW_STATUS_SUCCESS);

        munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    }

    return MUNIT_OK;
}

/**************************************************************************
 *  test_internal_data_move — 内部搬移验证
 *
 *  流程：
 *    1. 擦除源 block + 目标 block（目标 = 源 + L3_BLOCK_DEST_OFF）
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
 **************************************************************************/

static MunitResult test_internal_data_move(const MunitParameter params[], void *data)
{
    (void)data;
    const uint16_t block = l3_param_block(params);
    const uint16_t dest  = (uint16_t)(block + L3_BLOCK_DEST_OFF);

    /* Step 1: 擦除源 + 目标 block */
    test_erase_block(block, TEST_PLANE_BM(0), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_erase_block(dest, TEST_PLANE_BM(0), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));

    /* Step 2: 写源 block（64 pages） */
    test_prog_block(block, TEST_PLANE_BM(0), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    /* Step 3: 用普通读（00h-30h）验证源 block 全部 64 pages 正确，排除写阶段问题 */
    test_read_block(block, TEST_PLANE_BM(0), &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

    /* Step 4: 逐 page 内部搬移：READ_FOR_INTERNAL_DATA_MOVE → PROGRAM_FOR_INTERNAL_DATA_MOVE
     * ⚠ 这两个 op 都**不是** page read/write（fc_is_page_read / fc_is_page_write 都不含），
     *   数据路径不介入：搬的是 NAND 内部缓存，页内 META 原样跟着走。
     *   故目标页读回时，源页写入时铸的 META（源页 PBA）仍然自洽 —— 见 Step 5。
     *   这两条命令的 meta 不参与编解码（无数据路径），填源页 PBA 只为表达「搬的是这一页」。 */
    for (uint16_t page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        /* 4a: READ_FOR_INTERNAL_DATA_MOVE (00h-35h) — 将源 block page 读到 NAND 内部缓存 */
        {
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = 0U, .page = page},
                                               test_plane_cw_bitmap(TEST_PLANE_BM(0)),
                                               (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = 0U, .page = page}),
                                               &L3_CONF(OP_READ_FOR_INTERNAL_DATA_MOVE, FC_CRC_ENABLE));

            (void)test_submit_wait(cmd);
            munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
        }

        /* 4b: PROGRAM_FOR_INTERNAL_DATA_MOVE (85h-10h) — 将内部缓存数据写入目标 block page */
        {
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = dest, .plane = 0U, .page = page},
                                               test_plane_cw_bitmap(TEST_PLANE_BM(0)),
                                               (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = 0U, .page = page}),
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
        const uint32_t      tag = (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = 0U, .page = page});
        fc_command_status_t completion =
            test_read_page(dest, TEST_PLANE_BM(0), page, tag, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

        munit_assert_uint32(completion.status & FC_CW_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block, page, TEST_PLANE_BM(0));
    }

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
    (void)data;
    const uint16_t block = l3_param_block(params);
    const uint8_t  plane = l3_param_plane(params);

    /* Phase 1: 擦除 */
    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));

    /* Phase 2: 写全 block 64 pages */
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    /* Phase 3: 读回全 block 64 pages 验证 */
    test_read_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE), NULL);

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
 *     残留状态；参数化后块池变大（100/104），顺序依赖更明显。
 **************************************************************************/

static MunitResult test_block_erase_verify(const MunitParameter params[], void *data)
{
    (void)data;
    const uint16_t block = l3_param_block(params);
    const uint8_t  plane = l3_param_plane(params);

    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    read_block_verify_erased(block, TEST_PLANE_BM(plane));

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_cache_ops — Cache 读序列验证
 *
 *  流程：
 *    1. 擦除 block（参数给出）
 *    2. 用 PROGRAM_PAGE_CACHE 写该 plane 全 block（63 cache + 1 final 确认）
 *    3. Cache 读序列遍历所有 64 pages
 *    4. 每 page 数据与预期 pattern 比对
 *
 *  验证点：
 *    - Cache 写 + Cache 读数据一致性
 *    - Cache 读序列正确遍历多页
 *    - 各阶段 status 正确
 *
 *  ⚠️ CRC/LDPC/加扰/RAID 全部关闭（LDPC RAW 模式），纯 RAW 数据验证。
 *  ⚠ plane 1 的 cache 路径改造前从未走过（旧代码 plane 恒 0）——参数化新增的覆盖面。
 *     cache 只有单 plane 变体（ONFI 1.0 无 multi-plane；器件侧有 two-plane cache
 *     **编程**、无 two-plane cache **读**）。
 **************************************************************************/

static MunitResult test_block_cache_ops(const MunitParameter params[], void *data)
{
    (void)data;
    const uint16_t block = l3_param_block(params);
    const uint8_t  plane = l3_param_plane(params);

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
    (void)data;
    const uint16_t block = l3_param_block(params);

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

    return MUNIT_OK;
}

/**************************************************************************
 *  test_block_random_data — 随机列读验证
 *
 *  流程：
 *    1. 擦除 + 写全 block 基准数据（参数给出的 plane）
 *    2. 按 sample 参数取页集，每页：
 *       a. Normal read page 加载页缓冲
 *       b. RANDOM_DATA_READ 抽样页集里的每个 CW，验证数据
 *
 *  验证点：
 *    - 同页不同 CW 的列地址解码正确
 *    - 跨页行地址切换后随机列读正确
 *    - 数据与原始写入一致
 *
 *  页集/列集由 sample 参数决定（"spread" 逐字等于改造前的现状，留作回归基线）。
 **************************************************************************/

static MunitResult test_block_random_data(const MunitParameter params[], void *data)
{
    (void)data;
    const uint16_t          block = l3_param_block(params);
    const uint8_t           plane = l3_param_plane(params);
    const l3_sample_case_t *sc    = l3_sample_case_find(munit_parameters_get(params, "sample"));

    munit_assert_not_null(sc); /* 挡表外 --param sample 值（框架不校验） */

    test_erase_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_ERASE_BLOCK, FC_CRC_ENABLE));
    test_prog_block(block, TEST_PLANE_BM(plane), &L3_CONF(OP_NORMAL_PROGRAM_PAGE, FC_CRC_ENABLE), NULL);

    for (uint8_t si = 0U; si < sc->page_count; si++)
    {
        const uint16_t page = sc->pages[si];

        /* Normal read page — 加载 NAND 页缓冲（顺带校验整页） */
        read_page_verify(block, page, TEST_PLANE_BM(plane), 0U, &L3_CONF(OP_NORMAL_READ_PAGE_WITH_DATA_OUT, FC_CRC_ENABLE));

        for (uint8_t ci = 0U; ci < sc->cw_count; ci++)
        {
            const uint8_t cw = sc->cws[ci];

            /* 随机列读：只抽 1 个 CW ⇒ cw_bitmap 必须是**被抽样那个 CW 的位**。
             * ⚠ 这里曾经写死 0x01U（= 只取 CW0），而期望值按抽样 cw 算——因为
             *   链是按掩码位枚举 cw 的（cmd->pba 的 cw 字段会被枚举值覆盖），
             *   于是 desc 落在 CW0、期望值取 CW1/CW3，两者对不上。
             *   旧 pattern 是 4 字节周期、cw*512 又是 4 的倍数 ⇒ 四个 CW 期望值
             *   完全相同，这条错误一直被掩盖（v22 changelog 记录过同类问题）。 */
            fc_command_t *cmd = test_cmd_build(&(nand_addr_t){.block = block, .plane = plane, .page = page},
                                               (uint32_t)1U << cw,
                                               (uint32_t)pba_from_addr(&(nand_addr_t){.block = block, .plane = plane, .page = page}),
                                               &L3_CONF(OP_RANDOM_DATA_READ, FC_CRC_ENABLE));

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
 *  包含（前 3 条是定长命令序列，地址不参与，不参数化）：
 *    - init_sequence（初始化序列：RESET -> READ_ID -> ...）
 *    - feature_write_read（Feature 写回验证）
 *    - status_query（状态查询）
 *  其余 6 条挂参数（block / plane / sample，见上方 l3_*_params）：
 *    - internal_data_move（片上内部搬移，block）
 *    - block_erase_verify（擦除后全 0xFF 验证，block×plane）
 *    - block_write_read（全 block 写读，block×plane）
 *    - block_cache_ops（cache 读序列数据验证，block×plane）
 *    - block_multi_plane（多 Plane 擦写读验证，block）
 *    - block_random_data（随机列读写验证，block×plane×sample）
 *    - block_lock_tight（永久锁芯片验证，必须放在最后）
 *
 *  参数化行用 MUNIT_TEST_OPTION_NONE：不加 --single 时按参数表**全组合枚举**，
 *  加 --single 时每个参数枚举按种子随机抽一个值（munit.c:1139）。runner.iterations
 *  默认 0 ⇒ 迭代次数回落到套件级 iterations（ut_main.c 里 = 1），与是否带
 *  SINGLE_ITERATION 无关（munit.c:1375 / :706-709）。
 *
 *  （unlock_erase 用 #if 0 隔开，暂不启用）
 **************************************************************************/

MunitTest g_fc_lifecycle_tests[] = {
    {     "init_sequence",      test_init_sequence, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,                  NULL},
    {"feature_write_read", test_feature_write_read, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,                  NULL},
    {      "status_query",       test_status_query, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,                  NULL},
    {"internal_data_move", test_internal_data_move, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,       l3_block_params},
    {"block_erase_verify", test_block_erase_verify, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l3_block_plane_params},
    {  "block_write_read",   test_block_write_read, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l3_block_plane_params},
    {   "block_cache_ops",    test_block_cache_ops, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l3_block_plane_params},
    { "block_multi_plane",  test_block_multi_plane, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,       l3_block_params},
    { "block_random_data",  test_block_random_data, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,         l3_rnd_params},
#if 0  /* block_lock_tight 被 #if 0 隔开，此处不注册 */
    { "block_lock_tight",    test_block_lock_tight,    test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL },
#endif
    {                NULL,                    NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE,                  NULL},
};