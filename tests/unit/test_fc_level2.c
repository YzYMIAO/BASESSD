/*
 * test_fc_level2.c — Level 2 数据路径测试：fc_data_path 模块
 *
 * 测试三个数据路径管线函数：
 *   fc_data_path_encode       写路径：META → CRC? → Random 加扰? → ECC 编码
 *   fc_data_path_process_read 读路径 Stage 1：ECC 解码 → 条件解扰 → CRC 校验 → META 校验
 *   fc_data_path_raid_recover 读路径 Stage 2：RAID XOR 重建 → ECC 重试 → 解扰
 *
 * ── 全部用例都是 buffer 级：建链后**不提交** ──
 *   本层只验数据路径本身（编码产物、status 位、链几何），**不碰 NAND**。
 *   fc_poll_dispatch_single() 对 page write 调 fc_data_path_encode，
 *   fc_complete_from_nand()   对 page read 调 fc_data_path_process_read——
 *   所以**提交出去的命令绝不能再手工调这两个函数**（会重复编码 / 重复解扰）。
 *   本层一律走 test_cmd_build() 拿一条未提交的命令，自己驱动管线。
 *   NAND 上的端到端往返由 Level 3 的页 helper 覆盖（test_prog_page / test_read_page）。
 *
 * ── 产物自证（v22 教训）──
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
 *   fc_test_common（Level 2 / Level 3 共用一份，见 tests/unit/fc_test_common.h）。
 *   源数据约定：每个 CW 的 USER 区 = **该 CW 自己的 PAA 重复 128 次**
 *   （test_fill_user_paa 填槽 + 拷进 desc）。故「读回来逐 dword 与期望 PAA 比」
 *   同时验了 block / plane / page / cw 四维。
 *
 * ── 配置形态 ──
 *   random     : FC_RANDOM_ENABLE(=0) / FC_RANDOM_DISABLE
 *   crc        : FC_CRC_ENABLE(=0) / FC_CRC_DISABLE
 *   ldpc       : FC_ECC_MODE_BCH（本层默认，走真实编解码）/ FC_ECC_MODE_LDPC_RAW
 *   cw_bitmap  : flat 绝对编号位图（bit n = plane*CW_NUM + cw）。
 *                v27 起是**字面位图**：置 1 = 要访问该 CW，**0 = 空集**（v23~v26 的
 *                「0 = 本命令 plane 段内全部 CW」重载已取消）⇒ 整段访问必须显式写出来。
 *   raid       : 形参保留但**数据路径不读**——config.raid 全仓唯一读者是 fc.c 的
 *                分派钩子（决定走不走 fc_stripe_submit）
 *
 * ⚠ 加扰种子（v24）= 本 CW 的 **META（扇区 LBA）**，由 command->meta 的高位 + 本 desc
 *   的 plane/cw 造出。若 meta == 0，4 条 CW 的种子只由 cw(0..3) 派生，仍互异但都来自
 *   小 LBA——故 fixture 显式给一个非零 LBA，让「种子随 LBA 变」这条性质真的被测到。
 *
 * 释放：一律 fc_command_release() 收尾——v16 起它沿 desc 链把 desc + buffer
 *       一起还池；不要再手动 fc_buffer_release（会二次释放、空闲链成环）。
 */

#include "fc/fc.h"
#include "fc/fc_command.h"
#include "fc/fc_crc_engine.h"
#include "fc/fc_data_path.h"
#include "fc/fc_ldpc_engine.h"
#include "fc/fc_random_engine.h"
#include "fc_test_common.h"
#include "munit.h"
#include "nand/nand_config.h"
#include "nand/onfi_1_0.h"

#if CONFIG_FC_RAID
#    include "fc/fc_raid_engine.h"
#endif

#include <string.h>

/* 命令的扇区 LBA（= 页 LBA TEST_PAGE << CW_WIDTH）。非零——它是加扰种子的来源，
 * 让「种子随 LBA 变」可测（见文件头）。 */
#define L2_META (DWORD)((DWORD)TEST_PAGE << CW_WIDTH)

/* 本层多数用例只要 plane 0 的整段（4 个 CW）。v27 起位图是**字面**的，
 * 0 是空集（ftl_cmd_resolve_cw_bitmap 的断言会当场炸），故必须显式写出来。 */
#define L2_BM_PLANE0_ALL (0x0FU)

/* 步骤级用例的 PAA（单 CW 原语没有 desc->paa）。取 plane 0 / cw 0 / 本层默认块页，
 * 非零——USER 区填出来才不是一片 0（"根本没写"与"写了 0"就分得开）。
 * 只用于 fc_data_path_meta_check 的自洽校验（低 3 位 = plane/cw = 0）。 */
#define L2_PAA (paa_from_addr(&(nand_addr_t){.block = TEST_BLOCK, .plane = 0U, .page = TEST_PAGE}))

/**************************************************************************
 *  本层专用的小封装
 **************************************************************************/

/* 建一条 **plane 0 单 plane、TEST_PAGE 页、BCH 编解码** 的命令（未提交）。
 * operation 固定 OP_NORMAL_PROGRAM_PAGE：数据路径（encode / process_read）**不读**
 * config.operation（fc_data_path.c 全文无该字段的读者），本层驱动的就是"一个已编好码
 * 的页"这个状态。需要 multi-plane 段的几何用例直接调 test_cmd_build（见文末）。
 * 掩码与 meta 由调用者给。 */
static fc_command_t *l2_cmd(uint32_t cw_bitmap, uint32_t meta, uint8_t random_mode, uint8_t crc_mode)
{
    fc_cmd_conf_t config = {0};

    config.operation = (DWORD)OP_NORMAL_PROGRAM_PAGE;
    config.random    = random_mode;
    config.crc       = crc_mode;
    config.ldpc      = FC_ECC_MODE_BCH;

    return test_cmd_build(&(nand_addr_t){.block = TEST_BLOCK, .plane = 0U, .page = TEST_PAGE}, cw_bitmap, meta, &config);
}

/* 要求每个 CW 的 status **恰好**是 READY（一个错误位都没有）。
 * 共享夹具的 test_verify_cw_status_all_success 只看 SUCCESS 位，而
 * FC_CW_STATUS_SUCCESS == FC_CW_STATUS_READY，CRC_ERROR / MATA_ERROR 会与它共存
 * 而不被发现；本层的正向用例要的是"干净读回"，故单独加这条更严的判据。 */
static void l2_verify_all_ready(const fc_command_t *cmd)
{
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status, ==, FC_CW_STATUS_READY);
    }
}

/* 逐 desc 与源表比对，返回是否**完全一致**（不 assert——供「应当不同」的用例取反）。 */
static uint8_t l2_user_matches_source(const fc_command_t *cmd)
{
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        const fc_descriptor_t *desc = test_cw_desc(cmd, i);
        const uint8_t         *buf  = test_cw_user(cmd, i);
        const uint8_t         *expect =
            test_source_slot((uint8_t)paa_to_plane(desc->paa), (uint8_t)paa_to_cw(desc->paa));

        for (uint16_t j = 0U; j < FC_BUFFER_USER_SIZE; j++)
        {
            if (buf[j] != expect[j])
            {
                return 0U;
            }
        }
    }
    return 1U;
}

