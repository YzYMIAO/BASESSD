/*
 * test_fc_data_path.c — 数据路径测试（buffer 级，不碰 NAND）：fc_data_path 模块
 *
 * 测试三个数据路径管线函数：
 *   fc_data_path_encode       写路径：META → CRC? → Random 加扰? → ECC 编码
 *   fc_data_path_process_read 读路径 Stage 1：ECC 解码 → 条件解扰 → CRC 校验 → META 校验
 *   fc_data_path_raid_recover 读路径 Stage 2：RAID XOR 重建 → ECC 重试 → 解扰
 *
 * ── 全部用例都是 buffer 级：建链后**不提交** ──
 *   本层只验数据路径本身（编码产物、status 位、链几何），**不碰 NAND**。
 *   fc_poll_dispatch_single() 对 page write 调 fc_data_path_encode，
 *   fc_nand_completion_finish()   对 page read 调 fc_data_path_process_read——
 *   所以**提交出去的命令绝不能再手工调这两个函数**（会重复编码 / 重复解扰）。
 *   本层一律走 test_cmd_build() 拿一条未提交的命令，自己驱动管线。
 *   NAND 上的端到端往返由 lifecycle 的页 helper 覆盖（test_prog_page / test_read_page）。
 *
 * ── 产物自证 ──
 *   「encode 后 process_read 回到明文」这条恒等式在两边都是空转时**照样成立**。
 *   故每条 encode 用例除了往返，还要查**编码产物本身**：
 *     l2_verify_meta_written  —— [0,4) == 本 desc 的 LBA（Stage 0 真写了）
 *     l2_verify_crc_written   —— [4,8) == 明文 USER 的 CRC（Stage 1 真写了）
 *     l2_verify_parity_nonzero—— [520,528) 非全 0（Stage 2 真写了）
 *     l2_user_matches_source  —— random=ON 时 encode 后 USER **必须已不是明文**
 *                                （fixture 一旦退化使 keystream 整片为 0，往返恒等式
 *                                 会把问题原样盖过去）
 *
 * ── 夹具 ──
 *   setup / teardown、建链原语 test_cmd_build、源数据表、desc 链小工具全部来自共享模块
 *   fc_test_common（data_path / lifecycle 共用一份，见 tests/unit/fc_test_common.h）。
 *   源数据约定：每个 CW 的 USER 区 = **该 CW 自己的 PAA 重复 128 次**
 *   （test_fill_user_paa 填槽 + 拷进 desc）。故「读回来逐 dword 与期望 PAA 比」
 *   同时验了 block / plane / page / cw 四维。
 *
 * ── 配置形态 ──
 *   random     : FC_RANDOM_ENABLE(=0) / FC_RANDOM_DISABLE
 *   crc        : FC_CRC_ENABLE(=0) / FC_CRC_DISABLE
 *   ldpc       : FC_ECC_MODE_NORMAL（本层默认，走真实编解码）/ FC_ECC_MODE_LDPC_RAW
 *   cw_bitmap  : flat 绝对编号位图（bit n = plane*CW_NUM + cw）。
 *                **字面位图**：置 1 = 要访问该 CW，**0 = 空集**，没有「0 = 段内全部 CW」
 *                的重载 ⇒ 整段访问必须显式写出来。
 *   raid       : 形参保留但**数据路径不读**——config.raid 全仓唯一读者是 fc.c 的
 *                分派钩子（决定走不走 fc_stripe_submit）
 *
 * ⚠ 加扰种子 = 本 CW 的 **META（页 PBA）**，由 command->meta 的高位 + 本 desc
 *   的 plane/cw 造出。若 meta == 0，4 条 CW 的种子只由 cw(0..3) 派生，仍互异但都来自
 *   小地址——故 fixture 显式给一个非零 PBA，让「种子随页地址变」这条性质真的被测到。
 *
 * 释放：一律 fc_command_release() 收尾——它沿 desc 链把 desc + buffer 一起还池；
 *       不要再手动 fc_buffer_release（会二次释放、空闲链成环）。
 */

#include "crc.h"
#include "fc_bch_engine.h"
#include "fc_crc_engine.h"
#include "fc_data_path.h"
#include "fc_ecc_engine.h"
#include "fc_interface.h"
#include "fc_random_engine.h"
#include "fc_test_common.h"
#include "munit.h"
#include "nand_config.h"
#include "onfi_1_0.h"

#if CONFIG_FC_RAID
#    include "fc_raid_engine.h"
#endif

#include <string.h>

/* ── 本层地址抽样 ──
 * 命令地址**按 munit 种子抽样**（block / page；plane 由用例钉死），与 lifecycle 同路：
 * 每轮拿到不同的加扰种子与 USER 图案，而不是所有用例挤在同一个固定页上。
 *
 * ⚠ 为什么必须加 salt：munit 每次 exec 前把 PRNG 重置回 runner->seed（munit.c），
 *   而导出的 munit_rand_* **不带盐** ⇒ 用例体内的**第一次**抽样在所有用例之间取值完全
 *   相同。故每次抽样异或进 `hash(用例名) ^ hash(用途标签)`：前者让**用例之间**互异，
 *   后者让**同一用例内各用途**互异。复现契约 = (seed, 用例名, 第几次抽样)。
 * ⚠ 本层**不碰介质**（见文件头）——地址只喂两样：**加扰种子**（fc_random_seed_from_meta）
 *   与 **USER 区的 PAA 图案**（test_fill_user_paa）。所以收益是「14 条用例拿到 14 组不同的
 *   种子 / 图案」，**不是** lifecycle 那种「地址错值编译期无声」的风险。之所以仍值得做：本层
 *   多数用例是 MUNIT_TEST_OPTION_SINGLE_ITERATION（munit.c 把迭代数硬压成 1）
 *   ⇒ `--iterations N` 对它们无效，用例体内抽样是唯一的加量杠杆。
 * ⚠ 不设窗口：lifecycle 的 30%~70% 是为了避开固定元数据区，而本层不写介质、那理由不成立，
 *   故用整段 [0, BLOCKS_PER_PLANE) / [0, PAGES_PER_BLOCK)，与 test_scramble_whiten 同口径。
 *
 * 抽样结果必须打出来（见 l2_addr_draw）——它是 (seed, 用例名) 的纯函数，不打就无从复现。 */

/** DJB2，与 munit.c 的 munit_str_hash **逐字同实现**（那份是 static，只能照抄）。 */
static DWORD l2_hash(const char *s)
{
    DWORD h = 5381U;

    for (; *s != '\0'; s++)
    {
        h = (h << 5U) + h + (BYTE)*s;
    }

    return h;
}

/** 每次抽样 = munit 随机流 ^ 用例名哈希 ^ 用途标签哈希，再过一道终混。
 *
 * ⚠ 终混（murmur3 fmix32）**不是**装饰：DJB2 的**低位很弱**——h = h*33 + c 且 33 ≡ 1
 *   (mod 32) ⇒ h 的低 5 位只等于各字符之和 mod 32。取 `% BLOCKS_PER_PLANE` / `% PAGES_PER_BLOCK`
 *   时被截到低位，两个哈希只要**低位相同**就会撞。实测（宿主机复刻）：
 *   `process_read_meta_error` 与 `process_read_ldpc_raw` 的哈希只在高位不同
 *   （0xE9E586CA vs 0x0A6286CA，XOR = 0xE3870000）⇒ 四条种子下两条用例**逐位抽到同一个地址**。
 *   终混让结果的每一位都受全 32 位影响，撞的概率回到 1/2^11 量级。 */
static DWORD l2_rand(const char *test, const char *tag)
{
    DWORD h = munit_rand_uint32() ^ l2_hash(test) ^ l2_hash(tag);

    h ^= h >> 16;
    h *= 0x85EBCA6BU;
    h ^= h >> 13;
    h *= 0xC2B2AE35U;
    h ^= h >> 16;

    return h;
}

/** 抽一个完整合法地址：block / page 随机，plane 由调用者钉死，cw 恒 0
 * （test_cmd_build 断言 cw == 0）。
 *
 * 就地 emit 一条 test_trace_detail（debug 档）——一处覆盖全部用例，不必每个用例体各写一行。 */
static nand_addr_s l2_addr_draw(const char *test, BYTE plane)
{
    nand_addr_s addr;

    memset(&addr, 0, sizeof(addr));
    /* `% N` 的取值已在 [0, N) 内，赋值转换即那次显式窄化（H48 ②）。 */
    addr.block = l2_rand(test, "block") % BLOCKS_PER_PLANE;
    addr.plane = plane;
    addr.page  = l2_rand(test, "page") % PAGES_PER_BLOCK;
    addr.cw    = 0U;

    test_trace_detail("addr block=%hu page=%hhu plane=%hhu", addr.block, addr.page, addr.plane);
    return addr;
}

/* 本层多数用例只要 plane 0 的整段（4 个 CW）。位图是**字面**的，0 是空集
 * （ftl_cmd_resolve_cw_bitmap 的断言会当场炸），故必须显式写出来。 */
#define L2_BM_PLANE0_ALL (0x0FU)

/*************************************************************************
 *  本层专用的小封装
 **************************************************************************/

/** 建一条 **单 plane、BCH 编解码** 的命令（未提交）。operation 固定 OP_NORMAL_PROGRAM_PAGE：
 * 数据路径（encode / process_read）**不读** config.operation（fc_data_path.c 全文无该字段的
 * 读者），本层驱动的就是"一个已编好码的页"这个状态。需要 multi-plane 段的几何用例直接调
 * test_cmd_build（见文末）。掩码由调用者给。
 *
 * 地址由调用者传（见「本层地址抽样」段）：命令的 **pba 与 meta 同源**——两者都从 addr 派生，
 * 写读同页自然一致，且页地址整体进了 META ⇒「读错页」会被 Pass 4 ② 抓到。 */
static fc_cmd_s *l2_cmd(const nand_addr_s *addr, DWORD cw_bitmap, BYTE random_mode, BYTE crc_mode)
{
    fc_cmd_cfg_s config;

    ASSERT(addr != NULL);

    memset(&config, 0, sizeof(config));
    config.operation = OP_NORMAL_PROGRAM_PAGE;
    config.random    = random_mode;
    config.crc       = crc_mode;
    config.ldpc      = FC_ECC_MODE_NORMAL;

    return test_cmd_build(addr, cw_bitmap, pba_from_addr(addr), &config);
}

/** 要求每个 CW 的 status **恰好**是 READY（一个错误位都没有）。
 * 共享夹具的 test_verify_cw_status_all_success 只看 SUCCESS 位，而
 * descriptor 成功 = FC_CW_STATUS_UNUSED(0)；CRC_ERROR / META_ERROR 单独置位
 * 而不被发现；本层的正向用例要的是"干净读回"，故单独加这条更严的判据。 */
static void l2_verify_all_ready(const fc_cmd_s *cmd)
{
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status, ==, FC_CW_STATUS_UNUSED);
    }
}

/** 逐 desc 与源表比对，返回是否**完全一致**（不 assert——供「应当不同」的用例取反）。 */
static BYTE l2_user_matches_source(const fc_cmd_s *cmd)
{
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        const fc_desc_s *desc   = test_cw_desc(cmd, i);
        const BYTE      *buf    = test_cw_user(cmd, i);
        const BYTE      *expect = test_source_slot(paa_to_plane(desc->paa), paa_to_cw(desc->paa));

        for (WORD j = 0U; j < FC_BUFFER_USER_SIZE; j++)
        {
            if (buf[j] != expect[j])
            {
                return 0U;
            }
        }
    }
    return 1U;
}

/** ── 编码产物自证（见文件头「产物自证」） ── */

static void l2_verify_meta_written(const fc_cmd_s *cmd)
{
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        DWORD stored = 0U;

        memcpy(&stored, test_cw_base(cmd, i), FC_CRC_META_SIZE);
        munit_assert_uint32(stored, ==, test_cw_desc(cmd, i)->meta);

        /* 证据（--log-visible info）：把**介质上真实写下的值**打出来。
         * 只报 [ OK ] 时看不出 Stage 0 到底写没写——空转的 encode 一样会返回 OK。 */
        if (i == 0U)
        {
            test_trace_summary("meta written: %hhu CW, cw0=0x%08lX == desc->meta (Stage 0 真写了)", test_cw_count(cmd), stored);
        }
    }
}

/** ⚠ 必须拿**源槽**（明文）算 CRC，不能拿 desc 的 USER 区——random=ON 时那里已被加扰，
 *   而 CRC 是对加扰**之前**的明文算的（fc_crc_engine.h 头注）。 */
static void l2_verify_crc_written(const fc_cmd_s *cmd)
{
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        const fc_desc_s *desc   = test_cw_desc(cmd, i);
        const BYTE      *plain  = test_source_slot(paa_to_plane(desc->paa), paa_to_cw(desc->paa));
        DWORD            stored = 0U;

        memcpy(&stored, test_cw_base(cmd, i) + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE);
        munit_assert_uint32(stored, ==, fc_crc_compute(plain));

        /* 证据（--log-visible info）：介质上的 CRC 字段 vs 明文重算值。
         * ⚠ 打的是**明文槽**的重算值——random=ON 时 desc 的 USER 区已加扰，拿它算必错。 */
        if (i == 0U)
        {
            test_trace_summary("crc written: %hhu CW, cw0=0x%08lX == crc32(明文槽) (Stage 1 真写了)", test_cw_count(cmd), stored);
        }
    }
}

/** ECC parity 区 [520,528) 必须**非全 0**——证明 Stage 2（ecc_encode）真写了它。
 * 判据取"非全 0"而不是"等于某个期望值"：parity 是 BCH 生成多项式的余式，期望值得
 * 另写一份编码器才算得出；而"整段全 0"正是"根本没编码"的指纹（未初始化 / 擦除态）。
 * 哨兵思想与 meta / crc 两条一致，只是判据更弱——够用即可。 */