/* ── 编码产物自证（见文件头「产物自证」） ── */

static void l2_verify_meta_written(const fc_command_t *cmd)
{
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        uint32_t stored = 0U;

        (void)memcpy(&stored, test_cw_base(cmd, i), FC_CRC_META_SIZE);
        munit_assert_uint32(stored, ==, test_cw_desc(cmd, i)->meta);
    }
}

/* ⚠ 必须拿**源槽**（明文）算 CRC，不能拿 desc 的 USER 区——random=ON 时那里已被加扰，
 *   而 CRC 是对加扰**之前**的明文算的（fc_crc_engine.h 头注）。 */
static void l2_verify_crc_written(const fc_command_t *cmd)
{
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        const fc_descriptor_t *desc   = test_cw_desc(cmd, i);
        const uint8_t         *plain  = test_source_slot((uint8_t)paa_to_plane(desc->paa), (uint8_t)paa_to_cw(desc->paa));
        uint32_t               stored = 0U;

        (void)memcpy(&stored, test_cw_base(cmd, i) + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE);
        munit_assert_uint32(stored, ==, fc_crc_compute(plain));
    }
}

static void l2_verify_parity_nonzero(const fc_command_t *cmd)
{
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        const uint8_t *base = test_cw_base(cmd, i);

        /* test_check_pattern 返回 1 表示"整段都是该 pattern"⇒ 断言 == 0 即"非全 0" */
        munit_assert_uint8(test_check_pattern(base + FC_BUFFER_DATA_SIZE,
                                              (uint16_t)(FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE),
                                              0x00U),
                           ==,
                           0U);
    }
}

/* buffer 级往返：建链 → 填明文 → encode → process_read → 校验。
 * **不提交**——FC 提交时会自己再跑一遍 encode / process_read，重复编码会让用例空转。 */
static void l2_buffer_round_trip(uint32_t cw_bitmap, uint8_t random_mode, uint8_t crc_mode)
{
    fc_command_t *cmd = l2_cmd(cw_bitmap, L2_META, random_mode, crc_mode);

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    l2_verify_all_ready(cmd);
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
}

/**************************************************************************
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
 * fc_crc_compute(cw + 8) 只吃 **FC_CRC_USER_SIZE = 512** 字节（fc_crc_engine.c:125），
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
typedef struct
{
    const char *name;
    uint32_t    lo; /* 注入字节区间 [lo, hi)（CW 内偏移） */
    uint32_t    hi;
    uint8_t     expect_ok; /* 1 = 注入后 CRC 仍通过（该区域不在 CRC 覆盖内） */
} l2_crc_case_t;

static const l2_crc_case_t l2_crc_cases[] = {
    {     "user-1",  FC_CRC_USER_OFFSET, FC_CRC_USER_OFFSET + FC_BUFFER_USER_SIZE, 0U},
    {"crc-field-1",    FC_CRC_META_SIZE,     FC_CRC_META_SIZE + FC_CRC_FIELD_SIZE, 0U},
    {     "meta-1",                  0U,                         FC_CRC_META_SIZE, 1U},
    {   "parity-1", FC_BUFFER_DATA_SIZE,                   FC_BUFFER_POOL_CW_SIZE, 1U},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_crc_cases 一一对应。 */
static const char *l2_crc_case_names[] = {"user-1", "crc-field-1", "meta-1", "parity-1", NULL};

_Static_assert((sizeof(l2_crc_case_names) / sizeof(l2_crc_case_names[0]) - 1U) == (sizeof(l2_crc_cases) / sizeof(l2_crc_cases[0])),
               "l2_crc_case_names 与 l2_crc_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h:432）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_crc_params[] = {
    {"case", l2_crc_case_names},
    {  NULL,              NULL},
};

static const l2_crc_case_t *l2_crc_case_find(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0U; i < (sizeof(l2_crc_cases) / sizeof(l2_crc_cases[0])); i++)
    {
        if (strcmp(l2_crc_cases[i].name, name) == 0)
        {
            return &l2_crc_cases[i];
        }
    }

    return NULL;
}

static MunitResult test_crc_step_detect(const MunitParameter params[], void *data)
{
    const l2_crc_case_t *c  = l2_crc_case_find(munit_parameters_get(params, "case"));
    uint8_t             *cw = test_cw_scratch(0U);
    uint32_t             off;
    uint32_t             bit;

    (void)data;

    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */

    /* 残留哨兵：未写到的字节应能看见 0xCC，避免"其实没写"被当成通过 */
    (void)memset(cw, 0xCC, FC_BUFFER_POOL_CW_SIZE);

    fc_data_path_meta_write(cw, L2_META);
    test_fill_paa(cw + FC_CRC_USER_OFFSET, L2_PAA);
    fc_data_path_crc_write(cw);

    /* 先自证：未注入时应通过（否则下面的"检出"没有意义） */
    munit_assert_uint8(fc_data_path_crc_check(cw), ==, 1U);

    off = (uint32_t)munit_rand_int_range((int)c->lo, (int)c->hi - 1);
    bit = (uint32_t)munit_rand_int_range(0, 7);
    munit_logf(MUNIT_LOG_INFO, "case=%s inject cw[%u] bit %u", c->name, (unsigned)off, (unsigned)bit);

    /* 步骤级没有原地解码，直接翻回来即可——不依赖任何"解码不改 buffer"假设 */
    cw[off] ^= (uint8_t)(1U << bit);
    munit_assert_uint8(fc_data_path_crc_check(cw), ==, c->expect_ok);

    cw[off] ^= (uint8_t)(1U << bit);
    munit_assert_uint8(fc_data_path_crc_check(cw), ==, 1U); /* 还原 ⇒ 必通过 */

    return MUNIT_OK;
}

/* ── 0/1 游程统计（加扰用例用） ──
 *
 * 位序：每字节 **MSB 先**。加扰引擎的 fc_lfsr_byte 就是把先出的位放在 MSB，
 * 按这个顺序读出来的 bit 串就是 LFSR 的输出序列本身（见 fc_random_engine.c）。
 * 字节内反序会得到另一套游程分布，两处必须用同一约定。 */

typedef struct
{
    uint32_t hist[2][16]; /* hist[b][k] = 长度 k+1 的游程个数；k=15 表示 >=16（钳位） */
    uint32_t ones;        /* 1 的总个数 */
    uint32_t runs;        /* 游程总数（= 两个 hist 之和） */
} l2_bit01_t;

/* 游程长度 → 桶号。>=16 全部钳进第 15 桶——**不能丢弃**：全 0xFF 的明文就一个
 * 长度 4096 的游程、PAA 重复的明文有 128 个 >=16 的 0 游程，正是加扰要治的病。 */