static void l2_verify_parity_nonzero(const fc_cmd_s *cmd)
{
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        const BYTE *base = test_cw_base(cmd, i);

        /* test_check_pattern 返回 1 表示"整段都是该 pattern"⇒ 断言 == 0 即"非全 0" */
        munit_assert_uint8(test_check_pattern(base + FC_BUFFER_DATA_SIZE, (FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE), 0x00U), ==, 0U);

        /* 证据（--log-visible info）：parity 区首 4 字节的实际值（Stage 2 真写了）。 */
        if (i == 0U)
        {
            DWORD head = 0U;

            memcpy(&head, base + FC_BUFFER_DATA_SIZE, sizeof(head));
            test_trace_summary("parity written: %hhu CW, cw0[520..524)=0x%08lX != 0 (Stage 2 真写了)", test_cw_count(cmd), head);
        }
    }
}

/** buffer 级往返：建链 → 填明文 → encode → process_read → 校验。
 * **不提交**——FC 提交时会自己再跑一遍 encode / process_read，重复编码会让用例空转。
 * 返回链上的 CW 数，供调用者打证据摘要（避免调用者按位图 popcount 反推）。 */
static BYTE l2_buffer_round_trip(const nand_addr_s *addr, DWORD cw_bitmap, BYTE random_mode, BYTE crc_mode)
{
    fc_cmd_s *cmd      = l2_cmd(addr, cw_bitmap, random_mode, crc_mode);
    BYTE      cw_count = test_cw_count(cmd);

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);
    munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

    l2_verify_all_ready(cmd);
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    fc_command_release(cmd->cmd_id);
    return cw_count;
}

/*************************************************************************
 *  步骤级用例（单 CW 原语）——不建链、不需要 config 位
 *
 *  这些直接调 fc_data_path_* 的单 CW 原语（见 include/fc/fc_data_path.h），
 *  只碰 test_cw_scratch() 给的一块 528B buffer：
 *  **调了哪些原语 = 开了哪些功能**，不必用 config 位去拼出"只开 CRC"这类状态。
 **************************************************************************/

/* ── CRC 步骤级：钉**覆盖边界** ──
 *
 * "只开 CRC" 就是只调 meta_write + crc_write——不调 scramble、不调 ecc_encode，
 * 于是不需要任何 config 位，也不需要 LDPC_RAW 去绕开 ECC 把注入的 bit 纠回去。
 *
 * fc_crc_compute(cw + 8) 只吃 **FC_CRC_USER_SIZE = 512** 字节（fc_crc_engine.c），
 * 存的值在 cw[4..8)，两者都不碰 META[0,4) 与 parity[520,528) ⇒ 这四处**期望不同**，
 * 是四个分支，不是一个断言的四种输入 —— 所以走参数化，不用随机采样：
 *
 *   user-1 / crc-field-1 → **必检出**（check == 0）
 *   meta-1 / parity-1    → **必不检出**（check == 1）
 *
 * 后两条是负例：没有它们，一个"把整块 528B 都拿去算 CRC"的错误实现照样能过。
 * crc-field-1 的机制也不同——坏的是**存的值**，不是被校验的数据。
 *
 * 区域内的偏移仍由框架 PRNG 取（运行器打印 seed，失败可 --seed 原样重放）。 */
typedef struct L2_CRC_CASE_S
{
    const char *name;
    DWORD       lo; /* 注入字节区间 [lo, hi)（CW 内偏移） */
    DWORD       hi;
    BYTE        expect_ok; /* 1 = 注入后 CRC 仍通过（该区域不在 CRC 覆盖内） */
} l2_crc_case_s;

static const l2_crc_case_s l2_crc_cases[] = {
    {     "user-1",  FC_CRC_USER_OFFSET, FC_CRC_USER_OFFSET + FC_BUFFER_USER_SIZE, 0U},
    {"crc-field-1",    FC_CRC_META_SIZE,     FC_CRC_META_SIZE + FC_CRC_FIELD_SIZE, 0U},
    {     "meta-1",                  0U,                         FC_CRC_META_SIZE, 1U},
    {   "parity-1", FC_BUFFER_DATA_SIZE,                   FC_BUFFER_POOL_CW_SIZE, 1U},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_crc_cases 一一对应。 */
static const char *l2_crc_case_names[] = {"user-1", "crc-field-1", "meta-1", "parity-1", NULL};

_Static_assert((sizeof(l2_crc_case_names) / sizeof(l2_crc_case_names[0]) - 1U) == (sizeof(l2_crc_cases) / sizeof(l2_crc_cases[0])), "l2_crc_case_names 与 l2_crc_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_crc_params[] = {
    {"case", l2_crc_case_names},
    {  NULL,              NULL},
};

/** 五张场景表（crc / ecc / enc / rt / bm）共用的按名查行：表的 `name` 是首字段、
 * names[] 是与表同形的 NULL 结尾名表（_Static_assert 已钉住数量）⇒ 查名得下标、
 * 按下标取行，避免"名字数组"与"场景表"两处错位。表外名字返回 NULL，调用者断言非 NULL。
 * 各表行类型不同 ⇒ 行地址按 `row_size` 步进，由调用方传 sizeof(行)。 */
static const void *l2_case_find(const char *name, const char *const names[], const void *rows, size_t row_size)
{
    size_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0U; names[i] != NULL; i++)
    {
        if (strcmp(names[i], name) == 0)
        {
            return (const BYTE *)rows + i * row_size;
        }
    }

    return NULL;
}

/** 各表的类型化入口：把 void* 在查行结果上收窄回具体行类型（H43 指针 reinterpret）。 */
static const l2_crc_case_s *l2_crc_case_find(const char *name)
{
    return (const l2_crc_case_s *)l2_case_find(name, l2_crc_case_names, l2_crc_cases, sizeof(l2_crc_cases[0]));
}

/* ── 用例体：写 CRC → 自证干净 → 在场景区间注入 1 bit → 查判定 ──
 *
 * 四段：
 *   ① 0xCC 铺满 528B 当残留哨兵——"没写到"与"写成了 0"才分得开
 *   ② meta_write + 明文 USER + crc_write —— 只开 CRC，不调 scramble / ecc_encode
 *   ③ **先自证**：未注入时 check 必须 == 1。否则下面那句"检出了"毫无意义
 *   ④ 注入 1 bit，断言 check == c->expect_ok（user / crc-field 必检出，
 *      meta / parity 必不检出，见场景表）；再翻回来断言必通过
 *      —— 钉住"判定只看数据，不粘状态"
 *
 * 注入位置由框架 PRNG 在 c->lo..c->hi-1 内取（运行器打印 seed，失败可 --seed 原样重放）。
 * 步骤级**不原地解码**，故直接翻回来即可，不依赖"解码不改 buffer"这类假设。 */
static MunitResult test_crc_step_detect(const MunitParameter params[], void *data)
{
    const l2_crc_case_s *c  = l2_crc_case_find(munit_parameters_get(params, "case"));
    BYTE                *cw = test_cw_scratch(0U);
    DWORD                off;
    DWORD                bit;

    (void)data;

    test_trace_begin();

    /* 抽地址（在 test_trace_begin 之后——它的 detail 行要落在前缀行收尾之后）。 */
    const nand_addr_s addr = l2_addr_draw("crc_step_detect", 0U);

    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */

    /* 残留哨兵：未写到的字节应能看见 0xCC，避免"其实没写"被当成通过 */
    memset(cw, 0xCC, FC_BUFFER_POOL_CW_SIZE);

    /* META 与 PAA 同源（都是抽到的那一页）；PAA 非零 ⇒ USER 区填出来不是一片 0，
     * "根本没写"与"写了 0"分得开。低 3 位 = plane/cw = 0，与 meta_check 的自洽校验一致。 */
    fc_data_path_meta_write(cw, pba_from_addr(&addr));
    test_fill_paa(cw + FC_CRC_USER_OFFSET, paa_from_addr(&addr));
    fc_data_path_crc_write(cw);

    /* 先自证：未注入时应通过（否则下面的"检出"没有意义） */
    munit_assert_uint8(fc_data_path_crc_check(cw), ==, 1U);

    off = (DWORD)munit_rand_int_range((int)c->lo, (int)c->hi - 1);
    bit = (DWORD)munit_rand_int_range(0, 7);
    test_trace_summary("case=%s inject cw[%lu] bit %lu", c->name, off, bit);

    /* 步骤级没有原地解码，直接翻回来即可——不依赖任何"解码不改 buffer"假设 */
    cw[off] ^= (1U << bit);
    munit_assert_uint8(fc_data_path_crc_check(cw), ==, c->expect_ok);

    cw[off] ^= (1U << bit);
    munit_assert_uint8(fc_data_path_crc_check(cw), ==, 1U); /* 还原 ⇒ 必通过 */

    return MUNIT_OK;
}

/* ── 0/1 游程统计（加扰用例用） ──
 *
 * 位序：每字节 **MSB 先**。加扰引擎的 fc_lfsr_byte 就是把先出的位放在 MSB，
 * 按这个顺序读出来的 bit 串就是 LFSR 的输出序列本身（见 fc_random_engine.c）。
 * 字节内反序会得到另一套游程分布，两处必须用同一约定。 */

typedef struct L2_BIT01_S
{
    DWORD hist[2][16]; /* hist[b][k] = 长度 k+1 的游程个数；k=15 表示 >=16（钳位） */
    DWORD ones;        /* 1 的总个数 */
    DWORD runs;        /* 游程总数（= 两个 hist 之和） */
} l2_bit01_s;

/** 游程长度 → 桶号。>=16 全部钳进第 15 桶——**不能丢弃**：全 0xFF 的明文就一个
 * 长度 4096 的游程、PAA 重复的明文有 128 个 >=16 的 0 游程，正是加扰要治的病。 */
static BYTE l2_run_bucket(DWORD run)
{
    return (run >= 16U) ? 15U : (BYTE)(run - 1U);
}

/** 一趟流式扫描数游程（0/1 分开入桶）。
 *
 * 只维护两个游标：cur（当前段的位值）、run（当前段已累积长度）。每读一个 bit：
 *   - 与 cur 相同 → run++（段加长）
 *   - 与 cur 不同 → 结算旧段（hist[cur][桶]++、runs++），再 cur = b、run = 1（开新段）
 *
 * ⚠ cur 从**首位**初始化，不是从 0——否则首段是 1 时会漏计一次。
 * ⚠ 循环**后**必须再补一次结算——最后一段没有后继，不补则段数恒少 1（游程编码的经典 off-by-one）。
 *
 * 随机数据每步 1/2 概率翻转 ⇒ 段数 ≈ 位数的一半（2048）、平均段长 2（几何分布 P(len=k)=2^-k）；
 * 推导与判据盲区见 fc_nand_ut.md §2.4.7，「目标 = 独立」见 fc_nand_design.md §2.6。 */
static void l2_bit01_analyze(const BYTE *data, WORD len, l2_bit01_s *st)
{
    WORD  i;
    BYTE  k;
    BYTE  cur;
    DWORD run;

    memset(st, 0, sizeof(*st));
    if (len == 0U)
    {
        return;
    }

    cur = ((data[0] >> 7U) & 1U);
    run = 0U;

    for (i = 0U; i < len; i++)
    {
        for (k = 0U; k < 8U; k++)
        {
            const BYTE b = ((data[i] >> (7U - k)) & 1U);

            if (b != 0U)
            {
                st->ones++;
            }

            if (b == cur)
            {
                run++;
            }
            else
            {
                st->hist[cur][l2_run_bucket(run)]++;
                st->runs++;
                cur = b;
                run = 1U;
            }
        }
    }

    st->hist[cur][l2_run_bucket(run)]++;
    st->runs++;
}

/** 平均游程 ×100 = 总位数 × 100 ÷ 段数（整数比较，避开浮点与 %f）。
 * 随机数据每步 1/2 概率翻转 ⇒ 平均段长 = 2.00（推导见 fc_nand_ut.md §2.4.7）。 */
static DWORD l2_bit01_mean_x100(const l2_bit01_s *st, WORD len)
{
    return (st->runs == 0U) ? 0U : (((DWORD)len * 8U * 100U) / st->runs);
}

/** 与"恰好一半"的偏离量（bit 数）。 */
static DWORD l2_bit01_bias(const l2_bit01_s *st, WORD len)
{
    const DWORD half = (DWORD)len * 4U;

    return (st->ones > half) ? (st->ones - half) : (half - st->ones);
}

/** 把统计结果打成日志（上板后照这个分布收紧阈值）。只打非空的桶。 */
static void l2_bit01_log(const char *tag, const l2_bit01_s *st, WORD len)
{
    BYTE k;

    test_trace_summary("%s: ones=%lu/%u runs=%lu mean_x100=%lu", tag, st->ones, (len * 8U), st->runs, l2_bit01_mean_x100(st, len));

    for (k = 0U; k < 16U; k++)
    {
        if ((st->hist[0][k] + st->hist[1][k]) != 0U)
        {
            test_trace_detail("  run len %2u%s : zero x%lu, one x%lu", (k + 1U), (k == 15U) ? "+" : " ", st->hist[0][k], st->hist[1][k]);
        }
    }
}

/** 加扰白化：三种偏置输入 → 加扰 → 统计游程 → 解扰 → 自反。
 *
 * 理论依据（详见 fc_nand_design.md §2.6 / fc_nand_ut.md §2.4.7）：
 *   加扰的目标是让输出**统计独立**——P(bit=1 | 全部历史) = 1/2，**不是**「0/1 各占一半」这种
 *   无条件均衡，更**不是**「下一位尽量和当前相反」。`0101…` 满足均衡却最不随机（翻转率 100%、
 *   段数 = 4096 = 完美交替）。随机数据每步 1/2 概率翻转 ⇒ 段数 ≈ 位数的一半（≈2048）、
 *   平均段长 2（几何分布 P(len=k)=2^-k）。故下面判据 ①②③ 检的是随机性的**症状**。
 *   ⚠ 它们测不出**周期性**——`00110011…` 均衡、平均段长正好 2.0、无长游程，三条全过却仍是固定图案；
 *   真正的病根（独立）无法被有限统计量完全覆盖。
 *
 * 判据（阈值来自离线实测 2048 个种子：页 0..255 × plane_cw 0..7）：
 *   ① 密文 1 的个数    实测 1908~2158（最坏偏离 140）⇒ 这里放到 2048±192
 *   ② 平均游程 ×100    实测 190~215（随机数据理论值 200）⇒ 放到 [170, 240]
 *   ③ >=16 的长游程数  实测 0~1 ⇒ 放到 <=3
 *   ④ 明文明显偏置时，密文必须比明文更靠近 50%——"加扰有效"本身
 *   ⑤ 解扰后逐字节回到明文（自反）
 * ①~③ 都离"加扰器死掉"（占比 0%/100%、平均游程 4096、长游程 4096）差得极远。
 *
 * 游程**众数**只打印不断言：PAA 重复的明文众数本来就是 1（每个 32-bit 字里有孤立的 1），
 * 加扰前后都是 1，没有鉴别力。
 *
 * 地址由框架 PRNG 取（运行器打印 seed，失败可 --seed 原样重放）。 */
static MunitResult test_scramble_whiten(const MunitParameter params[], void *data)
{
    static const char *const tag[3] = {"all-0x00", "all-0xFF", "PAA-repeat"};
    BYTE                     kind;

    (void)params;
    (void)data;

    test_trace_begin();

    for (kind = 0U; kind < 3U; kind++)
    {
        BYTE *cw               = test_cw_scratch(0U);
        BYTE *plain            = test_cw_scratch(1U);
        /* 取一个完整合法地址：meta 直接取它的 PBA——低 3 位天然是 plane/cw，
         * 与建链时 desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw 同构。 */
        const nand_addr_s addr = {
            .lun   = 0U,
            .block = (WORD)munit_rand_int_range(0, BLOCKS_PER_PLANE - 1),
            .plane = (BYTE)munit_rand_int_range(0, PLANE_NUM - 1),
            .page  = (BYTE)munit_rand_int_range(0, PAGES_PER_BLOCK - 1),
            .cw    = (BYTE)munit_rand_int_range(0, CW_NUM - 1),
        };
        const DWORD paa  = paa_from_addr(&addr);
        const DWORD meta = pba_from_addr(&addr);
        const DWORD seed = fc_random_seed_from_meta(meta);
        l2_bit01_s  before;
        l2_bit01_s  after;

        /* 明文：常量图案，或本 CW 的 PAA 重复（后者是真实写路径的图案） */
        if (kind == 2U)
        {
            test_fill_paa(cw + FC_CRC_USER_OFFSET, paa);
        }
        else
        {
            memset(cw + FC_CRC_USER_OFFSET, (kind == 0U) ? 0x00 : 0xFF, FC_BUFFER_USER_SIZE);
        }
        memcpy(plain, cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE);

        l2_bit01_analyze(plain, FC_BUFFER_USER_SIZE, &before);

        /* 自证：常量图案的明文必须**恰好 1 个游程**——否则是分析函数自己错了 */
        if (kind < 2U)
        {
            munit_assert_uint32(before.runs, ==, 1U);
            munit_assert_uint32(before.ones, ==, (kind == 0U) ? 0U : (FC_BUFFER_USER_SIZE * 8U));
        }

        fc_data_path_scramble(cw, seed);
        l2_bit01_analyze(cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, &after);

        test_trace_detail("--- %s: block=%hu page=%hhu plane=%hhu cw=%hhu meta=0x%08lX seed=0x%08lX", tag[kind], addr.block, addr.page, addr.plane, addr.cw, meta, seed);
        l2_bit01_log("  plain    ", &before, FC_BUFFER_USER_SIZE);
        l2_bit01_log("  scrambled", &after, FC_BUFFER_USER_SIZE);

        /* ① 1 的个数落到 50% 附近 */
        munit_assert_uint32(l2_bit01_bias(&after, FC_BUFFER_USER_SIZE), <=, 192U);

        /* ② 平均游程落到随机数据的理论值 2.0 附近 */
        munit_assert_uint32(l2_bit01_mean_x100(&after, FC_BUFFER_USER_SIZE), >=, 170U);
        munit_assert_uint32(l2_bit01_mean_x100(&after, FC_BUFFER_USER_SIZE), <=, 240U);

        /* ③ >=16 的长游程基本消失 */
        munit_assert_uint32((after.hist[0][15] + after.hist[1][15]), <=, 3U);

        /* ④ 明文明显偏置时，密文必须比它更靠近 50% */
        if (l2_bit01_bias(&before, FC_BUFFER_USER_SIZE) > 512U)
        {
            munit_assert_uint32(l2_bit01_bias(&after, FC_BUFFER_USER_SIZE), <, l2_bit01_bias(&before, FC_BUFFER_USER_SIZE));
        }

        /* ⑤ 自反：解扰后逐字节回到明文 */
        fc_data_path_scramble(cw, seed);
        munit_assert_memory_equal(FC_BUFFER_USER_SIZE, cw + FC_CRC_USER_OFFSET, plain);
    }

    return MUNIT_OK;
}

/* ── ECC 步骤级（数据路径实际跑的是 BCH(m=13, t=4)） ──
 *
 * fc_data_path_ecc_encode / _decode 就是 fc_ldpc_* 的统一入口，默认编译宏走
 * **BCH 分支**（FC_ECC_ALGO_BCH；全仓无 -DFC_ECC_ALGO 覆盖），fc_ldpc_config_t
 * 的三个字段在两边都被 (void)config 忽略。
 * 覆盖区间是 data[0..520) = **META(4) + CRC(4) + USER(512)**，parity 8B 在 [520,528)。
 *
 * 场景走 **munit 参数化**（一个用例 × N 组参数）：每组一次独立的 setup/tear_down
 * ⇒ 某个场景红了不会遮住后面的，且运行器在测试名下打印 `case=…`；板子上可用
 * `test --param case meta-1` 钉单个场景复现。⚠ 只有一个维度——`case` 是**手挑的
 * 场景名**，不是 k×region 的笛卡尔积（那会造出一堆重复/无意义的组合）。
 *
 * 判据钉在 t=4 上：1..4 bit 错必须**全部精确纠正**。超能力（≥5 bit）单列一个
 * BOUND 场景：BCH 允许误纠（实测约 0.28% 返回 OK 但数据是错的），所以**不钉结果**，
 * 只钉两条恒成立的不变式（失败必须原子 / 纠正数不得超过 t）。
 *
 * 注入位置走框架 PRNG 并逐个打印：失败时按 seed 重放或直接按位置复现。
 * ⚠ 框架默认种子是 time() 派生的（munit.c），每次跑都不同 ⇒ 任何"超能力错误
 *   必然不可纠"的断言都会偶发红灯。
 *
 * 本用例**不设** SINGLE_ITERATION ⇒ 可用 `--iterations N` 给每个场景多抽几组注入
 * 位置（默认仍是 1 次：根套件 iterations=1）。 */

#define L2_ECC_DATA_BITS ((DWORD)FC_BUFFER_DATA_SIZE * 8U) /* 4160：ECC 保护的数据区 */
#define L2_ECC_META_BITS ((DWORD)FC_CRC_META_SIZE * 8U)    /* 32：META cw[0..4) */
#define L2_ECC_CRC_LO    (L2_ECC_META_BITS)                /* 32：CRC 字段 cw[4..8) */
#define L2_ECC_CRC_HI    (2U * L2_ECC_META_BITS)           /* 64 */
#define L2_ECC_USER_LO   (L2_ECC_CRC_HI)                   /* 64：USER cw[8..520) */
/* parity 区里**确定被校验**的字节数：8B 槽只写了 7B，且被校验的 52 bit 并不连续
 * （parity 首 6 字节 + 第 7 字节高 4 位；第 7 字节低 4 位与第 8 字节编码器从不写、
 * 解码器也不校验）。这里只取前 6 字节，避开这段布局细节。 */
#define L2_ECC_PARITY_BYTES (6U)
#define L2_ECC_PARITY_LO    (L2_ECC_DATA_BITS)                            /* 4160 */
#define L2_ECC_PARITY_HI    (L2_ECC_PARITY_LO + L2_ECC_PARITY_BYTES * 8U) /* 4208 */

/** 造一块"写路径形态"的 CW：META + USER(PAA 明文) + CRC + ECC。
 * 不含加扰——加扰是正交的一步（见 test_scramble_whiten），ECC 覆盖整个 data[0..520)，
 * 与 USER 区是否已加扰无关。ref 非 NULL 时留一份基准副本（比对用）。 */
static void l2_ecc_cw_build(const nand_addr_s *addr, BYTE *cw, BYTE *ref)
{
    ASSERT(addr != NULL);

    memset(cw, 0xCC, FC_BUFFER_POOL_CW_SIZE); /* 残留哨兵：没被写到的字节可见 */
    fc_data_path_meta_write(cw, pba_from_addr(addr));
    test_fill_paa(cw + FC_CRC_USER_OFFSET, paa_from_addr(addr));
    fc_data_path_crc_write(cw);
    munit_assert_int(fc_data_path_ecc_encode(cw), ==, FC_STATUS_OK);

    if (ref != NULL)
    {
        memcpy(ref, cw, FC_BUFFER_POOL_CW_SIZE);
    }
}

/** 在 CW 的 bit 区间 [lo, hi) 里随机翻 k 个**互不重复**的 bit，逐个打印位置。
 * k ≤ 8（BCH t=4 的用例最多用到 4；留点余量给后续超能力冒烟）。 */
static void l2_ecc_flip(BYTE *cw, DWORD lo, DWORD hi, DWORD k)
{
    DWORD pos[8];
    DWORD i;
    DWORD j;

    ASSERT(k <= 8U);
    ASSERT(lo < hi);

    for (i = 0U; i < k; i++)
    {
        BYTE dup;

        do
        {
            pos[i] = lo + (DWORD)munit_rand_int_range(0, (int)(hi - lo) - 1);
            dup    = 0U;
            for (j = 0U; j < i; j++)
            {
                if (pos[j] == pos[i])
                {
                    dup = 1U;
                }
            }
        } while (dup != 0U);

        cw[pos[i] / 8U] ^= (1U << (pos[i] % 8U));
        test_trace_detail("  flip bit %lu (cw[%lu] bit %lu)", pos[i], (pos[i] / 8U), (pos[i] % 8U));
    }
}

/* ── 场景表 ──
 *
 * 每行是**手挑的一个场景**（不是维度乘积）。注入分两段：
 *   d_k 个 bit 落在数据区 [d_lo, d_hi)；p_k 个 bit 落在 parity 区 [4160, 4208)。
 * d_k == 0 时 d_lo/d_hi 不参与（填 0 占位）。
 *
 * 列序：name / d_lo / d_hi / d_k / p_k / expect。
 *
 * expect 语义：
 *   EXACT —— 必可纠：status == OK、纠正数 == d_k + p_k、缓冲区回到基准。
 *            回比长度由 p_k 定：p_k == 0 ⇒ parity 没被动过，比整块 528B；
 *            p_k > 0 ⇒ 解码器只翻 errloc < 4160 的位（fc_ldpc_engine.c），
 *            parity 字节**不回写**，只比数据区 520B。
 *   BOUND —— 超能力：结果不确定，只钉不变式（见用例体）。
 *
 * 覆盖面：干净 / META / CRC 字段 / USER / k=2..t 的整数据区 / parity 单侧 k=1 与
 * k=t / 数据+parity 混合 / 超能力边界。CRC 字段单列是因为它**自身也被 ECC 保护**——
 * 若 ECC 修不好 CRC 字段，Pass 3 的 CRC 校验会跟着把这一页判错。 */
typedef enum L2_ECC_EXPECT_E
{
    L2_ECC_EXACT,
    L2_ECC_BOUND
} l2_ecc_expect_e;

typedef struct L2_ECC_CASE_S
{
    const char     *name;
    DWORD           d_lo; /* 数据区注入区间（bit）；d_k == 0 时不参与 */
    DWORD           d_hi;
    DWORD           d_k; /* 数据区注入 bit 数 */
    DWORD           p_k; /* parity 区注入 bit 数 */
    l2_ecc_expect_e expect;
} l2_ecc_case_s;