static uint8_t l2_run_bucket(uint32_t run)
{
    return (run >= 16U) ? 15U : (uint8_t)(run - 1U);
}

static void l2_bit01_analyze(const uint8_t *data, uint16_t len, l2_bit01_t *st)
{
    uint16_t i;
    uint8_t  k;
    uint8_t  cur;
    uint32_t run;

    (void)memset(st, 0, sizeof(*st));
    if (len == 0U)
    {
        return;
    }

    cur = (uint8_t)((data[0] >> 7U) & 1U);
    run = 0U;

    for (i = 0U; i < len; i++)
    {
        for (k = 0U; k < 8U; k++)
        {
            const uint8_t b = (uint8_t)((data[i] >> (7U - k)) & 1U);

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

/* 平均游程 ×100（整数比较，避开浮点与 %f）。随机数据的理论值是 2.00。 */
static uint32_t l2_bit01_mean_x100(const l2_bit01_t *st, uint16_t len)
{
    return (st->runs == 0U) ? 0U : (((uint32_t)len * 8U * 100U) / st->runs);
}

/* 与"恰好一半"的偏离量（bit 数）。 */
static uint32_t l2_bit01_bias(const l2_bit01_t *st, uint16_t len)
{
    const uint32_t half = (uint32_t)len * 4U;

    return (st->ones > half) ? (st->ones - half) : (half - st->ones);
}

/* 把统计结果打成日志（上板后照这个分布收紧阈值）。只打非空的桶。 */
static void l2_bit01_log(const char *tag, const l2_bit01_t *st, uint16_t len)
{
    uint8_t k;

    munit_logf(MUNIT_LOG_INFO,
               "%s: ones=%u/%u runs=%u mean_x100=%u",
               tag,
               (unsigned)st->ones,
               (unsigned)((uint32_t)len * 8U),
               (unsigned)st->runs,
               (unsigned)l2_bit01_mean_x100(st, len));

    for (k = 0U; k < 16U; k++)
    {
        if ((st->hist[0][k] + st->hist[1][k]) != 0U)
        {
            munit_logf(MUNIT_LOG_INFO,
                       "  run len %2u%s : zero x%u, one x%u",
                       (unsigned)(k + 1U),
                       (k == 15U) ? "+" : " ",
                       (unsigned)st->hist[0][k],
                       (unsigned)st->hist[1][k]);
        }
    }
}

/* 加扰白化：三种偏置输入 → 加扰 → 统计游程 → 解扰 → 自反。
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
    uint8_t                  kind;

    (void)params;
    (void)data;

    for (kind = 0U; kind < 3U; kind++)
    {
        uint8_t *cw            = test_cw_scratch(0U);
        uint8_t *plain         = test_cw_scratch(1U);
        /* 取合法地址：page 取 4 的倍数——meta 的低 3 位要留给 plane/cw，
         * 与建链时 desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw 同构。 */
        const nand_addr_t addr = {
            .lun   = 0U,
            .block = (uint16_t)munit_rand_int_range(0, BLOCKS_PER_PLANE - 1),
            .plane = (uint8_t)munit_rand_int_range(0, PLANE_NUM - 1),
            .page  = (uint8_t)(munit_rand_int_range(0, PAGES_PER_BLOCK - 1) & ~3),
            .cw    = (uint8_t)munit_rand_int_range(0, CW_NUM - 1),
        };
        const nand_paa_t paa  = paa_from_addr(&addr);
        const uint32_t   meta = lba_to_meta((uint32_t)addr.page << CW_WIDTH, addr.plane, addr.cw);
        const uint32_t   seed = fc_random_seed_from_meta(meta);
        l2_bit01_t       before;
        l2_bit01_t       after;

        /* 明文：常量图案，或本 CW 的 PAA 重复（后者是真实写路径的图案） */
        if (kind == 2U)
        {
            test_fill_paa(cw + FC_CRC_USER_OFFSET, paa);
        }
        else
        {
            (void)memset(cw + FC_CRC_USER_OFFSET, (kind == 0U) ? 0x00 : 0xFF, FC_BUFFER_USER_SIZE);
        }
        (void)memcpy(plain, cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE);

        l2_bit01_analyze(plain, FC_BUFFER_USER_SIZE, &before);

        /* 自证：常量图案的明文必须**恰好 1 个游程**——否则是分析函数自己错了 */
        if (kind < 2U)
        {
            munit_assert_uint32(before.runs, ==, 1U);
            munit_assert_uint32(before.ones, ==, (kind == 0U) ? 0U : ((uint32_t)FC_BUFFER_USER_SIZE * 8U));
        }

        fc_data_path_scramble(cw, seed);
        l2_bit01_analyze(cw + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, &after);

        munit_logf(MUNIT_LOG_INFO,
                   "--- %s: block=%u page=%u plane=%u cw=%u meta=0x%08X seed=0x%08X",
                   tag[kind],
                   (unsigned)addr.block,
                   (unsigned)addr.page,
                   (unsigned)addr.plane,
                   (unsigned)addr.cw,
                   (unsigned)meta,
                   (unsigned)seed);
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
            munit_assert_uint32(l2_bit01_bias(&after, FC_BUFFER_USER_SIZE),
                                <,
                                l2_bit01_bias(&before, FC_BUFFER_USER_SIZE));
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
 * ⚠ 框架默认种子是 time() 派生的（munit.c:1395），每次跑都不同 ⇒ 任何"超能力错误
 *   必然不可纠"的断言都会偶发红灯。
 *
 * 本用例**不设** SINGLE_ITERATION ⇒ 可用 `--iterations N` 给每个场景多抽几组注入
 * 位置（默认仍是 1 次：根套件 iterations=1）。 */

#define L2_ECC_DATA_BITS ((uint32_t)FC_BUFFER_DATA_SIZE * 8U) /* 4160：ECC 保护的数据区 */
#define L2_ECC_META_BITS ((uint32_t)FC_CRC_META_SIZE * 8U)    /* 32：META cw[0..4) */
#define L2_ECC_CRC_LO    (L2_ECC_META_BITS)                   /* 32：CRC 字段 cw[4..8) */
#define L2_ECC_CRC_HI    (2U * L2_ECC_META_BITS)              /* 64 */
#define L2_ECC_USER_LO   (L2_ECC_CRC_HI)                      /* 64：USER cw[8..520) */
/* parity 区里**确定被校验**的字节数：8B 槽只写了 7B，且被校验的 52 bit 并不连续
 * （parity 首 6 字节 + 第 7 字节高 4 位；第 7 字节低 4 位与第 8 字节编码器从不写、
 * 解码器也不校验）。这里只取前 6 字节，避开这段布局细节。 */
#define L2_ECC_PARITY_BYTES (6U)
#define L2_ECC_PARITY_LO    (L2_ECC_DATA_BITS)                            /* 4160 */
#define L2_ECC_PARITY_HI    (L2_ECC_PARITY_LO + L2_ECC_PARITY_BYTES * 8U) /* 4208 */

/* 造一块"写路径形态"的 CW：META + USER(PAA 明文) + CRC + ECC。
 * 不含加扰——加扰是正交的一步（见 test_scramble_whiten），ECC 覆盖整个 data[0..520)，
 * 与 USER 区是否已加扰无关。ref 非 NULL 时留一份基准副本（比对用）。 */
static void l2_ecc_cw_build(uint8_t *cw, uint8_t *ref)
{
    (void)memset(cw, 0xCC, FC_BUFFER_POOL_CW_SIZE); /* 残留哨兵：没被写到的字节可见 */
    fc_data_path_meta_write(cw, L2_META);
    test_fill_paa(cw + FC_CRC_USER_OFFSET, L2_PAA);
    fc_data_path_crc_write(cw);
    munit_assert_int(fc_data_path_ecc_encode(cw), ==, FC_STATUS_OK);

    if (ref != NULL)
    {
        (void)memcpy(ref, cw, FC_BUFFER_POOL_CW_SIZE);
    }
}

/* 在 CW 的 bit 区间 [lo, hi) 里随机翻 k 个**互不重复**的 bit，逐个打印位置。
 * k ≤ 8（BCH t=4 的用例最多用到 4；留点余量给后续超能力冒烟）。 */
static void l2_ecc_flip(uint8_t *cw, uint32_t lo, uint32_t hi, uint32_t k)
{
    uint32_t pos[8];
    uint32_t i;
    uint32_t j;

    ASSERT(k <= 8U);
    ASSERT(lo < hi);

    for (i = 0U; i < k; i++)
    {
        uint8_t dup;

        do
        {
            pos[i] = lo + (uint32_t)munit_rand_int_range(0, (int)(hi - lo) - 1);
            dup    = 0U;
            for (j = 0U; j < i; j++)
            {
                if (pos[j] == pos[i])
                {
                    dup = 1U;
                }
            }
        } while (dup != 0U);

        cw[pos[i] / 8U] ^= (uint8_t)(1U << (pos[i] % 8U));
        munit_logf(MUNIT_LOG_INFO, "  flip bit %u (cw[%u] bit %u)", (unsigned)pos[i], (unsigned)(pos[i] / 8U), (unsigned)(pos[i] % 8U));
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
 *            p_k > 0 ⇒ 解码器只翻 errloc < 4160 的位（fc_ldpc_engine.c:179-186），
 *            parity 字节**不回写**，只比数据区 520B。
 *   BOUND —— 超能力：结果不确定，只钉不变式（见用例体）。
 *
 * 覆盖面：干净 / META / CRC 字段 / USER / k=2..t 的整数据区 / parity 单侧 k=1 与
 * k=t / 数据+parity 混合 / 超能力边界。CRC 字段单列是因为它**自身也被 ECC 保护**——
 * 若 ECC 修不好 CRC 字段，Pass 3 的 CRC 校验会跟着把这一页判错。 */
typedef enum
{
    L2_ECC_EXACT,
    L2_ECC_BOUND
} l2_ecc_expect_t;

typedef struct
{
    const char     *name;
    uint32_t        d_lo; /* 数据区注入区间（bit）；d_k == 0 时不参与 */
    uint32_t        d_hi;
    uint32_t        d_k; /* 数据区注入 bit 数 */
    uint32_t        p_k; /* parity 区注入 bit 数 */
    l2_ecc_expect_t expect;
} l2_ecc_case_t;

static const l2_ecc_case_t l2_ecc_cases[] = {
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

_Static_assert((sizeof(l2_ecc_case_names) / sizeof(l2_ecc_case_names[0]) - 1U) == (sizeof(l2_ecc_cases) / sizeof(l2_ecc_cases[0])),
               "l2_ecc_case_names 与 l2_ecc_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h:432）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_ecc_params[] = {
    {"case", l2_ecc_case_names},
    {  NULL,              NULL},
};

static const l2_ecc_case_t *l2_ecc_case_find(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0U; i < (sizeof(l2_ecc_cases) / sizeof(l2_ecc_cases[0])); i++)
    {
        if (strcmp(l2_ecc_cases[i].name, name) == 0)
        {
            return &l2_ecc_cases[i];
        }
    }

    return NULL;
}

/* 一个用例跑遍场景表：每组的 setup/tear_down 由运行器各跑一次，失败互不遮蔽。
 * ecc_bits 预置 0xAA 当哨兵——证明解码确实写了它，而不是"恰好为 0"。 */
static MunitResult test_ecc_step(const MunitParameter params[], void *data)
{
    const l2_ecc_case_t *c        = l2_ecc_case_find(munit_parameters_get(params, "case"));
    uint8_t             *cw       = test_cw_scratch(0U);
    uint8_t             *ref      = test_cw_scratch(1U);
    uint8_t             *inj      = test_cw_scratch(2U); /* 仅 BOUND 用：注入后、解码前的快照 */
    uint8_t              ecc_bits = 0xAAU;
    fc_status_t          status;
    uint32_t             total;

    (void)data;

    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */
    total = c->d_k + c->p_k;

    l2_ecc_cw_build(cw, ref);
    if (c->d_k != 0U)
    {
        l2_ecc_flip(cw, c->d_lo, c->d_hi, c->d_k);
    }
    if (c->p_k != 0U)
    {
        l2_ecc_flip(cw, L2_ECC_PARITY_LO, L2_ECC_PARITY_HI, c->p_k);
    }

    munit_logf(MUNIT_LOG_INFO, "case=%s inject=%u", c->name, (unsigned)total);

    if (c->expect == L2_ECC_BOUND)
    {
        (void)memcpy(inj, cw, FC_BUFFER_POOL_CW_SIZE); /* 失败原子性的基准 = 解码前状态 */
    }

    status = fc_data_path_ecc_decode(cw, &ecc_bits);

    if (c->expect == L2_ECC_BOUND)
    {
        /* 超能力：BCH 允许误纠 ⇒ 只钉两条**恒成立**的不变式，不钉结果。
         * ① 失败必须原子：IO_ERROR 时缓冲一字节不动（对照**注入后**快照 inj，
         *    **不是**干净码字 ref——cw 本来就带 5 个错 bit，跟 ref 比永远不等）、
         *    且纠正数为 0（fc_ldpc_engine.c:168-171 在翻转循环**之前**就 return 了）。
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

    munit_assert_int(status, ==, FC_STATUS_OK);
    munit_assert_uint8(ecc_bits, ==, (uint8_t)total);
    munit_assert_memory_equal((c->p_k == 0U) ? FC_BUFFER_POOL_CW_SIZE : FC_BUFFER_DATA_SIZE, cw, ref);

    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_encode 测试
 *
 *  验证管线（Stage 3 跨命令 stripe RAID 已移出，见 fc_data_path.c:191）：
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

typedef struct
{
    const char *name;
    uint32_t    cw_bitmap;
    uint8_t     random_mode;
    uint8_t     crc_mode;
    uint8_t     n_cw;         /* 期望链上 desc 数 */
    uint8_t     expect_plain; /* 1 = encode 后 USER 区仍等于明文 */
} l2_enc_case_t;

static const l2_enc_case_t l2_enc_cases[] = {
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

_Static_assert((sizeof(l2_enc_case_names) / sizeof(l2_enc_case_names[0]) - 1U) == (sizeof(l2_enc_cases) / sizeof(l2_enc_cases[0])),
               "l2_enc_case_names 与 l2_enc_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h:432）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_enc_params[] = {
    {"case", l2_enc_case_names},
    {  NULL,              NULL},
};

static const l2_enc_case_t *l2_enc_case_find(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0U; i < (sizeof(l2_enc_cases) / sizeof(l2_enc_cases[0])); i++)
    {
        if (strcmp(l2_enc_cases[i].name, name) == 0)
        {
            return &l2_enc_cases[i];
        }
    }

    return NULL;
}

static MunitResult test_encode(const MunitParameter params[], void *data)
{
    const l2_enc_case_t *c;
    fc_command_t        *enc;

    (void)data;

    c = l2_enc_case_find(munit_parameters_get(params, "case"));
    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */

    enc = l2_cmd(c->cw_bitmap, L2_META, c->random_mode, c->crc_mode);
    munit_assert_uint32(test_cw_count(enc), ==, c->n_cw);

    test_fill_user_paa(enc);
    munit_assert_int(fc_data_path_encode(enc), ==, FC_STATUS_OK);

    /* random=ON ⇒ encode 后 USER 区应已不是明文（[0,8) 是 META + CRC，不属于 USER 区）。
     * 这条是自证：fixture 一旦退化（例如 seed 恒 0 使 keystream 整片为 0），
     * 往返恒等式会把问题原样盖过去。 */
    munit_assert_uint8(l2_user_matches_source(enc), ==, c->expect_plain);

    l2_verify_meta_written(enc);
    l2_verify_crc_written(enc); /* CRC 对明文算 ⇒ 与 USER 是否已加扰无关 */
    l2_verify_parity_nonzero(enc);

    munit_assert_int(fc_command_release(enc), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/* CRC 关闭分支：encode 不写 CRC 字段（哨兵原样保留）→ 读侧 Pass 3 整个跳过 → 无 CRC_ERROR */
static MunitResult test_encode_crc_disabled(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *enc = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

    munit_assert_uint32(test_cw_count(enc), ==, CW_NUM);

    test_fill_user_paa(enc);
    for (uint8_t i = 0U; i < test_cw_count(enc); i++)
    {
        /* 哨兵：确认 encode 在 CRC 关闭时确实没碰 [4,8) */
        (void)memset(test_cw_base(enc, i) + FC_CRC_META_SIZE, 0xFF, FC_CRC_FIELD_SIZE);
    }

    munit_assert_int(fc_data_path_encode(enc), ==, FC_STATUS_OK);

    /* META 与 CRC 是两件独立的事：关 CRC 不影响 Stage 0 */
    l2_verify_meta_written(enc);

    for (uint8_t i = 0U; i < test_cw_count(enc); i++)
    {
        munit_assert_uint32(test_check_pattern(test_cw_base(enc, i) + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE, 0xFFU),
                            ==,
                            1U);
    }

    munit_assert_int(fc_command_release(enc), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_process_read 测试
 *
 *  验证管线（Pass 1~4）：
 *     Buffer → ECC 解码 → 全部成功? → 解扰? → CRC 校验? → META 校验
 *                              ↓ 否
 *                        不解扰，返回 IO_ERROR
 *
 *  ⚠ Pass 3/Pass 4 只置 desc status 位、**不改返回值**（fc_data_path.c:551 恒返回 OK），
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
 *     process_read_mata_error  META 不符 → MATA_ERROR
 *     process_read_crc_error   CRC 不符 → CRC_ERROR
 *     process_read_ldpc_raw    LDPC_RAW：跳过 ECC 编码/解码
 **************************************************************************/

/* 三条 OK 往返的入参恰好就是 l2_buffer_round_trip 的三个形参，断言全在 helper 里
 * ⇒ 判据 7「同一断言、不同输入」。这里仍用**参数化**而非用例内循环：失败时 munit
 * 会把 case 名带进报错，且 `--param case single-cw` 能单跑（补 munit_parameters_get
 * 换来的能力）。 */
typedef struct
{
    const char *name;
    uint32_t    cw_bitmap;
    uint8_t     random_mode;
    uint8_t     crc_mode;
} l2_rt_case_t;

static const l2_rt_case_t l2_rt_cases[] = {
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

_Static_assert((sizeof(l2_rt_case_names) / sizeof(l2_rt_case_names[0]) - 1U) == (sizeof(l2_rt_cases) / sizeof(l2_rt_cases[0])),
               "l2_rt_case_names 与 l2_rt_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h:432）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_rt_params[] = {
    {"case", l2_rt_case_names},
    {  NULL,             NULL},
};

static const l2_rt_case_t *l2_rt_case_find(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0U; i < (sizeof(l2_rt_cases) / sizeof(l2_rt_cases[0])); i++)
    {
        if (strcmp(l2_rt_cases[i].name, name) == 0)
        {
            return &l2_rt_cases[i];
        }
    }

    return NULL;
}

static MunitResult test_process_read_ok(const MunitParameter params[], void *data)
{
    const l2_rt_case_t *c = l2_rt_case_find(munit_parameters_get(params, "case"));

    (void)data;

    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */
    l2_buffer_round_trip(c->cw_bitmap, c->random_mode, c->crc_mode);

    return MUNIT_OK;
}

/* UNC：**先破坏、只读一次**。
 * 未破坏的 CW0/2/3 读回 READY 就是"这一页本来是好的"的对照组——无需先跑一遍干净读：
 * process_read 是原地解码的，跑两遍会让第二遍面对已解扰的 buffer（ECC 校验子整体失配），
 * 把对照组一起判成 UNC。
 * ⚠ 破坏必须**远超** BCH 纠错能力，否则会被"纠正"回去、用例假过：
 *   按 8 字节步长翻转整个 520B 数据区（65 处、最多 520 bit）——8B parity 纠不了。 */
static MunitResult test_process_read_unc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏 CW1 */
    {
        uint8_t *base = test_cw_base(cmd, 1U);

        for (uint16_t j = 0U; j < FC_BUFFER_DATA_SIZE; j += 8U)
        {
            base[j] ^= 0xFFU;
        }
    }

    /* process_read 应返回 IO_ERROR（不解扰） */
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_IO_ERROR);

    /* 验证 desc status：CW1 为 UNC，其余为 READY（Pass 1 是**赋值**，错误位不会残留） */
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status, ==, (i == 1U) ? FC_CW_STATUS_UNC : FC_CW_STATUS_READY);
    }

    /* Pass 2 被跳过（"全部成功才解扰"）⇒ 未破坏的 CW0 也仍停在**加扰态**、不是明文。
     * 这条把"出错时不部分解扰"钉住；random=OFF 时它恒不成立，故本用例必须 random=ON。 */
    munit_assert_uint8(test_check_paa(test_cw_user(cmd, 0U), test_cw_desc(cmd, 0U)->paa), ==, 0U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/* 非连续掩码 + 数据路径：链位 0,1 ↔ cw 0,2。
 * 专钉 Pass 4 用的是 paa_to_cw(desc->paa) 而非链走位——若改用链走位，
 * 第 2 条 desc 会拿 cw=1 去算期望 META，假报 MATA_ERROR。
 * ⚠ 必须用**单 plane 段**（multi_plane=0，addr.plane=0）：掩码只覆盖 plane 0 时，
 *   multi-plane 写路径会给未覆盖的 plane 1 补一条同行回退地址、静默编一页全 0xFF。
 *   这条约束已由 test_cmd_build 的「multi-plane 必须逐 plane 覆盖」断言强制。 */
static MunitResult test_process_read_masked_chain(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* 0x05 = plane0 的 CW0 | CW2 */
    fc_command_t *cmd = l2_cmd(0x05U, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    munit_assert_uint32(test_cw_count(cmd), ==, 2U);

    /* 链几何：plane 0 的 CW0 与 CW2（链位 0,1 与 CW 号 0,2 不同） */
    {
        uint8_t plane_out[CW_TOTAL_MAX];
        uint8_t cw_out[CW_TOTAL_MAX];

        munit_assert_uint32(test_collect_chain(cmd, plane_out, cw_out), ==, 2U);
        munit_assert_uint32(plane_out[0], ==, 0U);
        munit_assert_uint32(cw_out[0], ==, 0U);
        munit_assert_uint32(plane_out[1], ==, 0U);
        munit_assert_uint32(cw_out[1], ==, 2U);
    }

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    l2_verify_all_ready(cmd);
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/* Pass 4（META 校验）触发：页内 META 是 LBA=A，本命令却声明 LBA=B。
 * 这是"读到了别的页"的真实成因——数据本身仍能正确解出（种子取自页内明文 META），
 * 但本命令声明的 LBA 与页内 META 不符 ⇒ 每个 CW 置 MATA_ERROR。返回值仍为 OK。 */
static MunitResult test_process_read_mata_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    test_fill_user_paa(cmd);
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 页内 META 此刻是 L2_META | plane_cw（encode Stage 0 写的）。把**命令声明的 LBA**
     * 换成"下一页"：低 3 位（plane/cw）不变 ⇒ 自洽校验①仍过，命中的是第二项校验②
     * ——它比的是 command->meta（fc_data_path.c:562），不是 desc->meta。 */
    cmd->meta = (DWORD)(TEST_PAGE + 1U) << CW_WIDTH;

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status & FC_CW_STATUS_MATA_ERROR, !=, 0U);
    }

    /* 数据本身没错（种子从页内 META 自举），错的只是"这一页是谁" */
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/* Pass 3（CRC 校验）触发：写时关 CRC（[4,8) 留下哨兵，且该哨兵被 ECC 保护、原样留在
 * buffer 里），读时开 CRC → stored 哨兵 ≠ 重算值 → 每个 CW 置 CRC_ERROR。返回值仍为 OK。
 * ⚠ config 是 process_read 的输入，可在编解码之间改——旧版用两条命令（写一条、读一条）
 *   表达同一件事，buffer 级一条命令翻个开关就够。 */
static MunitResult test_process_read_crc_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    static const uint32_t sentinel = 0xDEADBEEFU;

    fc_command_t *cmd = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

    test_fill_user_paa(cmd);
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        (void)memcpy(test_cw_base(cmd, i) + FC_CRC_META_SIZE, &sentinel, FC_CRC_FIELD_SIZE);
    }
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 读侧打开 CRC → stored 哨兵与重算值必不相等 */
    cmd->config.crc = FC_CRC_ENABLE;

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_cw_desc(cmd, i)->status & FC_CW_STATUS_CRC_ERROR, !=, 0U);
    }

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/* LDPC_RAW 分支：encode 跳过 ECC 编码（parity 区不写）、
 * process_read Pass 1 跳过解码直接标 READY → 数据原样往返。 */
static MunitResult test_process_read_ldpc_raw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    cmd->config.ldpc = FC_ECC_MODE_LDPC_RAW;
    test_fill_user_paa(cmd);

    /* 哨兵：RAW 跳过 ECC 编码 ⇒ parity 区应原样保留 */
    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        (void)memset(test_cw_base(cmd, i) + FC_BUFFER_DATA_SIZE, 0xAA, FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE);
    }

    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    for (uint8_t i = 0U; i < test_cw_count(cmd); i++)
    {
        munit_assert_uint32(test_check_pattern(test_cw_base(cmd, i) + FC_BUFFER_DATA_SIZE,
                                               (uint16_t)(FC_BUFFER_POOL_CW_SIZE - FC_BUFFER_DATA_SIZE),
                                               0xAAU),
                            ==,
                            1U);
    }

    munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

    l2_verify_all_ready(cmd);
    munit_assert_uint8(l2_user_matches_source(cmd), ==, 1U);

    munit_assert_int(fc_command_release(cmd), ==, FC_STATUS_OK);
    return MUNIT_OK;
}

/**************************************************************************
 *  fc_data_path_raid_recover 测试
 *
 *  验证管线：RAID XOR 重建 → ECC 重试 → 解扰所有 CW
 *
 *  ⚠ 本层构造不出真 stripe：fc_data_path_raid_recover 只收集**单条命令**的 desc 链
 *    （fc_data_path.c:591），而 parity CW 在另一条命令的链上（fc.c:888/924-933）；
 *    跨命令适配是 include/fc/fc_raid_engine.h:22 的未办事项。故这里用「4 个数据 CW
 *    的退化 stripe」，靠 pattern 异或为 0 还原被擦 CW。真 stripe 覆盖见
 *    test_generate_stripe_parity。
 *
 *  参数组合：random=OFF（无加扰，退化 stripe 成立：4 条 CW 异或恰好为 0——
 *            META_i = (base | i) 异或为 0、CRC 线性、ECC 线性、USER 明文异或为 0）。
 *            random=ON 不成立——各 CW keystream 不同，4 条编码后 CW 的异或不为 0，
 *            退化 stripe 的前提就破了。（v24 起被擦 CW 的种子本身可从 XOR 还原的明文
 *            META 取回，但那不改变前提不成立这件事。）
 *
 *  编译开关：CONFIG_FC_RAID
 **************************************************************************/

#if CONFIG_FC_RAID

static void do_raid_recover_test(uint8_t random_mode, uint8_t failed_idx)
{
    fc_command_t *cmd   = l2_cmd(L2_BM_PLANE0_ALL, L2_META, random_mode, FC_CRC_ENABLE);
    uint8_t       count = test_cw_count(cmd);

    munit_assert_uint32(count, >=, 3U);
    munit_assert_uint32(failed_idx, <, count);

    /* 不同 CW 填不同 pattern（USER 数据区 512B） */
    for (uint8_t i = 0U; i < count; i++)
    {
        (void)memset(test_cw_user(cmd, i), (uint8_t)(0x10U + i), FC_BUFFER_USER_SIZE);
    }

    /* encode：META + CRC + 加扰? + ECC（RAID 不在 encode 里做，见 fc_data_path.c:191） */
    munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);

    /* 破坏指定 CW（全 CW 528B 翻转，使 ECC 不可纠） */
    uint8_t *failed_base = test_cw_base(cmd, failed_idx);
    for (uint16_t j = 0U; j < FC_BUFFER_POOL_CW_SIZE; j++)
    {
        failed_base[j] ^= 0xFF;
    }

    /* raid_recover：XOR 重建 + ECC 重试 + 解扰 */
    munit_assert_int(fc_data_path_raid_recover(cmd, failed_idx), ==, FC_STATUS_OK);

    /* 验证：全部 CW USER 数据区恢复原始 pattern */
    for (uint8_t i = 0U; i < count; i++)
    {
        munit_assert_uint32(test_check_pattern(test_cw_user(cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x10U + i)), ==, 1U);
    }

    fc_command_release(cmd);
}

static MunitResult test_raid_recover_no_random(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;
    do_raid_recover_test(FC_RANDOM_DISABLE, 1U);
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
    /* 分配数据命令（4 CW, random=ON, raid=NORMAL — encode 里不做 XOR） */
    fc_command_t *data_cmd = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);
    uint8_t       cw_cnt   = test_cw_count(data_cmd);

    munit_assert_uint32(cw_cnt, ==, 4U);

    /* 不同 CW 填不同 pattern */
    for (uint8_t i = 0U; i < cw_cnt; i++)
    {
        (void)memset(test_cw_user(data_cmd, i), (uint8_t)(0x20U + i), FC_BUFFER_USER_SIZE);
    }

    /* encode 数据命令：META → CRC → Random → ECC */
    munit_assert_int(fc_data_path_encode(data_cmd), ==, FC_STATUS_OK);

    /* 分配 parity 命令（同 CW 数，random=OFF） */
    fc_command_t *parity_cmd = l2_cmd(L2_BM_PLANE0_ALL, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    munit_assert_uint32(test_cw_count(parity_cmd), ==, cw_cnt);

    /* 同步 parity desc 的 META（模拟 fc_stripe_submit 中的行为） */
    for (uint8_t i = 0U; i < cw_cnt; i++)
    {
        test_cw_desc(parity_cmd, i)->meta = test_cw_desc(data_cmd, i)->meta;
    }

    /* 调用 generate_stripe_parity — XOR 数据命令 CW 到 parity buffer + ECC 编码 parity */
    {
        const fc_command_t *data_ptrs[1U]; /* PLANE_NUM=2 → 1 条数据命令 */
        data_ptrs[0U] = data_cmd;
        munit_assert_int(fc_data_path_generate_stripe_parity(data_ptrs, 1U, parity_cmd), ==, FC_STATUS_OK);
    }

    /*
     * 验证：清空数据命令的 CW0 → 用 fc_raid_recover() 从 parity + 其他 CW 重建 →
     * ECC 解码 + 解扰 → 与原始 pattern 一致。
     *
     * buffers[] 数组：[CW0, CW1, CW2, CW3, parity_CW0, parity_CW1, parity_CW2, parity_CW3]
     * XOR 重建时传入所有 CW 和 parity CW，fc_raid_recover 会跳过 failed_idx 并 XOR 其余。
     */
    {
        uint8_t *buffers[CW_TOTAL_MAX];

        for (uint8_t i = 0U; i < cw_cnt; i++)
        {
            buffers[i]          = test_cw_base(data_cmd, i);
            buffers[cw_cnt + i] = test_cw_base(parity_cmd, i);
        }
        uint8_t total_count = (uint8_t)(cw_cnt + cw_cnt); /* 8 */

        /* 清空数据命令 CW0，模拟丢失 */
        (void)memset(buffers[0U], 0, FC_BUFFER_POOL_CW_SIZE);

        /* RAID XOR 重建 CW0：传入所有 CW（含 parity） */
        {
            uint8_t recovered[FC_BUFFER_POOL_CW_SIZE];

            fc_raid_recover(recovered, buffers, total_count, 0U, FC_BUFFER_POOL_CW_SIZE);
            (void)memcpy(buffers[0U], recovered, FC_BUFFER_POOL_CW_SIZE);
        }

        /* ECC 解码 + 解扰后验证数据 CW0~CW3（parity CW 不验证，它们存储 XOR 结果） */
        const fc_ldpc_config_t cfg = {FC_BUFFER_DATA_SIZE, 8U, 4U};

        for (uint8_t i = 0U; i < cw_cnt; i++)
        {
            uint8_t *base = buffers[i];

            munit_assert_int(fc_ldpc_decode(&cfg, base, FC_BUFFER_DATA_SIZE, NULL), ==, FC_STATUS_OK);
            (void)fc_random_transform(base + FC_CRC_USER_OFFSET, FC_BUFFER_USER_SIZE, test_cw_seed(data_cmd, i));
            munit_assert_uint32(
                test_check_pattern(test_cw_user(data_cmd, i), FC_BUFFER_USER_SIZE, (uint8_t)(0x20U + i)), ==, 1U);
        }
    }

    fc_command_release(data_cmd);
    fc_command_release(parity_cmd);
    return MUNIT_OK;
}

#endif /* CONFIG_FC_RAID */

/**************************************************************************
 *  flat CW 掩码建链（v7）
 *
 *  建链走 FTL 策略层（ftl_cmd_configure_descriptors），不碰 NAND：
 *    - 位图按 **flat 绝对编号**（bit n = plane*CW_NUM + cw）逐位建 desc
 *    - 链序 plane-major
 *    - v27：位图是**字面**的（0 = 空集，不再展开成"段内全部 CW"）——三条用例各自
 *      显式给位图
 *
 *  「按掩码建整条链」是策略，v13 起属 FTL（FC 只剩单数逐条入口
 *  fc_command_configure_descriptor）。这三条用例仍留在 Level 2，
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
static const fc_cmd_conf_t L2_PROG_MULTI = {
    .operation = (DWORD)OP_PROGRAM_PAGE_MULTI_PLANE,
    .crc       = FC_CRC_ENABLE,
    .random    = FC_RANDOM_ENABLE,
    .ldpc      = FC_ECC_MODE_BCH,
};

static const fc_cmd_conf_t L2_PROG_SINGLE = {
    .operation = (DWORD)OP_NORMAL_PROGRAM_PAGE,
    .crc       = FC_CRC_ENABLE,
    .random    = FC_RANDOM_ENABLE,
    .ldpc      = FC_ECC_MODE_BCH,
};

/* 期望链（逐 desc 的 plane / cw），与下面 l2_bm_cases 的行一一对应。 */
static const uint8_t l2_bm_mixed_plane[] = {0U, 0U, 1U};
static const uint8_t l2_bm_mixed_cw[]    = {0U, 3U, 1U};
static const uint8_t l2_bm_p1_plane[]    = {1U, 1U, 1U, 1U};
static const uint8_t l2_bm_p1_cw[]       = {0U, 1U, 2U, 3U};
static const uint8_t l2_bm_span_plane[]  = {0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U};
static const uint8_t l2_bm_span_cw[]     = {0U, 1U, 2U, 3U, 0U, 1U, 2U, 3U};

typedef struct
{
    const char          *name;
    uint8_t              addr_plane; /* 段内 plane（配合 conf 决定段 = [plane, plane+1) 或 [0,2)） */
    uint32_t             bitmap;     /* flat 绝对编号位图（v27 起字面量，0 = 空集） */
    const fc_cmd_conf_t *conf;
    uint8_t              n;         /* 期望链上 desc 数 */
    const uint8_t       *plane_exp; /* 期望链：逐 desc 的 plane */
    const uint8_t       *cw_exp;    /* 期望链：逐 desc 的 cw */
} l2_bm_case_t;

static const l2_bm_case_t l2_bm_cases[] = {
  /* 0x29 = plane0 取 {CW0,CW3} + plane1 取 {CW1} → 3 条 desc（plane-major） */
    {      "flat-mixed", 0U, 0x29U,  &L2_PROG_MULTI, 3U, l2_bm_mixed_plane, l2_bm_mixed_cw},
 /* 0xF0 = 只选 plane 1 的 4 个 CW（绝对编号，不是 0x0F）→ 4 条 desc 全在 plane 1。
  * 段必须是**单 plane 且 addr.plane = 1**：用 multi-plane op 配只覆盖 plane 1 的位图
  * 会被 test_cmd_build 的「逐 plane 覆盖」断言挡下——那正是静默编程未覆盖 plane 的形态。 */
    {     "plane1-only", 1U, 0xF0U, &L2_PROG_SINGLE, 4U,    l2_bm_p1_plane,    l2_bm_p1_cw},
 /* 0xFF = 整段 → 8 条 desc，钉住「段内每个 plane × 每个 CW 都有一条」这条不变量。
  * ⚠ 位图必须显式给（v27 起 0 是空集，不再展开）；config.cw 不参与推导（v23 起）。 */
    {"span-multi-plane", 0U, 0xFFU,  &L2_PROG_MULTI, 8U,  l2_bm_span_plane,  l2_bm_span_cw},
};

/* 场景名（munit 要 NULL 结尾的 const char**），与 l2_bm_cases 一一对应。 */
static const char *l2_bm_case_names[] = {
    "flat-mixed",
    "plane1-only",
    "span-multi-plane",
    NULL,
};

_Static_assert((sizeof(l2_bm_case_names) / sizeof(l2_bm_case_names[0]) - 1U) == (sizeof(l2_bm_cases) / sizeof(l2_bm_cases[0])),
               "l2_bm_case_names 与 l2_bm_cases 数量不符");

/* MunitTest.parameters 是**非 const** 指针（munit.h:432）⇒ 本数组不能声明成 const。 */
static MunitParameterEnum l2_bm_params[] = {
    {"case", l2_bm_case_names},
    {  NULL,             NULL},
};

static const l2_bm_case_t *l2_bm_case_find(const char *name)
{
    uint32_t i;

    if (name == NULL)
    {
        return NULL;
    }

    for (i = 0U; i < (sizeof(l2_bm_cases) / sizeof(l2_bm_cases[0])); i++)
    {
        if (strcmp(l2_bm_cases[i].name, name) == 0)
        {
            return &l2_bm_cases[i];
        }
    }

    return NULL;
}

static MunitResult test_cw_bitmap(const MunitParameter params[], void *data)
{
    const l2_bm_case_t *c;
    fc_command_t       *cmd;
    uint8_t             plane_out[CW_TOTAL_MAX];
    uint8_t             cw_out[CW_TOTAL_MAX];
    uint8_t             n;
    uint8_t             i;

    (void)data;

    c = l2_bm_case_find(munit_parameters_get(params, "case"));
    munit_assert_true(c != NULL); /* --param case 传了表外的名字 */

    cmd = test_cmd_build(&(nand_addr_t){.block = TEST_BLOCK, .plane = c->addr_plane, .page = TEST_PAGE},
                         c->bitmap,
                         L2_META,
                         c->conf);

    /* config.cw 不回写（v23 起它连建链都不参与，纯残留字段）——三条场景同判据。 */
    munit_assert_uint32(cmd->config.cw, ==, 0U);

    n = test_collect_chain(cmd, plane_out, cw_out);
    munit_assert_uint32(n, ==, c->n);

    for (i = 0U; i < n; i++)
    {
        munit_assert_uint32(plane_out[i], ==, c->plane_exp[i]);
        munit_assert_uint32(cw_out[i], ==, c->cw_exp[i]);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  所有 Level 2 测试共享 test_setup / test_tear_down（共享夹具模块
 *  fc_test_common：fc_init 一次性 + 开 NAND 放行闩锁 + 分配/释放源数据表）
 **************************************************************************/

MunitTest g_fc_data_path_tests[] = {
  /* ── 步骤级（单 CW 原语）── */
    {          "crc_step_detect",           test_crc_step_detect, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l2_crc_params},
    {          "scramble_whiten",           test_scramble_whiten, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,          NULL},
    {                 "ecc_step",                  test_ecc_step, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l2_ecc_params},

 /* ── encode ── */
    {                   "encode",                    test_encode, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE, l2_enc_params},
    {      "encode_crc_disabled",       test_encode_crc_disabled, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── process_read ── */
    {          "process_read_ok",           test_process_read_ok, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,  l2_rt_params},
    {         "process_read_unc",          test_process_read_unc, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {"process_read_masked_chain", test_process_read_masked_chain, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {  "process_read_mata_error",   test_process_read_mata_error, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {   "process_read_crc_error",    test_process_read_crc_error, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
    {    "process_read_ldpc_raw",     test_process_read_ldpc_raw, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── raid_recover ── */
#if CONFIG_FC_RAID
    {   "raid_recover_no_random",    test_raid_recover_no_random, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},

 /* ── stripe parity ── */
    {   "generate_stripe_parity",    test_generate_stripe_parity, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION,          NULL},
#endif  /* CONFIG_FC_RAID */

  /* ── flat CW 掩码建链（v7）── */
    {                "cw_bitmap",                 test_cw_bitmap, test_setup, test_tear_down,             MUNIT_TEST_OPTION_NONE,  l2_bm_params},

    {                       NULL,                           NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE,          NULL},
};