static const l2_ecc_case_s l2_ecc_cases[] = {
    {   "clean",             0U, L2_ECC_DATA_BITS, 0U, 0U, L2_ECC_EXACT},
    {  "meta-1",             0U, L2_ECC_META_BITS, 1U, 0U, L2_ECC_EXACT},
    {   "crc-1",  L2_ECC_CRC_LO,    L2_ECC_CRC_HI, 1U, 0U, L2_ECC_EXACT},
    {  "user-1", L2_ECC_USER_LO, L2_ECC_DATA_BITS, 1U, 0U, L2_ECC_EXACT},
    {  "data-2",             0U, L2_ECC_DATA_BITS, 2U, 0U, L2_ECC_EXACT},
    {  "data-3",             0U, L2_ECC_DATA_BITS, 3U, 0U, L2_ECC_EXACT},
    {  "data-4",             0U, L2_ECC_DATA_BITS, 4U, 0U, L2_ECC_EXACT},
    {"parity-1",             0U,               0U, 0U, 1U, L2_ECC_EXACT},
    {"parity-4",             0U,               0U, 0U, 4U, L2_ECC_EXACT},
    { "mixed-4",             0U, L2_ECC_DATA_BITS, 2U, 2U, L2_ECC_EXACT},
    {"beyond-5",             0U, L2_ECC_DATA_BITS, 5U, 0U, L2_ECC_BOUND},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_ecc_cases 一一对应——
 * 下面一条 _Static_assert 把两表数量钉死，改名/增删时不会漏。 */
static const char *l2_ecc_case_names[] = {
    "clean",
    "meta-1",
    "crc-1",
    "user-1",
    "data-2",
    "data-3",
    "data-4",
    "parity-1",
    "parity-4",
    "mixed-4",
    "beyond-5",
    NULL,
};

_Static_assert((sizeof(l2_ecc_case_names) / sizeof(l2_ecc_case_names[0]) - 1U) == (sizeof(l2_ecc_cases) / sizeof(l2_ecc_cases[0])), "l2_ecc_case_names 与 l2_ecc_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_ecc_params[] = {
    {"case", l2_ecc_case_names},
    {  NULL,              NULL},
};

/** 类型化入口，查行逻辑见 l2_case_find。 */
static const l2_ecc_case_s *l2_ecc_case_find(const char *name)
{
    return (const l2_ecc_case_s *)l2_case_find(name, l2_ecc_case_names, l2_ecc_cases, sizeof(l2_ecc_cases[0]));
}

/* ── 用例体：造基准 → 注错 → 解码 → 判据 ──
 *
 * 四段（下面对应四处 ①②③④ 注释）：
 *   ① l2_ecc_cw_build()          造一块"写路径形态"的干净 CW，另存一份基准 ref
 *   ② l2_ecc_flip()              按场景表在指定 bit 区间翻 k 个**互不重复**的 bit
 *   ③ fc_data_path_ecc_decode()  **被测函数**：唯一一次调用，之后 cw 就是"读回来"的样子
 *   ④ 判据                       EXACT 逐字节回比 ref；BOUND 只钉两条不变式
 *
 * 三个 scratch 缓冲（test_cw_scratch，静态分配，用例之间不复用）：
 *   cw  —— 工作副本，全程就地修改（注错 → 解码纠错）
 *   ref —— 干净基准，只在 EXACT 判据里被读
 *   inj —— 仅 BOUND 用：**注入后、解码前**的快照，用来验"失败必须原子"
 *
 * 一个用例跑遍场景表（`case` 参数选行）：每组一次独立的 setup/tear_down，失败互不遮蔽。
 * ⚠ 运行器在**每条参数化用例**开头都用同一个 `runner->seed` 重置 PRNG（munit.c / :1125）
 *   ⇒ 本用例各 `case` 都从**同一段**随机流起抽 ⇒ 注入位置**逐 case 嵌套**（data-4 ⊇ data-3
 *   ⊇ data-2，beyond-5 ⊇ data-4）。是"每次多一个错"的叠加，不是 N 组独立抽样。
 * ecc_bits 预置 0xAA 当哨兵——证明解码**确实写了**它，而不是"恰好为 0"（哨兵取 0 就分不开）。 */
static MunitResult test_ecc_step(const MunitParameter params[], void *data)
{
    const l2_ecc_case_s *c        = l2_ecc_case_find(munit_parameters_get(params, "case"));
    BYTE                *cw       = test_cw_scratch(0U);
    BYTE                *ref      = test_cw_scratch(1U);
    BYTE                *inj      = test_cw_scratch(2U); /* 仅 BOUND 用：注入后、解码前的快照 */
    BYTE                 ecc_bits = 0xAAU;
    fc_status_e          status;
    DWORD                total;

    (void)data;

    test_trace_begin();

    /* 抽地址（在 test_trace_begin 之后——它的 detail 行要落在前缀行收尾之后）。 */
    const nand_addr_s addr = l2_addr_draw("ecc_step", 0U);

    munit_assert_true(c != NULL); /* 只有 --param case 传了表外的名字才会是 NULL */
    total = c->d_k + c->p_k;      /* 期望纠正数 = 数据区注入数 + parity 区注入数 */

    /* ① 造干净码字（META + USER + CRC + ECC），ref 留一份基准副本 */
    l2_ecc_cw_build(&addr, cw, ref);

    /* ② 注错。两段区间不重叠（数据区 [0,4160) / parity 区 [4160,4208)）。
     *    ⚠ d_k == 0 的场景 d_lo/d_hi 是占位 0，必须靠 d_k 跳过——否则会把 0 当区间
     *    传进 l2_ecc_flip，撞上它的 ASSERT(lo < hi)。 */
    if (c->d_k != 0U)
    {
        l2_ecc_flip(cw, c->d_lo, c->d_hi, c->d_k);
    }
    if (c->p_k != 0U)
    {
        l2_ecc_flip(cw, L2_ECC_PARITY_LO, L2_ECC_PARITY_HI, c->p_k);
    }

    test_trace_summary("case=%s inject=%lu", c->name, total);

    /* BOUND 的"原子性"基准必须是**注入后**的状态：cw 本来就带 5 个错 bit，拿它跟干净的
     * ref 比永远不等。EXACT 用不到 inj。 */
    if (c->expect == L2_ECC_BOUND)
    {
        memcpy(inj, cw, FC_BUFFER_POOL_CW_SIZE);
    }

    /* ③ 被测：唯一一次解码调用，就地纠错并回报纠正位数 */
    status = fc_data_path_ecc_decode(cw, &ecc_bits);

    if (c->expect == L2_ECC_BOUND)
    {
        /* 超能力：BCH 允许误纠 ⇒ 只钉两条**恒成立**的不变式，不钉结果。
         * ① 失败必须原子：IO_ERROR 时缓冲一字节不动（对照**注入后**快照 inj，
         *    **不是**干净码字 ref——cw 本来就带 5 个错 bit，跟 ref 比永远不等）、
         *    且纠正数为 0——IO_ERROR 在翻转循环**之前**就 return（fc_ldpc_engine.c），
         *    而 err_bits 早在 fc_ldpc_decode 入口就被置 0（fc_ldpc_engine.c）⇒
         *    既没被写坏、也不是"没写"。
         * ② 纠正数不得超过 t=4：errloc[4] 定长，BM 至多 t 个根。 */
        if (status == FC_STATUS_IO_ERROR)
        {
            munit_assert_uint8(ecc_bits, ==, 0U);
            munit_assert_memory_equal(FC_BUFFER_POOL_CW_SIZE, cw, inj);
        }
        else
        {
            munit_assert_int(status, ==, FC_STATUS_OK);
            munit_assert_uint8(ecc_bits, <=, 4U);
        }
        return MUNIT_OK;
    }

    /* ④b 能力内（k ≤ t=4）：必须**精确**纠正，三条一起钉——
     *    · 状态 OK；
     *    · 纠正数**恰好等于**注入数：多纠 = 纠到了没坏的位置，比漏纠更危险；
     *    · 缓冲区逐字节回到基准（回比长度见下）。
     * ⚠ 回比长度看 parity 有没有被动过：p_k == 0 ⇒ parity 是干净的，整块 528B 都要比；
     *   p_k > 0 ⇒ 解码器只翻 errloc < 4160 的位（fc_ldpc_engine.c），parity 字节
     *   **不回写**，只能比数据区 520B。 */
    munit_assert_int(status, ==, FC_STATUS_OK);
    munit_assert_uint8(ecc_bits, ==, total);
    munit_assert_memory_equal((c->p_k == 0U) ? FC_BUFFER_POOL_CW_SIZE : FC_BUFFER_DATA_SIZE, cw, ref);

    return MUNIT_OK;
}

/*************************************************************************
 *  fc_data_path_encode 测试
 *
 *  验证管线（Stage 3 跨命令 stripe RAID 已移出，见 fc_data_path.c）：
 *     Data → META → CRC? → Random 加扰? → ECC 编码
 *
 *  这几条**只 encode、不 process_read**：查的是编码产物本身。
 *  往返（encode→process_read→明文）由下一节的用例承担。
 *
 *  场景表（参数化，`--param case <name>` 可单跑）：
 *     case            random  crc     cw    期望 USER 区
 *     random-off-4cw  off     on      4CW   仍为明文
 *     random-on-4cw   on      on      4CW   **已非明文**（自证加扰真的发生）
 *     single-cw       off     on      1CW   仍为明文
 *
 *  三条断言集完全相同（cw_count / meta / crc / parity / USER 区），只有两列期望不同
 *  （cw 数、USER 区是否仍是明文）⇒ 判据 7「期望不同 → 参数化」。
 *  crc 关闭是**另一套断言**（查 [4,8) 哨兵未被写、无 parity / USER 断言）⇒ 独立成条。
 **************************************************************************/

typedef struct L2_ENC_CASE_S
{
    const char *name;
    DWORD       cw_bitmap;
    BYTE        random_mode;
    BYTE        crc_mode;
    BYTE        n_cw;         /* 期望链上 desc 数 */
    BYTE        expect_plain; /* 1 = encode 后 USER 区仍等于明文 */
} l2_enc_case_s;

static const l2_enc_case_s l2_enc_cases[] = {
    {"random-off-4cw", L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE, CW_NUM, 1U},
    { "random-on-4cw", L2_BM_PLANE0_ALL,  FC_RANDOM_ENABLE, FC_CRC_ENABLE, CW_NUM, 0U},
    {     "single-cw",            0x01U, FC_RANDOM_DISABLE, FC_CRC_ENABLE,     1U, 1U},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_enc_cases 一一对应——
 * 下面一条 _Static_assert 把两表数量钉死，改名/增删时不会漏。 */
static const char *l2_enc_case_names[] = {
    "random-off-4cw",
    "random-on-4cw",
    "single-cw",
    NULL,
};

_Static_assert((sizeof(l2_enc_case_names) / sizeof(l2_enc_case_names[0]) - 1U) == (sizeof(l2_enc_cases) / sizeof(l2_enc_cases[0])), "l2_enc_case_names 与 l2_enc_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_enc_params[] = {
    {"case", l2_enc_case_names},
    {  NULL,              NULL},
};

/** 类型化入口，查行逻辑见 l2_case_find。 */
static const l2_enc_case_s *l2_enc_case_find(const char *name)
{
    return (const l2_enc_case_s *)l2_case_find(name, l2_enc_case_names, l2_enc_cases, sizeof(l2_enc_cases[0]));
}

/* ── 用例体：建链 → 填明文 → encode → 自证编码产物 ──
 *
 * 只调 fc_data_path_encode，**不调 process_read**：本用例的靶子是"写路径真的按 config
 * 位把该做的都做了"，往返正确性由 process_read_ok 负责，两处不重复。
 *
 * 判据两层：
 *   ① l2_user_matches_source(enc) == c->expect_plain
 *      random=ON ⇒ encode 后 USER 区**必须已不是明文**；random=OFF ⇒ 必须仍是明文。
 *      这是**自证**：fixture 一旦退化（例如 seed 恒 0 使 keystream 整片为 0），
 *      "encode 后 process_read 回到明文"这条往返恒等式会照样成立、把问题盖过去。
 *   ② meta / crc / parity 三条产物自证（见文件头「产物自证」），分别钉 Stage 0 / 1 / 2
 *      真写了各自的字段，而不是"恰好看不出来"。
 *
 * n_cw 一并钉住：建链几何错（多 / 少一条 desc）会先在这里露头。 */
static MunitResult test_encode(const MunitParameter params[], void *data)
{
    const l2_enc_case_s *c;
    fc_cmd_s            *enc;

    (void)data;

    test_trace_begin();

    /* 抽地址（在 test_trace_begin 之后——它的 detail 行要落在前缀行收尾之后）。 */
    const nand_addr_s addr = l2_addr_draw("encode", 0U);

    c = l2_enc_case_find(munit_parameters_get(params, "case"));
    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */

    enc = l2_cmd(&addr, c->cw_bitmap, c->random_mode, c->crc_mode);
    munit_assert_uint8(test_cw_count(enc), ==, c->n_cw);

    test_fill_user_paa(enc);
    munit_assert_int(fc_data_path_encode(enc->cmd_id), ==, FC_STATUS_OK);

    /* random=ON ⇒ encode 后 USER 区应已不是明文（[0,8) 是 META + CRC，不属于 USER 区）。
     * 这条是自证：fixture 一旦退化（例如 seed 恒 0 使 keystream 整片为 0），
     * 往返恒等式会把问题原样盖过去。 */
    munit_assert_uint8(l2_user_matches_source(enc), ==, c->expect_plain);

    test_trace_summary("case=%s bm=0x%02lX random=%hhu crc=%hhu: %hhu CW, USER%s明文", c->name, c->cw_bitmap, c->random_mode, c->crc_mode, c->n_cw, (c->expect_plain != 0U) ? " 仍是" : " 已非");

    l2_verify_meta_written(enc);
    l2_verify_crc_written(enc); /* CRC 对明文算 ⇒ 与 USER 是否已加扰无关 */
    l2_verify_parity_nonzero(enc);

    fc_command_release(enc->cmd_id);
    return MUNIT_OK;
}

/** CRC 关闭分支：encode 不写 CRC 字段（哨兵原样保留）→ 读侧 Pass 3 整个跳过 → 无 CRC_ERROR */
static MunitResult test_encode_crc_disabled(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    const nand_addr_s addr = l2_addr_draw("encode_crc_disabled", 0U);
    fc_cmd_s         *enc  = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

    munit_assert_uint8(test_cw_count(enc), ==, CW_NUM);

    test_fill_user_paa(enc);
    for (BYTE i = 0U; i < test_cw_count(enc); i++)
    {
        /* 哨兵：确认 encode 在 CRC 关闭时确实没碰 [4,8) */
        memset(test_cw_base(enc, i) + FC_CRC_META_SIZE, 0xFF, FC_CRC_FIELD_SIZE);
    }

    munit_assert_int(fc_data_path_encode(enc->cmd_id), ==, FC_STATUS_OK);

    /* META 与 CRC 是两件独立的事：关 CRC 不影响 Stage 0 */
    l2_verify_meta_written(enc);

    for (BYTE i = 0U; i < test_cw_count(enc); i++)
    {
        munit_assert_uint32(test_check_pattern(test_cw_base(enc, i) + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE, 0xFFU), ==, 1U);
    }

    /* 证据（--log-visible info）：CRC 关闭时该字段应**原样保留哨兵** 0xFFFFFFFF
     * ⇒ 读侧 Pass 3 整个跳过。打出来才看得出"关闭"是真的没写，而不是写了个 0。 */
    test_trace_summary("crc disabled: %hhu CW, [4,8) 全 0xFFFFFFFF（哨兵原样保留）", test_cw_count(enc));

    fc_command_release(enc->cmd_id);
    return MUNIT_OK;
}

/*************************************************************************
 *  介质格式 KAT（golden vector）
 *
 *  这几条用例的目的不是再测一遍功能，而是**冻结介质格式**：写进 NAND 的
 *  parity / 加扰序列 / CRC 变体一旦漂移，旧固件写的页在新固件上就不可读。
 *  现有往返用例是「同源编解码」，编码器/解码器一起错时仍然通过
 *  （docs/design/fc_nand_ut.md §6.2 缺口⑥、§6.3），故必须用固定输入 +
 *  独立算出的期望值钉住。
 *
 *  ⚠ 期望值不是从被测实现反推的，来源写在各用例上方。
 **************************************************************************/

/* BCH 参数与生产配置一致（fc_bch_init(13, 4, 0, 0)）：
 * parity = m*t = 52 bit ⇒ 7B；ECC 槽 = CW 528B - data 520B = 8B。 */
#define L2_BITS_PER_BYTE     (8U)
#define L2_BCH_KAT_M         (13U)
#define L2_BCH_KAT_T         (4U)
#define L2_BCH_KAT_BITS      (L2_BCH_KAT_M * L2_BCH_KAT_T)
#define L2_BCH_KAT_ECC_BYTES ((L2_BCH_KAT_BITS + L2_BITS_PER_BYTE - 1U) / L2_BITS_PER_BYTE)
#define L2_BCH_KAT_ECC_SLOT  (FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE)

/* 固定输入图案参数；改这两个值会同时改变 golden。 */
#define L2_BCH_KAT_STEP (37U)
#define L2_BCH_KAT_BASE (11U)

/* 固定错误位置：按数据区位宽四等分，避免只测同一字节。 */
#define L2_BCH_KAT_DATA_BITS (FC_BUFFER_DATA_SIZE * L2_BITS_PER_BYTE)

/* KAT 负例的翻转掩码：只翻 1 bit，证明判据有区分力。
 * BCH 用 0x08 与 scramble/CRC 的 0x01 错开，避免负例都落在同一 bit。 */
#define L2_KAT_FLIP_MASK     (0x01U)
#define L2_BCH_KAT_FLIP_MASK (0x08U)

/* BCH parity 固定向量。
 *
 * 输入：data[i] = (i * L2_BCH_KAT_STEP + L2_BCH_KAT_BASE) & 0xFF。
 * 独立来源：Python 实现的 GF(2^13)（prim=0x201B）、
 * g(x)=∏minpoly(α^1,α^3,α^5,α^7)、MSB-first 系统编码；与本仓
 * fc_bch_encode 逐字节一致。期望 7B parity（第 8 字节保持 0）：
 * B5 53 C5 CC 06 7E 60。 */
static const BYTE s_bch_kat_ecc[L2_BCH_KAT_ECC_BYTES] = {0xB5U, 0x53U, 0xC5U, 0xCCU, 0x06U, 0x7EU, 0x60U};

/* scramble_kat — 加扰序列固定向量。
 *
 * 输入：data[i] = i & 0xFF（512B），seed 由固定 META 0x12345678 派生。
 * 期望：seed = 0x021C1E0F；前 16B 与全 512B 的 FNV-1a32 如下。
 * 独立来源：按 fc_random_engine.c 注释的多项式 x^16+x^14+x^13+x^11+1
 * （右移、抽头 bit0^bit2^bit3^bit5）用 Python 独立实现，逐字节一致。
 * 统计/自反判据测不出序列漂移（fc_nand_ut.md §6.3），这条负责冻结序列。 */
#define L2_SCRAMBLE_KAT_META       0x12345678UL
#define L2_SCRAMBLE_KAT_SEED       0x021C1E0FUL
#define L2_SCRAMBLE_KAT_FNV        0x8499365CUL
#define L2_SCRAMBLE_KAT_PREFIX_LEN (16U) /* 失败时可读的前缀长度；完整序列由 FNV 钉住 */

/* FNV-1a 32-bit 规范常量（offset basis / prime），不是本仓自造。 */
#define L2_FNV1A32_OFFSET_BASIS (0x811C9DC5UL)
#define L2_FNV1A32_PRIME        (0x01000193UL)

/* CRC 固定向量。
 *
 * 512B USER：data[i] = i & 0xFF，期望 MPEG-2 = 0xEA3BEC76。
 * ⚠ 本仓按 STM32 word 顺序喂 CRC：每个 4B 组入序是
 *   data[3],data[2],data[1],data[0]（不是标准字节流的 data[0..3]）。
 *   该期望由独立 Python 按同一 word 顺序算出；标准字节流顺序是
 *   0xD08D7AD9，不能混用。
 * ISO-HDLC check value "123456789" = 0xCBF43926（标准）。
 *
 * 序列断言 ISO → MPEG-2 → ISO 覆盖 crc_port_compute_iso_hdlc 的
 * 「临时切硬件配置后恢复 MPEG-2」路径：恢复失败时同一次 boot 内
 * 后续写页的 CRC 会算错，重启后 CRC 校验失败。 */
#define L2_CRC_KAT_MPEG2    (0xEA3BEC76UL)
#define L2_CRC_KAT_ISO_HDLC (0xCBF43926UL)

/** FNV-1a 32-bit：KAT 用独立摘要，避免在测试里再实现一遍被测算法。 */
static DWORD l2_fnv1a32(const BYTE *data, DWORD length)
{
    DWORD hash = L2_FNV1A32_OFFSET_BASIS;

    for (DWORD i = 0U; i < length; i++)
    {
        hash ^= data[i];
        hash *= L2_FNV1A32_PRIME;
    }
    return hash;
}

/** BCH parity KAT：固定 520B → 固定 7B；4 bit 错误必须精确纠正。 */
static MunitResult test_bch_parity_kat(const MunitParameter params[], void *data)
{
    static BYTE               kat_data[FC_BUFFER_DATA_SIZE];
    static BYTE               kat_work[FC_BUFFER_DATA_SIZE];
    static BYTE               kat_ecc[L2_BCH_KAT_ECC_SLOT];
    static BYTE               kat_ecc_flip[L2_BCH_KAT_ECC_SLOT];
    static const unsigned int err_pos[L2_BCH_KAT_T] = {
        0U,
        L2_BCH_KAT_DATA_BITS / 4U,
        L2_BCH_KAT_DATA_BITS / 2U,
        (L2_BCH_KAT_DATA_BITS * 3U) / 4U,
    };

    (void)params;
    (void)data;

    test_trace_begin();

    for (DWORD i = 0U; i < sizeof(kat_data); i++)
    {
        kat_data[i] = (BYTE)((i * L2_BCH_KAT_STEP + L2_BCH_KAT_BASE) & 0xFFU);
    }

    fc_bch_control_s *bch = fc_bch_init((int)L2_BCH_KAT_M, (int)L2_BCH_KAT_T, 0U, 0U);

    munit_assert_not_null(bch);

    memset(kat_ecc, 0, sizeof(kat_ecc));
    fc_bch_encode(bch, kat_data, sizeof(kat_data), kat_ecc);
    munit_assert_uint8(kat_ecc[L2_BCH_KAT_ECC_BYTES], ==, 0U); /* 第 8 字节不属于 52-bit parity */
    munit_assert_memory_equal(sizeof(s_bch_kat_ecc), kat_ecc, s_bch_kat_ecc);

    /* C7 负例：输入翻 1 bit，parity 必须变化（证明向量有区分力）。 */
    memcpy(kat_work, kat_data, sizeof(kat_work));
    kat_work[0] ^= L2_BCH_KAT_FLIP_MASK;
    memset(kat_ecc_flip, 0, sizeof(kat_ecc_flip));
    fc_bch_encode(bch, kat_work, sizeof(kat_work), kat_ecc_flip);
    munit_assert_int(memcmp(kat_ecc_flip, s_bch_kat_ecc, sizeof(s_bch_kat_ecc)), !=, 0);

    /* 解码侧：固定 L2_BCH_KAT_T 个 bit 错误按 golden parity 精确纠正。 */
    memcpy(kat_work, kat_data, sizeof(kat_work));
    for (BYTE i = 0U; i < L2_BCH_KAT_T; i++)
    {
        kat_work[err_pos[i] / L2_BITS_PER_BYTE] ^= 1U << (err_pos[i] % L2_BITS_PER_BYTE);
    }

    unsigned int errloc[L2_BCH_KAT_T];
    unsigned int nerr = 0U;

    munit_assert_true(fc_bch_decode(bch, kat_work, sizeof(kat_work), s_bch_kat_ecc, errloc, &nerr));
    munit_assert_uint32(nerr, ==, L2_BCH_KAT_T);
    for (BYTE i = 0U; i < nerr; i++)
    {
        kat_work[errloc[i] / L2_BITS_PER_BYTE] ^= 1U << (errloc[i] % L2_BITS_PER_BYTE);
    }
    munit_assert_memory_equal(sizeof(kat_work), kat_work, kat_data);

    test_trace_summary("bch kat: %uB -> parity %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX %02hhX; %u-bit errors corrected",
                       sizeof(kat_data),
                       kat_ecc[0],
                       kat_ecc[1],
                       kat_ecc[2],
                       kat_ecc[3],
                       kat_ecc[4],
                       kat_ecc[5],
                       kat_ecc[6],
                       L2_BCH_KAT_T);
    return MUNIT_OK;
}

/** 加扰序列 KAT：固定 seed + 固定明文 → 固定密文摘要；自反仍成立。 */
static MunitResult test_scramble_kat(const MunitParameter params[], void *data)
{
    static DWORD      words[FC_BUFFER_USER_SIZE / sizeof(DWORD)]; /* DWORD 数组保证 4B 对齐 */
    static const BYTE prefix[L2_SCRAMBLE_KAT_PREFIX_LEN] = {0xF0U, 0x79U, 0xBFU, 0x4BU, 0x0FU, 0x2CU, 0x1DU, 0xE1U, 0xDFU, 0x98U, 0xCDU, 0x6BU, 0x01U, 0xECU, 0xE7U, 0x5BU};
    BYTE             *buf                                = (BYTE *)words;

    (void)params;
    (void)data;

    test_trace_begin();

    for (DWORD i = 0U; i < FC_BUFFER_USER_SIZE; i++)
    {
        buf[i] = i & 0xFFU;
    }

    const DWORD seed = fc_random_seed_from_meta(L2_SCRAMBLE_KAT_META);

    munit_assert_uint32(seed, ==, L2_SCRAMBLE_KAT_SEED);

    fc_random_transform(buf, FC_BUFFER_USER_SIZE, seed);
    munit_assert_memory_equal(sizeof(prefix), buf, prefix);
    munit_assert_uint32(l2_fnv1a32(buf, FC_BUFFER_USER_SIZE), ==, L2_SCRAMBLE_KAT_FNV);

    /* C7 负例：明文翻 1 bit，输出摘要必须变化。 */
    for (DWORD i = 0U; i < FC_BUFFER_USER_SIZE; i++)
    {
        buf[i] = i & 0xFFU;
    }
    buf[0] ^= L2_KAT_FLIP_MASK;
    fc_random_transform(buf, FC_BUFFER_USER_SIZE, seed);
    munit_assert_uint32(l2_fnv1a32(buf, FC_BUFFER_USER_SIZE), !=, L2_SCRAMBLE_KAT_FNV);

    /* 自反：同一函数再变换一次回到明文。 */
    for (DWORD i = 0U; i < FC_BUFFER_USER_SIZE; i++)
    {
        buf[i] = i & 0xFFU;
    }
    fc_random_transform(buf, FC_BUFFER_USER_SIZE, seed);
    fc_random_transform(buf, FC_BUFFER_USER_SIZE, seed);
    for (DWORD i = 0U; i < FC_BUFFER_USER_SIZE; i++)
    {
        munit_assert_uint8(buf[i], ==, i & 0xFFU);
    }

    test_trace_summary("scramble kat: seed=0x%08lX fnv=0x%08lX; inverse held", seed, L2_SCRAMBLE_KAT_FNV);
    return MUNIT_OK;
}

/** CRC KAT：MPEG-2 512B + ISO-HDLC check value + ISO→MPEG-2 恢复序列。 */
static MunitResult test_crc_kat(const MunitParameter params[], void *data)
{
    static const BYTE iso_kat[] = "123456789";
    static DWORD      user_words[FC_CRC_USER_SIZE / sizeof(DWORD)]; /* DWORD 数组保证 4B 对齐 */
    BYTE             *user = (BYTE *)user_words;

    (void)params;
    (void)data;

    test_trace_begin();

    for (DWORD i = 0U; i < FC_CRC_USER_SIZE; i++)
    {
        user[i] = i & 0xFFU;
    }

    /* 先 ISO 再 MPEG-2：硬件后端会临时切换 CRC 配置，这里验证它恢复成功。 */
    munit_assert_uint32(crc_compute_iso_hdlc(iso_kat, (sizeof(iso_kat) - 1U)), ==, L2_CRC_KAT_ISO_HDLC);
    munit_assert_uint32(fc_crc_compute(user), ==, L2_CRC_KAT_MPEG2);
    munit_assert_uint32(crc_compute_iso_hdlc(iso_kat, (sizeof(iso_kat) - 1U)), ==, L2_CRC_KAT_ISO_HDLC);
    munit_assert_uint32(fc_crc_compute(user), ==, L2_CRC_KAT_MPEG2);

    /* C7 负例：USER 翻 1 bit，MPEG-2 必须变化。 */
    user[0] ^= L2_KAT_FLIP_MASK;
    munit_assert_uint32(fc_crc_compute(user), !=, L2_CRC_KAT_MPEG2);

    test_trace_summary("crc kat: MPEG2(%uB)=0x%08lX, ISO(%uB)=0x%08lX, ISO->MPEG2 restore held",
                       FC_CRC_USER_SIZE,
                       L2_CRC_KAT_MPEG2,
                       sizeof(iso_kat) - 1U,
                       L2_CRC_KAT_ISO_HDLC);
    return MUNIT_OK;
}

/*************************************************************************
 *  fc_data_path_process_read 测试
 *
 *  验证管线（Pass 1~4）：
 *     Buffer → ECC 解码 → 全部成功? → 解扰? → CRC 校验? → META 校验
 *                              ↓ 否
 *                        不解扰，返回 IO_ERROR
 *
 *  ⚠ Pass 3/Pass 4 只置 desc status 位、**不改返回值**（fc_data_path.c 恒返回 OK），
 *    所以"读成功"必须查 desc->status，只看返回值会漏掉 CRC / META 校验回归。
 *  ⚠ Pass 2「解扰」的条件是**全部 CW 都通过 ECC**——有一条失败就整条命令都不解扰，
 *    未破坏的 CW 也停在加扰态（见 test_process_read_unc 的断言）。
 *  ⚠ process_read 是**原地解码**的：对同一个 buffer 跑两遍，第二遍面对的是已解扰的
 *    数据区，ECC 校验子整体失配。故错误注入一律「**先破坏、只读一次**」。
 *
 *  用例划分：
 *     process_read_ok          参数化 3 case（random-on-4cw / random-off-4cw / single-cw）——
 *                              三条只是 l2_buffer_round_trip 的三组入参、断言完全相同
 *     process_read_unc         有 UNC → 不解扰，返回 IO_ERROR
 *     process_read_masked_chain 2CW / 0x05 非连续掩码：链位 0,1 ↔ cw 0,2
 *     process_read_meta_error  META 不符 → META_ERROR
 *     process_read_crc_error   CRC 不符 → CRC_ERROR
 *     process_read_ldpc_raw    LDPC_RAW：跳过 ECC 编码/解码
 **************************************************************************/

/* 三条 OK 往返的入参恰好就是 l2_buffer_round_trip 的三个形参，断言全在 helper 里
 * ⇒ 判据 7「同一断言、不同输入」。这里仍用**参数化**而非用例内循环：失败时 munit
 * 会把 case 名带进报错，且 `--param case single-cw` 能单跑（补 munit_parameters_get
 * 换来的能力）。 */
typedef struct L2_RT_CASE_S
{
    const char *name;
    DWORD       cw_bitmap;
    BYTE        random_mode;
    BYTE        crc_mode;
} l2_rt_case_s;

static const l2_rt_case_s l2_rt_cases[] = {
    { "random-on-4cw", L2_BM_PLANE0_ALL,  FC_RANDOM_ENABLE, FC_CRC_ENABLE},
    {"random-off-4cw", L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE},
    {     "single-cw",            0x01U,  FC_RANDOM_ENABLE, FC_CRC_ENABLE},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_rt_cases 一一对应。 */
static const char *l2_rt_case_names[] = {
    "random-on-4cw",
    "random-off-4cw",
    "single-cw",
    NULL,
};

_Static_assert((sizeof(l2_rt_case_names) / sizeof(l2_rt_case_names[0]) - 1U) == (sizeof(l2_rt_cases) / sizeof(l2_rt_cases[0])), "l2_rt_case_names 与 l2_rt_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_rt_params[] = {
    {"case", l2_rt_case_names},
    {  NULL,             NULL},
};

/** 类型化入口，查行逻辑见 l2_case_find。 */
static const l2_rt_case_s *l2_rt_case_find(const char *name)
{
    return (const l2_rt_case_s *)l2_case_find(name, l2_rt_case_names, l2_rt_cases, sizeof(l2_rt_cases[0]));
}

/* ── 用例体：正向往返（encode → process_read → 逐字节比对）──
 *
 * 断言全在 l2_buffer_round_trip 里：往返 USER 逐字节 == 源表、每个 CW **恰好** READY。
 * 本函数只负责选行、把"证明了什么"打进证据摘要。
 *
 * 判据用 l2_verify_all_ready 而非夹具的 test_verify_cw_status_all_success：后者只看
 * 成功位，而 descriptor 成功 = FC_CW_STATUS_UNUSED(0)，CRC_ERROR / META_ERROR
 * 会与它共存而不被发现 —— 正向用例要的是"一个错误位都没有"。
 * ⚠ 全程**不提交**命令：提交后 FC 会自己再跑一遍 encode / process_read，重复编码会让
 *   用例空转（见文件头「全部用例都是 buffer 级」）。 */
static MunitResult test_process_read_ok(const MunitParameter params[], void *data)
{
    const l2_rt_case_s *c = l2_rt_case_find(munit_parameters_get(params, "case"));
    BYTE                cw_count;

    (void)data;

    test_trace_begin();

    /* 抽地址（在 test_trace_begin 之后——它的 detail 行要落在前缀行收尾之后）。 */
    const nand_addr_s addr = l2_addr_draw("process_read_ok", 0U);

    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */
    cw_count = l2_buffer_round_trip(&addr, c->cw_bitmap, c->random_mode, c->crc_mode);

    /* 证据摘要（--log-visible info）：断言全在 helper 里，这里只把「证明了什么」打出来
     * ——链上实际 CW 数、比对字节数、以及「全部 READY + USER 回到源」这两个结论。 */
    test_trace_summary("case=%s bm=0x%02lX: encode+process_read 往返一致，%hhu CW x %lu B，全部 READY 且 USER == 源", c->name, c->cw_bitmap, cw_count, ((DWORD)cw_count * FC_BUFFER_USER_SIZE));

    return MUNIT_OK;
}

/** UNC：**先破坏、只读一次**。
 * 未破坏的 CW0/2/3 读回 READY 就是"这一页本来是好的"的对照组——无需先跑一遍干净读：
 * process_read 是原地解码的，跑两遍会让第二遍面对已解扰的 buffer（ECC 校验子整体失配），
 * 把对照组一起判成 UNC。
 * ⚠ 破坏必须**远超** BCH 纠错能力，否则会被"纠正"回去、用例假过：
 *   按 8 字节步长翻转整个 520B 数据区（65 处、最多 520 bit）——8B parity 纠不了。 */
static MunitResult test_process_read_unc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    const nand_addr_s addr = l2_addr_draw("process_read_unc", 0U);
    fc_cmd_s         *cmd  = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

    /* 破坏 CW1 */
    {
        BYTE *base = test_cw_base(cmd, 1U);

        for (WORD j = 0U; j < FC_BUFFER_DATA_SIZE; j += 8U)
        {
            base[j] ^= 0xFFU;
        }
    }

    /* process_read 应返回 IO_ERROR（不解扰） */
    munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_IO_ERROR);

    /* 验证 desc status：CW1 为 UNC，其余为 READY（Pass 1 是**赋值**，错误位不会残留） */
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status, ==, (i == 1U) ? FC_CW_STATUS_UNC : FC_CW_STATUS_UNUSED);
    }

    /* Pass 2 被跳过（"全部成功才解扰"）⇒ 未破坏的 CW0 也仍停在**加扰态**、不是明文。
     * 这条把"出错时不部分解扰"钉住；random=OFF 时它恒不成立，故本用例必须 random=ON。 */
    munit_assert_uint8(test_check_paa(test_cw_user(cmd, 0U), test_cw_desc(cmd, 0U)->paa), ==, 0U);

    /* 证据（--log-visible info）：哪几个 CW 报了什么。裸 [ OK ] 看不出"确实只坏了 CW1、
     * 对照组干净"——而那正是这条用例的全部意义。 */
    test_trace_summary("unc: 破坏 CW1（%u 处 x 8B 步长）-> process_read=IO_ERROR, cw1=UNC 其余 READY, Pass 2 未解扰", (FC_BUFFER_DATA_SIZE / 8U));

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/**************************************************************************
 *  test_process_read_ecc_max — 单 CW ECC 计数与命令级聚合
 *
 *  背景：`fc_data_path_process_read` 是 `desc->ecc_bits` 的唯一生产者（ECC 解码
 *  成功后写实际纠正位数），`fc_cmd_ecc_max_get` 在 release 前把链上最大值交给
 *  FTL（读重试 / 风险评估按「本次读纠了多少 bit」决策）。本用例让「恒为 0」与
 *  「真的写了」可区分（C8：跑通 != 生效）。
 *
 *  三段：
 *    ① 干净页：四条 CW 全部解码成功但纠 0 bit ⇒ ecc_bits 全 0、聚合 0。
 *    ② 能力内：CW0 注 1 bit、CW1 注 2 bit ⇒ process_read OK；逐 CW
 *       1/2/0/0，聚合 == 2；USER 已解扰回 PAA 明文（证明 OK 不是「没解码直接放过」）。
 *    ③ 不可纠：CW0 整段破坏（必 UNC）、CW2 注 1 bit ⇒ process_read IO_ERROR；
 *       成功解的 CW2 仍记 1，失败 CW0 不写（保持出池时的 0），聚合 == 1 —— 钉住
 *       「聚合只看成功解码的 CW」。
 *
 *  ⚠ ③ 用整段破坏而不是 5 bit：BCH 可能误纠 5 bit（ecc_step 的 beyond-5 只能按
 *    BOUND 判据），结果不确定；本用例要求 UNC **必然发生**（C7）。
 **************************************************************************/

static MunitResult test_process_read_ecc_max(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    const nand_addr_s addr = l2_addr_draw("process_read_ecc_max", 0U);

    /* ① 基线：干净页纠 0 bit */
    {
        fc_cmd_s *cmd = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

        test_fill_user_paa(cmd);
        munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);
        munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

        for (BYTE i = 0U; i < test_cw_count(cmd); i++)
        {
            munit_assert_uint32(test_cw_desc(cmd, i)->ecc_bits, ==, 0U);
        }
        munit_assert_uint32(fc_cmd_ecc_max_get(cmd->cmd_id), ==, 0U);

        fc_command_release(cmd->cmd_id);
    }

    /* ② 能力内：CW0 1 bit、CW1 2 bit */
    {
        fc_cmd_s *cmd = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

        test_fill_user_paa(cmd);
        munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

        l2_ecc_flip(test_cw_base(cmd, 0U), L2_ECC_USER_LO, L2_ECC_DATA_BITS, 1U);
        l2_ecc_flip(test_cw_base(cmd, 1U), L2_ECC_USER_LO, L2_ECC_DATA_BITS, 2U);

        munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

        munit_assert_uint32(test_cw_desc(cmd, 0U)->ecc_bits, ==, 1U);
        munit_assert_uint32(test_cw_desc(cmd, 1U)->ecc_bits, ==, 2U);
        munit_assert_uint32(test_cw_desc(cmd, 2U)->ecc_bits, ==, 0U);
        munit_assert_uint32(test_cw_desc(cmd, 3U)->ecc_bits, ==, 0U);
        munit_assert_uint32(fc_cmd_ecc_max_get(cmd->cmd_id), ==, 2U);

        /* 纠错后 USER 回到 PAA 明文：OK 的前提是这页真的被解码并解扰过。 */
        munit_assert_uint8(test_check_paa(test_cw_user(cmd, 0U), test_cw_desc(cmd, 0U)->paa), ==, 1U);
        munit_assert_uint8(test_check_paa(test_cw_user(cmd, 1U), test_cw_desc(cmd, 1U)->paa), ==, 1U);

        test_trace_summary("ecc_max 能力内: cw0=1 cw1=2 -> max=%hhu", fc_cmd_ecc_max_get(cmd->cmd_id));

        fc_command_release(cmd->cmd_id);
    }

    /* ③ 不可纠 CW0 + 可纠 CW2：聚合只算成功的 CW */
    {
        fc_cmd_s *cmd  = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_ENABLE, FC_CRC_ENABLE);
        BYTE     *base = test_cw_base(cmd, 0U);
        BYTE      max;

        test_fill_user_paa(cmd);
        munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

        for (WORD j = 0U; j < FC_BUFFER_DATA_SIZE; j += 8U)
        {
            base[j] ^= 0xFFU;
        }
        l2_ecc_flip(test_cw_base(cmd, 2U), L2_ECC_USER_LO, L2_ECC_DATA_BITS, 1U);

        munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_IO_ERROR);

        munit_assert_uint32(test_cw_desc(cmd, 0U)->status, ==, FC_CW_STATUS_UNC);
        munit_assert_uint32(test_cw_desc(cmd, 0U)->ecc_bits, ==, 0U);
        munit_assert_uint32(test_cw_desc(cmd, 2U)->ecc_bits, ==, 1U);

        max = fc_cmd_ecc_max_get(cmd->cmd_id);
        munit_assert_uint8(max, ==, 1U);

        test_trace_summary("ecc_max 不可纠: cw0=UNC cw2=1 -> max=%hhu", max);

        fc_command_release(cmd->cmd_id);
    }

    return MUNIT_OK;
}

/** 非连续掩码 + 数据路径：链位 0,1 ↔ cw 0,2。
 * 专钉 Pass 4 用的是 paa_to_cw(desc->paa) 而非链走位——若改用链走位，
 * 第 2 条 desc 会拿 cw=1 去算期望 META，假报 META_ERROR。
 * ⚠ 必须用**单 plane 段**（multi_plane=0，addr.plane=0）：掩码只覆盖 plane 0 时，
 *   multi-plane 写路径会给未覆盖的 plane 1 补一条同行回退地址、静默编一页全 0xFF。
 *   这条约束由 test_cmd_build 的「multi-plane 必须逐 plane 覆盖」断言强制；
 *   fc_request_data_planes_build 也守同一不变式（debug ASSERT + release 拒命令），
 *   生产侧同样受守卫。 */
static MunitResult test_process_read_masked_chain(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    /* 0x05 = plane0 的 CW0 | CW2 */
    const nand_addr_s addr = l2_addr_draw("process_read_masked_chain", 0U);
    fc_cmd_s         *cmd  = l2_cmd(&addr, 0x05U, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    munit_assert_uint8(test_cw_count(cmd), ==, 2U);

    /* 链几何：plane 0 的 CW0 与 CW2（链位 0,1 与 CW 号 0,2 不同） */
    {
        BYTE plane_out[CW_TOTAL_MAX];
        BYTE cw_out[CW_TOTAL_MAX];

        munit_assert_uint32(test_collect_chain(cmd, plane_out, cw_out), ==, 2U);
        munit_assert_uint32(plane_out[0], ==, 0U);
        munit_assert_uint32(cw_out[0], ==, 0U);
        munit_assert_uint32(plane_out[1], ==, 0U);
        munit_assert_uint32(cw_out[1], ==, 2U);
    }

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);
    munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

    l2_verify_all_ready(cmd);
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    /* 证据（--log-visible info）：掩码 → 链位 ↔ CW 号的映射。这条用例钉的就是
     * 「链位 0,1 ↔ cw 0,2 不同」——打出来才看得出 Pass 4 用的是 paa_to_cw 而非链走位。 */
    test_trace_summary("masked_chain bm=0x05: 链位0->cw0, 链位1->cw2（不同）, 往返一致");

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/** Pass 4（META 校验）触发：页内 META 是 LBA=A，本命令却声明 LBA=B。
 * 这是"读到了别的页"的真实成因——数据本身仍能正确解出（种子取自页内明文 META），
 * 但本命令声明的 LBA 与页内 META 不符 ⇒ 每个 CW 置 META_ERROR。返回值仍为 OK。 */
static MunitResult test_process_read_meta_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    const nand_addr_s addr = l2_addr_draw("process_read_meta_error", 0U);
    fc_cmd_s         *cmd  = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

    /* 页内 META 此刻是「命令声明的页」| plane_cw（encode Stage 0 写的）。把**命令声明的页**
     * 换成**同 page / 同 plane、block 环移一格**的那一页：低 3 位（plane/cw）不变 ⇒ 自洽
     * 校验①仍过，命中的是第二项校验②——它比的是 command->meta（fc_data_path.c），
     * 不是 desc->meta。
     * ⚠ 篡改点必须落在 PLANE_CW_MASK（0x7）**之上**：lba_to_meta() 会掩掉低 3 位再
     *   OR 回真实 plane/cw，动低 3 位等于什么都没改（`+1 页` 就是 `+4`，正好落在
     *   bit 2 的 plane 位上，被整段擦除，META_ERROR 恒不置位）。PBA 里 block 是**最高**
     *   字段，改它不动低 3 位——正是本用例要的「低位同、高位异」。环移保证 block 必变。 */
    {
        nand_addr_s tampered = addr;

        tampered.block = ((addr.block + 1U) % BLOCKS_PER_PLANE);
        cmd->meta      = pba_from_addr(&tampered);
    }

    munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status & FC_CW_STATUS_META_ERROR, !=, 0U);
    }

    /* 数据本身没错（种子从页内 META 自举），错的只是"这一页是谁" */
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    /* 证据（--log-visible info）：**声明的页 PBA** 与**页内 META** 各自的值。
     * ⚠ 篡改点一旦落进 PLANE_CW_MASK 就会被整段擦除，断言退化成恒真 ——
     *   把两个数打出来，正是为了让"篡改真的落在了掩码之上"一眼可查。 */
    test_trace_summary("meta_error: 声明 PBA=0x%08lX vs 页内 META=0x%08lX（低 3 位同、高位异）-> 逐 CW META_ERROR, 数据仍一致", cmd->meta, test_cw_desc(cmd, 0U)->meta);

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/** Pass 3（CRC 校验）触发：写时关 CRC（[4,8) 留下哨兵，且该哨兵被 ECC 保护、原样留在
 * buffer 里），读时开 CRC → stored 哨兵 ≠ 重算值 → 每个 CW 置 CRC_ERROR。返回值仍为 OK。
 * ⚠ config 是 process_read 的输入，可在编解码之间改：写与读共用一条命令，
 *   中间翻 CRC 开关即可，不需要另发一条读命令。 */
static MunitResult test_process_read_crc_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    static const DWORD sentinel = 0xDEADBEEFU;

    test_trace_begin();

    const nand_addr_s addr = l2_addr_draw("process_read_crc_error", 0U);
    fc_cmd_s         *cmd  = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

    test_fill_user_paa(cmd);
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        memcpy(test_cw_base(cmd, i) + FC_CRC_META_SIZE, &sentinel, FC_CRC_FIELD_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

    /* 读侧打开 CRC → stored 哨兵与重算值必不相等 */
    cmd->config.crc = FC_CRC_ENABLE;

    munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status & FC_CW_STATUS_CRC_ERROR, !=, 0U);
    }

    /* 证据（--log-visible info）：写时关 CRC 留下的哨兵值 vs 读时重算值（必然不等）。
     * 两个数打出来才说明 CRC_ERROR 是"真的比出来了"，而不是碰巧置位。 */
    test_trace_summary("crc_error: 写时关 CRC 留哨兵 0x%08lX, 读时开 CRC -> 逐 CW CRC_ERROR", sentinel);

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/** LDPC_RAW 分支：encode 跳过 ECC 编码（parity 区不写）、
 * process_read Pass 1 跳过解码直接标 READY → 数据原样往返。 */
static MunitResult test_process_read_ldpc_raw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    const nand_addr_s addr = l2_addr_draw("process_read_ldpc_raw", 0U);
    fc_cmd_s         *cmd  = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    cmd->config.ldpc = FC_ECC_MODE_LDPC_RAW;
    test_fill_user_paa(cmd);

    /* 哨兵：RAW 跳过 ECC 编码 ⇒ parity 区应原样保留 */
    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        memset(test_cw_base(cmd, i) + FC_BUFFER_DATA_SIZE, 0xAA, FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

    for (BYTE i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_check_pattern(test_cw_base(cmd, i) + FC_BUFFER_DATA_SIZE, (FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE), 0xAAU), ==, 1U);
    }

    munit_assert_int(fc_data_path_process_read(cmd->cmd_id), ==, FC_STATUS_OK);

    l2_verify_all_ready(cmd);
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    /* 证据（--log-visible info）：RAW 模式跳过 ECC 编解码，parity 区应原样保留 0xAA 哨兵。 */
    test_trace_summary("ldpc_raw: parity 区原样保留 0xAA（%u B/CW x %hhu CW）, 数据原样往返", (FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE), test_cw_count(cmd));

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/*************************************************************************
 *  fc_data_path_raid_recover 测试
 *
 *  验证管线：RAID XOR 重建 → ECC 重试 → 解扰所有 CW
 *
 *  ⚠ 本层构造不出真 stripe：fc_data_path_raid_recover 只收集**单条命令**的 desc 链
 *    （fc_data_path.c），而 parity CW 在另一条命令的链上（fc.c）；
 *    跨命令适配是 include/fc/fc_raid_engine.h 的未办事项。故这里用「4 个数据 CW
 *    的退化 stripe」，靠 pattern 异或为 0 还原被擦 CW。真 stripe 覆盖见
 *    test_generate_stripe_parity。
 *
 *  参数组合：random=OFF（无加扰，退化 stripe 成立：4 条 CW 异或恰好为 0——
 *            META_i = (base | i) 异或为 0、CRC 线性、ECC 线性、USER 明文异或为 0）。
 *            random=ON 不成立——各 CW keystream 不同，4 条编码后 CW 的异或不为 0，
 *            退化 stripe 的前提就破了。（被擦 CW 的种子本身可从 XOR 还原的明文
 *            META 取回，但那不改变前提不成立这件事。）
 *
 *  编译开关：CONFIG_FC_RAID
 **************************************************************************/

#if CONFIG_FC_RAID

/** 退化 stripe 的恢复流程（random=OFF 才成立，理由见上面的节头）：
 *   ① 4 个 CW 的 USER 区各填**不同** pattern —— 相同 pattern 下"XOR 还原"与"根本没动"
 *      分不开
 *   ② encode —— META / CRC / ECC 都写齐（RAID XOR **不在** encode 里做，fc_data_path.c）
 *   ③ 把 error_idx 那条 CW 的**整 528B** 翻成 0xFF —— 远超 BCH 能力 ⇒ 必然不可纠，
 *      保证走的是"XOR 重建"而不是"ECC 顺手纠回来"（后者会让用例假过）
 *   ④ fc_data_path_raid_recover(cmd->cmd_id, error_idx)：XOR 重建 → ECC 重试 → 解扰
 *   ⑤ 断言**全部** CW 的 USER 区都回到各自 pattern —— 不只被救的那条：
 *      "救回来了"与"没把别的 CW 弄坏"要一起成立
 *
 * 两个用例共用本函数，只差 random_mode / error_idx。 */
static void do_raid_recover_test(const nand_addr_s *addr, BYTE random_mode, BYTE error_idx)
{
    fc_cmd_s *cmd   = l2_cmd(addr, L2_BM_PLANE0_ALL, random_mode, FC_CRC_ENABLE);
    BYTE      count = test_cw_count(cmd);

    munit_assert_uint32(count, >=, 3U);
    munit_assert_uint32(error_idx, <, count);

    /* 不同 CW 填不同 pattern（USER 数据区 512B） */
    for (BYTE i = 0U; i < count; i++)
    {
        memset(test_cw_user(cmd, i), (0x10U + i), FC_BUFFER_USER_SIZE);
    }

    /* encode：META + CRC + 加扰? + ECC（RAID 不在 encode 里做，见 fc_data_path.c） */
    munit_assert_int(fc_data_path_encode(cmd->cmd_id), ==, FC_STATUS_OK);

    /* 破坏指定 CW（全 CW 528B 翻转，使 ECC 不可纠） */
    BYTE *failed_base = test_cw_base(cmd, error_idx);
    for (WORD j = 0U; j < FC_BUFFER_POOL_CW_SIZE; j++)
    {
        failed_base[j] ^= 0xFF;
    }

    /* raid_recover：XOR 重建 + ECC 重试 + 解扰 */
    munit_assert_int(fc_data_path_raid_recover(cmd->cmd_id, error_idx), ==, FC_STATUS_OK);

    /* 验证：全部 CW USER 数据区恢复原始 pattern */
    for (BYTE i = 0U; i < count; i++)
    {
        munit_assert_uint32(test_check_pattern(test_cw_user(cmd, i), FC_BUFFER_USER_SIZE, (0x10U + i)), ==, 1U);
    }

    fc_command_release(cmd->cmd_id);
}

/** 退化 stripe 恢复：random=OFF（唯一成立的档，见节头）配 error_idx = 1（中间那条 CW，
 * 不是链首 / 链尾）。断言在 do_raid_recover_test 里。 */
static MunitResult test_raid_recover_no_random(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    {
        const nand_addr_s addr = l2_addr_draw("raid_recover_no_random", 0U);

        do_raid_recover_test(&addr, FC_RANDOM_DISABLE, 1U);
    }
    return MUNIT_OK;
}

/**************************************************************************
 *  stripe parity 测试 — fc_data_path_generate_stripe_parity()
 *
 *  验证跨命令 stripe XOR parity 的正确性，不依赖 NAND 硬件。
 *  使用两条命令（数据 + parity），各 4 CW，数据命令先走 encode() 加扰/ECC，
 *  然后 generate_stripe_parity() XOR → parity ECC 编码。
 *
 *  验证方式：清空数据命令某 CW → 用 fc_raid_recover 从 parity 重建 →
 *  与原始 pattern 一致。
 **************************************************************************/

static MunitResult test_generate_stripe_parity(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    /* data 与 parity 两条命令**必须同一地址**（下面把 data 的 desc->meta 同步给 parity），
     * 故只抽一次，两处都传同一个 &addr。 */
    const nand_addr_s addr = l2_addr_draw("generate_stripe_parity", 0U);

    /* 分配数据命令（4 CW, random=ON, raid=NORMAL — encode 里不做 XOR） */
    fc_cmd_s *data_cmd = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_ENABLE, FC_CRC_ENABLE);
    BYTE      cw_cnt   = test_cw_count(data_cmd);

    munit_assert_uint32(cw_cnt, ==, 4U);

    /* 不同 CW 填不同 pattern */
    for (BYTE i = 0U; i < cw_cnt; i++)
    {
        memset(test_cw_user(data_cmd, i), (0x20U + i), FC_BUFFER_USER_SIZE);
    }

    /* encode 数据命令：META → CRC → Random → ECC */
    munit_assert_int(fc_data_path_encode(data_cmd->cmd_id), ==, FC_STATUS_OK);

    /* 分配 parity 命令（同 CW 数，random=OFF） */
    fc_cmd_s *parity_cmd = l2_cmd(&addr, L2_BM_PLANE0_ALL, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    munit_assert_uint8(test_cw_count(parity_cmd), ==, cw_cnt);

    /* 同步 parity desc 的 META（模拟 fc_stripe_submit 中的行为） */
    for (BYTE i = 0U; i < cw_cnt; i++)
    {
        test_cw_desc(parity_cmd, i)->meta = test_cw_desc(data_cmd, i)->meta;
    }

    /* 调用 generate_stripe_parity — XOR 数据命令 CW 到 parity buffer + ECC 编码 parity */
    {
        const fc_cmd_id_t data_ids[1U] = {data_cmd->cmd_id}; /* PLANE_NUM=2 → 1 条数据命令 */
        munit_assert_int(fc_data_path_generate_stripe_parity(data_ids, 1U, parity_cmd->cmd_id), ==, FC_STATUS_OK);
    }

    /*
     * 验证：清空数据命令的 CW0 → 用 fc_raid_recover() 从 parity + 其他 CW 重建 →
     * ECC 解码 + 解扰 → 与原始 pattern 一致。
     *
     * buffers[] 数组：[CW0, CW1, CW2, CW3, parity_CW0, parity_CW1, parity_CW2, parity_CW3]
     * XOR 重建时传入所有 CW 和 parity CW，fc_raid_recover 会跳过 error_idx 并 XOR 其余。
     */
    {
        BYTE *buffers[CW_TOTAL_MAX];

        for (BYTE i = 0U; i < cw_cnt; i++)
        {
            buffers[i]          = test_cw_base(data_cmd, i);
            buffers[cw_cnt + i] = test_cw_base(parity_cmd, i);
        }
        BYTE total_count = (cw_cnt + cw_cnt); /* 8 */

        /* 清空数据命令 CW0，模拟丢失 */
        memset(buffers[0U], 0, FC_BUFFER_POOL_CW_SIZE);

        /* RAID XOR 重建 CW0：传入所有 CW（含 parity） */
        {
            BYTE recovered[FC_BUFFER_POOL_CW_SIZE];

            fc_raid_recover(recovered, buffers, total_count, 0U, FC_BUFFER_POOL_CW_SIZE);
            memcpy(buffers[0U], recovered, FC_BUFFER_POOL_CW_SIZE);
        }

        /* ECC 解码 + 解扰后验证数据 CW0~CW3（parity CW 不验证，它们存储 XOR 结果） */
        for (BYTE i = 0U; i < cw_cnt; i++)
        {
            BYTE *base = buffers[i];

            munit_assert_int(fc_ecc_decode(base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            fc_random_transform(base + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, test_cw_seed(data_cmd, i));
            munit_assert_uint32(test_check_pattern(test_cw_user(data_cmd, i), FC_BUFFER_USER_SIZE, (0x20U + i)), ==, 1U);
        }
    }

    fc_command_release(data_cmd->cmd_id);
    fc_command_release(parity_cmd->cmd_id);
    return MUNIT_OK;
}

#endif /* CONFIG_FC_RAID */

/*************************************************************************
 *  flat CW 掩码建链
 *
 *  建链走 FTL 策略层（ftl_cmd_build_descriptors），不碰 NAND：
 *    - 位图按 **flat 绝对编号**（bit n = plane*CW_NUM + cw）逐位建 desc
 *    - 链序 plane-major
 *    - 位图是**字面**的（0 = 空集）——三条用例各自显式给位图
 *
 *  「按掩码建整条链」是策略，属 FTL（FC 只剩单数逐条入口
 *  fc_command_configure_descriptor）。这三条用例仍留在 data_path，
 *  因为验证的是 desc 链的几何（plane/cw 展开），而非 NAND 行为——
 *  且它们都**不提交**（半掩码组合正是「未覆盖 plane 被静默编程」的陷阱形态）。
 *
 *  这里直接调 test_cmd_build（不经 l2_cmd）：段的宽度由 **op 变体**决定，
 *  而"单 plane 段配 plane1 的位图"正是 plane1-only 场景要验的形态。
 *
 *  场景表（参数化，`--param case <名>`）：三条共用「建链 → 收链 → 断言 n → 逐条
 *  断言 (plane, cw)」的骨架，差异全在 **段宽 / addr.plane / 位图** 三个字段 ⇒ 判据 7。
 *  期望链以显式表给出——原 plane1-only / span 的「循环 + 公式」换成逐位比表，
 *  不变量（plane-major、cw 循环）记在场景表的注释里。
 **************************************************************************/

/* 整段访问用的两种 config：multi-plane 变体（段 = [0,2)）/ 单 plane 变体（段 = [plane,+1)）。 */
static const fc_cmd_cfg_s L2_PROG_MULTI = {
    .operation = OP_PROGRAM_PAGE_MULTI_PLANE,
    .crc       = FC_CRC_ENABLE,
    .random    = FC_RANDOM_ENABLE,
    .ldpc      = FC_ECC_MODE_NORMAL,
};

static const fc_cmd_cfg_s L2_PROG_SINGLE = {
    .operation = OP_NORMAL_PROGRAM_PAGE,
    .crc       = FC_CRC_ENABLE,
    .random    = FC_RANDOM_ENABLE,
    .ldpc      = FC_ECC_MODE_NORMAL,
};

/* 期望链（逐 desc 的 plane / cw），与下面 l2_bm_cases 的行一一对应。 */
static const BYTE l2_bm_mixed_plane[] = {0U, 0U, 1U};
static const BYTE l2_bm_mixed_cw[]    = {0U, 3U, 1U};
static const BYTE l2_bm_p1_plane[]    = {1U, 1U, 1U, 1U};
static const BYTE l2_bm_p1_cw[]       = {0U, 1U, 2U, 3U};
static const BYTE l2_bm_span_plane[]  = {0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U};
static const BYTE l2_bm_span_cw[]     = {0U, 1U, 2U, 3U, 0U, 1U, 2U, 3U};

typedef struct L2_BM_CASE_S
{
    const char         *name;
    BYTE                addr_plane; /* 段内 plane（配合 conf 决定段 = [plane, plane+1) 或 [0,2)） */
    DWORD               bitmap;     /* flat 绝对编号位图（字面位图，0 = 空集） */
    const fc_cmd_cfg_s *conf;
    BYTE                n;         /* 期望链上 desc 数 */
    const BYTE         *plane_exp; /* 期望链：逐 desc 的 plane */
    const BYTE         *cw_exp;    /* 期望链：逐 desc 的 cw */
} l2_bm_case_s;

static const l2_bm_case_s l2_bm_cases[] = {
  /* 0x29 = plane0 取 {CW0,CW3} + plane1 取 {CW1} → 3 条 desc（plane-major） */
    {      "flat-mixed", 0U, 0x29U,  &L2_PROG_MULTI, 3U, l2_bm_mixed_plane, l2_bm_mixed_cw},
 /* 0xF0 = 只选 plane 1 的 4 个 CW（绝对编号，不是 0x0F）→ 4 条 desc 全在 plane 1。
  * 段必须是**单 plane 且 addr.plane = 1**：用 multi-plane op 配只覆盖 plane 1 的位图
  * 会被 test_cmd_build 的「逐 plane 覆盖」断言挡下——那正是静默编程未覆盖 plane 的形态。 */
    {     "plane1-only", 1U, 0xF0U, &L2_PROG_SINGLE, 4U,    l2_bm_p1_plane,    l2_bm_p1_cw},
 /* 0xFF = 整段 → 8 条 desc，钉住「段内每个 plane × 每个 CW 都有一条」这条不变量。
  * ⚠ 位图必须显式给（0 是空集，不会展开）；config.cw_count 不参与推导。 */
    {"span-multi-plane", 0U, 0xFFU,  &L2_PROG_MULTI, 8U,  l2_bm_span_plane,  l2_bm_span_cw},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_bm_cases 一一对应。 */
static const char *l2_bm_case_names[] = {
    "flat-mixed",
    "plane1-only",
    "span-multi-plane",
    NULL,
};

_Static_assert((sizeof(l2_bm_case_names) / sizeof(l2_bm_case_names[0]) - 1U) == (sizeof(l2_bm_cases) / sizeof(l2_bm_cases[0])), "l2_bm_case_names 与 l2_bm_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_bm_params[] = {
    {"case", l2_bm_case_names},
    {  NULL,             NULL},
};

/** 类型化入口，查行逻辑见 l2_case_find。 */
static const l2_bm_case_s *l2_bm_case_find(const char *name)
{
    return (const l2_bm_case_s *)l2_case_find(name, l2_bm_case_names, l2_bm_cases, sizeof(l2_bm_cases[0]));
}

/* ── 用例体：位图 → 实际 desc 链几何（建链层，不碰数据路径）──
 *
 * 用 test_cmd_build 直接建链（**不走 l2_cmd**——本用例要的正是 multi-plane 段），
 * 再按**链序**把每条 desc 的 (plane, cw) 收出来，与场景表的期望数组逐条比。
 *
 * 钉三件事：
 *   ① 位图是 **flat 绝对编号**（bit n = plane*CW_NUM + cw）。"plane1-only = 0xF0" 这一行
 *      钉的就是"plane 1 的 CW0 是 bit 4 而不是 bit 0"——写成 0x0F 会越段。
 *   ② 链序是 **plane-major**（同 plane 内 cw 升序），与场景表数组一一对应。
 *   ③ `config.cw_count` **不回写**：它连建链都不参与，是纯残留字段 ⇒ 三条场景同判据 0。
 *
 * ⚠ 位图必须**显式**给（0 是空集，不会展开）；multi-plane 行的位图必须**逐 plane
 *   覆盖**，否则会被 test_cmd_build 的断言挡下——那正是"静默编程未覆盖 plane"的形态。
 *
 * 证据打印分两档（见 test_trace_summary / test_trace_detail）：摘要打位图 → desc 条数，
 * detail 逐条打 (plane, cw)——flat 编号下 plane 1 的 CW0 是 bit 4 这件事，打出来才看得见。 */
static MunitResult test_cw_bitmap(const MunitParameter params[], void *data)
{
    const l2_bm_case_s *c;
    fc_cmd_s           *cmd;
    BYTE                plane_out[CW_TOTAL_MAX];
    BYTE                cw_out[CW_TOTAL_MAX];
    BYTE                n;
    BYTE                i;

    (void)data;

    test_trace_begin();

    c = l2_bm_case_find(munit_parameters_get(params, "case"));
    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */

    /* plane 由场景表钉死（那正是本条用例的判据，不能随机）；block / page 抽样。 */
    {
        const nand_addr_s addr = l2_addr_draw("cw_bitmap", c->addr_plane);

        cmd = test_cmd_build(&addr, c->bitmap, pba_from_addr(&addr), c->conf);
    }

    /* config.cw_count 不回写（它连建链都不参与，纯残留字段）——三条场景同判据。 */
    munit_assert_uint32(cmd->config.cw_count, ==, 0U);

    n = test_collect_chain(cmd, plane_out, cw_out);
    munit_assert_uint32(n, ==, c->n);

    for (i = 0U; i < n; i++)
    {
        munit_assert_uint32(plane_out[i], ==, c->plane_exp[i]);
        munit_assert_uint32(cw_out[i], ==, c->cw_exp[i]);
    }

    /* 证据（--log-visible info）：位图 → 实际链几何。位图是 **flat 绝对编号**
     * （bit n = plane*CW_NUM + cw）⇒ 打出来才看得出 plane 1 的 CW0 是 bit 4 而不是 bit 0。 */
    test_trace_summary("cw_bitmap case=%s bitmap=0x%02lX addr_plane=%hhu -> %hhu desc:", c->name, c->bitmap, c->addr_plane, n);
    for (i = 0U; i < n; i++)
    {
        test_trace_detail("  desc[%hhu] plane=%hhu cw=%hhu", i, plane_out[i], cw_out[i]);
    }

    fc_command_release(cmd->cmd_id);
    return MUNIT_OK;
}

/*************************************************************************
 *  套件定义
 *
 *  所有 data_path 测试共享 test_setup / test_tear_down（共享夹具模块
 *  fc_test_common：fc_init 一次性 + 开 NAND 放行闩锁 + 分配/释放源数据表）
 **************************************************************************/

MunitTest g_fc_data_path_tests[] = {
  /* ── 步骤级（单 CW 原语）── */
    {          "dp_crc_step_detect",           test_crc_step_detect, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l2_crc_params},
    {          "dp_scramble_whiten",           test_scramble_whiten, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,          NULL},
    {                 "dp_ecc_step",                  test_ecc_step, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l2_ecc_params},

 /* ── encode ── */
    {                   "dp_encode",                    test_encode, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l2_enc_params},
    {      "dp_encode_crc_disabled",       test_encode_crc_disabled, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── 介质格式 KAT（golden vector）── */
    {           "dp_bch_parity_kat",            test_bch_parity_kat, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {             "dp_scramble_kat",              test_scramble_kat, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {                  "dp_crc_kat",                   test_crc_kat, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── process_read ── */
    {          "dp_process_read_ok",           test_process_read_ok, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,  l2_rt_params},
    {         "dp_process_read_unc",          test_process_read_unc, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {     "dp_process_read_ecc_max",      test_process_read_ecc_max, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,          NULL},
    {"dp_process_read_masked_chain", test_process_read_masked_chain, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {  "dp_process_read_meta_error",   test_process_read_meta_error, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {   "dp_process_read_crc_error",    test_process_read_crc_error, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {    "dp_process_read_ldpc_raw",     test_process_read_ldpc_raw, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── raid_recover ── */
#if CONFIG_FC_RAID
    {   "dp_raid_recover_no_random",    test_raid_recover_no_random, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── stripe parity ── */
    {   "dp_generate_stripe_parity",    test_generate_stripe_parity, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
#endif  /* CONFIG_FC_RAID */

  /* ── flat CW 掩码建链 ── */
    {                "dp_cw_bitmap",                 test_cw_bitmap, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,  l2_bm_params},

    {                          NULL,                           NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE,          NULL},
};
