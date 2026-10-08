/*
 * test_fc_level2.c — Level 2 数据路径测试：fc_data_path 模块
 *
 * 测试三个数据路径管线函数：
 *   fc_data_path_encode       写路径：META → CRC? → Random 加扰? → ECC 编码
 *   fc_data_path_process_read 读路径 Stage 1：ECC 解码 → 条件解扰 → CRC 校验 → META 校验
 *   fc_data_path_raid_recover 读路径 Stage 2：RAID XOR 重建 → ECC 重试 → 解扰
 *
 * ── 两种用例形态 ──
 *   ① **上板往返**（正向用例）：擦块 → 写一页 → 读回 → 与源表比对。走真实 NAND，
 *      数据路径由 FC 自己驱动（见下）。这一半是本轮新增的覆盖。
 *   ② **buffer 级**（错误分支 / 链几何）：建链后**不提交**，手工调
 *      fc_data_path_encode / process_read / raid_recover，只查编码产物与 status 位。
 *      UNC / MATA / CRC 三条错误分支与三条掩码几何用例属此类——它们在 NAND 上
 *      构造不出（或构造出来不可控），且几何用例的 multi-plane 半掩码组合会触发
 *      「未覆盖 plane 被静默编程」的已知陷阱（见 fc_test_common.h 末尾）。
 *
 * ── FC 会自己跑数据路径（重要）──
 *   fc_poll_dispatch_single() 对 page write 调 fc_data_path_encode，
 *   fc_complete_from_nand()   对 page read 调 fc_data_path_process_read。
 *   所以**提交出去的命令绝不能再手工调这两个函数**（会重复编码 / 重复解扰）。
 *   要直接单测编码产物或错误分支，请另建命令、不提交、自行调用。
 *
 * ── 夹具 ──
 *   setup / teardown、读写擦原语、源数据表、desc 链小工具全部来自共享模块
 *   fc_test_common（Level 2 / Level 3 共用一份，见 tests/unit/fc_test_common.h）。
 *   用例运行前按「将要写下去的各个 PAA」把 g_source_data 的对应槽填进各 CW 的
 *   USER 区（test_fill_user_from_source），读回后按 PAA 逐字节比对
 *   （test_verify_against_source）——读错 CW / 读错 plane 会当场显形。
 *
 * ── 配置形态 ──
 *   random     : FC_RANDOM_ENABLE / FC_RANDOM_DISABLE
 *   crc        : FC_CRC_ENABLE(=0) / FC_CRC_DISABLE
 *   ldpc       : FC_ECC_MODE_BCH（本层默认，走真实编解码）/ FC_ECC_MODE_LDPC_RAW
 *   cw_mask    : 0 = **本命令 plane 段内的全部 CW**（v23）；非 0 = flat 绝对编号掩码
 *   raid       : 形参保留但**数据路径不读**——config.raid 全仓唯一读者是 fc.c 的
 *                分派钩子（决定走不走 fc_stripe_submit）
 *
 * ⚠ 加扰种子（v24）= 本 CW 的 **META（扇区 LBA）**，由 command->meta 的高位 + 本 desc
 *   的 plane/cw 造出。若 meta == 0，4 条 CW 的种子只由 cw(0..3) 派生，仍互异但都来自
 *   小 LBA——故 fixture 显式给一个非零 LBA，让「种子随 LBA 变」这条性质真的被测到。
 *   另有 random=ON 用例加「encode 后 USER 区 ≠ 明文」断言自证加扰确实发生
 *   （v22 教训：fixture 一旦退化，keystream 可能整片为 0，往返恒等式会把问题盖过去）。
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
#include "ftl/ftl_fc_interface.h"
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

/**************************************************************************
 *  本层专用的小封装
 **************************************************************************/

/* 建一条单 plane（plane 0）命令并设好本层的 config 形态：
 * BCH 编解码 + 指定加扰 / CRC。掩码与 meta 由调用者给。 */
static fc_command_t *l2_page_cmd(uint8_t operation, uint32_t cw_mask, uint32_t meta, uint8_t random_mode, uint8_t crc_mode)
{
    fc_command_t *cmd = test_page_cmd_ex(TEST_BLOCK, TEST_PAGE, TEST_PLANE_BM(0), 0U, operation, cw_mask, meta);

    cmd->config.random = random_mode;
    cmd->config.crc    = crc_mode;
    cmd->config.ldpc   = FC_ECC_MODE_BCH;

    return cmd;
}

/* 读回后要求每个 CW 的 status **恰好**是 READY（一个错误位都没有）。
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

/* 上板往返：擦块 → 按掩码写一页 → 按**同一掩码**读回 → 校验。
 * 写读两侧 random / crc / ldpc 必须一致——同一份物理数据，两种期望形态会自相矛盾。
 * ⚠ 提交出去的命令由 FC 自己跑 encode / process_read，这里不手工调用。 */
static void l2_round_trip(uint32_t cw_mask, uint8_t random_mode, uint8_t crc_mode)
{
    test_erase_block(TEST_BLOCK, TEST_PLANE_BM_ALL, 1U);

    fc_command_t *wcmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, cw_mask, L2_META, random_mode, crc_mode);

    test_fill_user_from_source(wcmd);
    test_submit_wait(wcmd);

    fc_command_t *rcmd =
        l2_page_cmd((uint8_t)OP_NORMAL_READ_PAGE_WITH_DATA_OUT, cw_mask, L2_META, random_mode, crc_mode);

    test_memset_read_buffer(rcmd);
    test_submit_wait(rcmd);

    l2_verify_all_ready(rcmd);
    test_verify_against_source(rcmd);

    munit_assert_int(fc_command_release(wcmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_command_release(rcmd), ==, FC_STATUS_OK);
}

/**************************************************************************
 *  fc_data_path_encode 测试
 *
 *  验证管线（Stage 3 跨命令 stripe RAID 已移出，见 fc_data_path.c:191）：
 *     Data → META → CRC? → Random 加扰? → ECC 编码
 *
 *  每条用例都是「buffer 级查编码产物 + 上板往返」两段：
 *    - buffer 级那份命令**不提交**（FC 提交时会自己再 encode 一次），
 *      用来看编码产物本身（USER 是否明文、CRC 字段是否被写、parity 是否非零）。
 *    - 上板那份走真实 NAND，验证编解码 + 加扰 + 校验的端到端一致性。
 *
 *  参数组合：
 *     random  crc      cw    说明
 *     off     on       4CW   仅 META + CRC + ECC
 *     on      on       4CW   加扰 + ECC（另证 USER 区确已非明文）
 *     off     on       1CW   单 CW 边界
 *     off     off      4CW   CRC 关闭 → CRC 字段不被写（哨兵保留）
 **************************************************************************/

static MunitResult test_encode_random_off(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* ── buffer 级：encode 产物 ── */
    {
        fc_command_t *enc = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

        munit_assert_uint32(test_cw_count(enc), ==, CW_NUM);

        test_fill_user_from_source(enc);
        munit_assert_int(fc_data_path_encode(enc), ==, FC_STATUS_OK);

        /* 加扰关闭 ⇒ USER 区仍是明文（[0,8) 是 META + CRC，不属于 USER 区） */
        test_verify_against_source(enc);

        /* 末 8 字节为 ECC parity，非零 */
        for (uint8_t i = 0U; i < CW_NUM; i++)
        {
            const uint8_t *base        = test_cw_base(enc, i);
            uint8_t        parity_zero = 1U;

            for (uint16_t j = FC_BUFFER_DATA_SIZE; j < FC_BUFFER_POOL_CW_SIZE; j++)
            {
                if (base[j] != 0U)
                {
                    parity_zero = 0U;
                    break;
                }
            }
            munit_assert_uint32(parity_zero, ==, 0U);
        }

        fc_command_release(enc);
    }

    /* ── 上板往返 ── */
    l2_round_trip(0U, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

static MunitResult test_encode_random_on(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* ── buffer 级：encode 产物 ── */
    {
        fc_command_t *enc = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

        munit_assert_uint32(test_cw_count(enc), ==, CW_NUM);

        test_fill_user_from_source(enc);
        munit_assert_int(fc_data_path_encode(enc), ==, FC_STATUS_OK);

        /* 自证加扰真的发生了：encode 后 USER 区应已不是明文。
         * 否则 fixture 一旦退化（例如 seed 恒 0 使 keystream 整片为 0），
         * 下面"解扰后等于明文"的恒等式会把问题原样盖过去。 */
        munit_assert_uint8(l2_user_matches_source(enc), ==, 0U);

        fc_command_release(enc);
    }

    /* ── 上板往返 ── */
    l2_round_trip(0U, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

static MunitResult test_encode_single_cw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* ── buffer 级：只取 plane0 的 CW0（1 条 desc） ── */
    {
        fc_command_t *enc = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0x01U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

        munit_assert_uint32(test_cw_count(enc), ==, 1U);

        test_fill_user_from_source(enc);
        munit_assert_int(fc_data_path_encode(enc), ==, FC_STATUS_OK);
        test_verify_against_source(enc);

        fc_command_release(enc);
    }

    /* ── 上板往返（写读两侧掩码必须一致） ── */
    l2_round_trip(0x01U, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

/* CRC 关闭分支：encode 不写 CRC 字段（哨兵原样保留）→ 读侧 Pass 3 整个跳过 → 无 CRC_ERROR */
static MunitResult test_encode_crc_disabled(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* ── buffer 级：CRC 字段不被写 ── */
    {
        fc_command_t *enc = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

        munit_assert_uint32(test_cw_count(enc), ==, CW_NUM);

        test_fill_user_from_source(enc);
        for (uint8_t i = 0U; i < CW_NUM; i++)
        {
            /* 哨兵：确认 encode 在 CRC 关闭时确实没碰 [4,8) */
            (void)memset(test_cw_base(enc, i) + FC_CRC_META_SIZE, 0xFF, FC_CRC_FIELD_SIZE);
        }

        munit_assert_int(fc_data_path_encode(enc), ==, FC_STATUS_OK);

        for (uint8_t i = 0U; i < CW_NUM; i++)
        {
            munit_assert_uint32(test_check_pattern(test_cw_base(enc, i) + FC_CRC_META_SIZE, FC_CRC_FIELD_SIZE, 0xFFU),
                                ==,
                                1U);
        }

        fc_command_release(enc);
    }

    /* ── 上板往返 ── */
    l2_round_trip(0U, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

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
 *
 *  参数组合：
 *     random  ecc/crc/meta  cw / mask      说明
 *     on      全 OK         4CW            解码成功 → 解扰 → 全 READY
 *     off     全 OK         4CW            只解码
 *     on      有 UNC        4CW            不解扰，返回 IO_ERROR
 *     on      全 OK         1CW            单 CW 边界
 *     on      全 OK         2CW / 0x05     非连续掩码：链位 0,1 ↔ cw 0,2
 *     off     META 不符     4CW            写 LBA=A 读 LBA=B → MATA_ERROR
 *     off     CRC 不符      4CW            写关 CRC 读开 CRC → CRC_ERROR
 *     off     全 OK         4CW            LDPC_RAW：跳过 ECC 编码/解码
 **************************************************************************/

static MunitResult test_process_read_ok_random_on(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2_round_trip(0U, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

static MunitResult test_process_read_ok_random_off(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2_round_trip(0U, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

/* UNC：先正常往返（证明写入正确），再破坏读回 buffer 重跑 process_read。
 * ⚠ 破坏必须**远超** BCH 纠错能力，否则会被"纠正"回去、用例假过：
 *   按 8 字节步长翻转整个 520B 数据区（65 处、最多 520 bit）——8B parity 纠不了。 */
static MunitResult test_process_read_unc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_erase_block(TEST_BLOCK, TEST_PLANE_BM_ALL, 1U);

    fc_command_t *wcmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    test_fill_user_from_source(wcmd);
    test_submit_wait(wcmd);

    fc_command_t *rcmd =
        l2_page_cmd((uint8_t)OP_NORMAL_READ_PAGE_WITH_DATA_OUT, 0U, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    test_memset_read_buffer(rcmd);
    test_submit_wait(rcmd);

    l2_verify_all_ready(rcmd);
    test_verify_against_source(rcmd);

    /* 破坏 CW1 */
    {
        uint8_t *base = test_cw_base(rcmd, 1U);

        for (uint16_t j = 0U; j < FC_BUFFER_DATA_SIZE; j += 8U)
        {
            base[j] ^= 0xFFU;
        }
    }

    /* process_read 应返回 IO_ERROR（不解扰） */
    munit_assert_int(fc_data_path_process_read(rcmd), ==, FC_STATUS_IO_ERROR);

    /* 验证 desc status：CW1 为 UNC，其余为 READY（Pass 1 是**赋值**，错误位不会残留） */
    for (uint8_t i = 0U; i < test_cw_count(rcmd); i++)
    {
        munit_assert_uint32(test_cw_desc(rcmd, i)->status, ==, (i == 1U) ? FC_CW_STATUS_UNC : FC_CW_STATUS_READY);
    }

    munit_assert_int(fc_command_release(wcmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_command_release(rcmd), ==, FC_STATUS_OK);

    return MUNIT_OK;
}

static MunitResult test_process_read_single_cw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    l2_round_trip(0x01U, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

/* 非连续掩码 + 数据路径：链位 0,1 ↔ cw 0,2。
 * 专钉 Pass 4 用的是 paa_to_cw(desc->paa) 而非链走位——若改用链走位，
 * 第 2 条 desc 会拿 cw=1 去算期望 META，假报 MATA_ERROR。
 * ⚠ 用单 plane 段（multi_plane=0）而不是 multi_plane=2：掩码只覆盖 plane 0 时，
 *   multi-plane 写路径会给未覆盖的 plane 1 补一条同行回退地址、静默编一页全 0xFF
 *   （fc_test_common.h 末尾的陷阱说明）。几何（plane0 的 CW0/CW2）两者完全相同。 */
static MunitResult test_process_read_masked_chain(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    /* ── buffer 级：链几何 + 编解码往返 ── */
    {
        /* 0x05 = plane0 的 CW0 | CW2 */
        fc_command_t *cmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0x05U, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

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

        test_fill_user_from_source(cmd);
        munit_assert_int(fc_data_path_encode(cmd), ==, FC_STATUS_OK);
        munit_assert_int(fc_data_path_process_read(cmd), ==, FC_STATUS_OK);

        l2_verify_all_ready(cmd);
        test_verify_against_source(cmd);

        fc_command_release(cmd);
    }

    /* ── 上板往返（同一掩码） ── */
    l2_round_trip(0x05U, FC_RANDOM_ENABLE, FC_CRC_ENABLE);

    return MUNIT_OK;
}

/* Pass 4（META 校验）触发：写页时 LBA=A，读同一页时却报 LBA=B。
 * 这是"读到了别的页"的真实成因——数据本身仍能正确解出（种子取自 NAND 内的明文
 * META），但本命令声明的 LBA 与页内 META 不符 ⇒ 每个 CW 置 MATA_ERROR。
 * 返回值仍为 OK（Pass 4 只置位）。 */
static MunitResult test_process_read_mata_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_erase_block(TEST_BLOCK, TEST_PLANE_BM_ALL, 1U);

    fc_command_t *wcmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    test_fill_user_from_source(wcmd);
    test_submit_wait(wcmd);

    /* 读同一物理页（cmd->pba 的 page 不变），但声明的 LBA 换成"下一页"：
     * 低 3 位（plane/cw）不变 ⇒ 自洽校验①仍过，命中的是第二项校验②。 */
    fc_command_t *rcmd = l2_page_cmd((uint8_t)OP_NORMAL_READ_PAGE_WITH_DATA_OUT, 0U, (DWORD)(TEST_PAGE + 1U) << CW_WIDTH, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    test_memset_read_buffer(rcmd);
    test_submit_wait(rcmd);

    for (uint8_t i = 0U; i < test_cw_count(rcmd); i++)
    {
        munit_assert_uint32(test_cw_desc(rcmd, i)->status & FC_CW_STATUS_MATA_ERROR, !=, 0U);
    }

    /* 数据本身没错（种子从页内 META 自举），错的只是"这一页是谁" */
    test_verify_against_source(rcmd);

    munit_assert_int(fc_command_release(wcmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_command_release(rcmd), ==, FC_STATUS_OK);

    return MUNIT_OK;
}

/* Pass 3（CRC 校验）触发：写时关 CRC（[4,8) 留下哨兵，且该哨兵被 ECC 保护、原样写进
 * NAND），读时开 CRC → stored 哨兵 ≠ 重算值 → 每个 CW 置 CRC_ERROR。返回值仍为 OK。 */
static MunitResult test_process_read_crc_error(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    static const uint32_t sentinel = 0xDEADBEEFU;

    test_erase_block(TEST_BLOCK, TEST_PLANE_BM_ALL, 1U);

    fc_command_t *wcmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_DISABLE);

    test_fill_user_from_source(wcmd);
    for (uint8_t i = 0U; i < test_cw_count(wcmd); i++)
    {
        (void)memcpy(test_cw_base(wcmd, i) + FC_CRC_META_SIZE, &sentinel, FC_CRC_FIELD_SIZE);
    }
    test_submit_wait(wcmd);

    /* 读侧打开 CRC → stored 哨兵与重算值必不相等 */
    fc_command_t *rcmd =
        l2_page_cmd((uint8_t)OP_NORMAL_READ_PAGE_WITH_DATA_OUT, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    test_memset_read_buffer(rcmd);
    test_submit_wait(rcmd);

    for (uint8_t i = 0U; i < test_cw_count(rcmd); i++)
    {
        munit_assert_uint32(test_cw_desc(rcmd, i)->status & FC_CW_STATUS_CRC_ERROR, !=, 0U);
    }

    munit_assert_int(fc_command_release(wcmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_command_release(rcmd), ==, FC_STATUS_OK);

    return MUNIT_OK;
}

/* LDPC_RAW 分支：encode 跳过 ECC 编码（parity 区不写）、
 * process_read Pass 1 跳过解码直接标 READY → 数据原样往返。 */
static MunitResult test_process_read_ldpc_raw(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_erase_block(TEST_BLOCK, TEST_PLANE_BM_ALL, 1U);

    fc_command_t *wcmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    wcmd->config.ldpc = FC_ECC_MODE_LDPC_RAW;
    test_fill_user_from_source(wcmd);
    test_submit_wait(wcmd);

    fc_command_t *rcmd =
        l2_page_cmd((uint8_t)OP_NORMAL_READ_PAGE_WITH_DATA_OUT, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

    rcmd->config.ldpc = FC_ECC_MODE_LDPC_RAW;
    test_memset_read_buffer(rcmd);
    test_submit_wait(rcmd);

    l2_verify_all_ready(rcmd);
    test_verify_against_source(rcmd);

    munit_assert_int(fc_command_release(wcmd), ==, FC_STATUS_OK);
    munit_assert_int(fc_command_release(rcmd), ==, FC_STATUS_OK);

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
 *  这两条**保持 buffer 级**：NAND 上没有 parity CW，构造不出真 stripe。
 *
 *  编译开关：CONFIG_FC_RAID
 **************************************************************************/

#if CONFIG_FC_RAID

static void do_raid_recover_test(uint8_t random_mode, uint8_t failed_idx)
{
    fc_command_t *cmd   = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, random_mode, FC_CRC_ENABLE);
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
    fc_command_t *data_cmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_ENABLE, FC_CRC_ENABLE);
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
    fc_command_t *parity_cmd = l2_page_cmd((uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U, L2_META, FC_RANDOM_DISABLE, FC_CRC_ENABLE);

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
 *    - 掩码按 **flat 绝对编号**（bit n = plane*CW_NUM + cw）逐位建 desc
 *    - 链序 plane-major
 *    - mask == 0 时展开成**本命令 plane 段内的全部 CW**（v23；此前是每 plane 连续
 *      0..config.cw —— 单 plane 段内两者逐位相同，multi_plane 下也只差"段外不展开"）
 *
 *  「按掩码建整条链」是策略，v13 起属 FTL（FC 只剩单数逐条入口
 *  fc_command_configure_descriptor）。这三条用例仍留在 Level 2，
 *  因为验证的是 desc 链的几何（plane/cw 展开），而非 NAND 行为——
 *  且它们都是 multi-plane 半掩码组合，正是「未覆盖 plane 被静默编程」的陷阱形态，
 *  故**保持 buffer 级、绝不提交**。
 **************************************************************************/

/* 掩码 0x29：plane0 取 {CW0,CW3} + plane1 取 {CW1} → 3 条 desc（plane-major） */
static MunitResult test_cw_mask_flat_mixed(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = test_page_cmd_ex(TEST_BLOCK, TEST_PAGE, TEST_PLANE_BM_ALL, 1U, (uint8_t)OP_NORMAL_PROGRAM_PAGE, 0x29U /* 0b0010_1001 */, L2_META);

    uint8_t plane_out[CW_TOTAL_MAX];
    uint8_t cw_out[CW_TOTAL_MAX];
    uint8_t n = test_collect_chain(cmd, plane_out, cw_out);

    munit_assert_uint32(n, ==, 3U);
    munit_assert_uint32(plane_out[0], ==, 0U);
    munit_assert_uint32(cw_out[0], ==, 0U);
    munit_assert_uint32(plane_out[1], ==, 0U);
    munit_assert_uint32(cw_out[1], ==, 3U);
    munit_assert_uint32(plane_out[2], ==, 1U);
    munit_assert_uint32(cw_out[2], ==, 1U);

    /* config.cw 不回写（v23 起它连建链都不参与，纯残留字段） */
    munit_assert_uint32(cmd->config.cw, ==, 0U);

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* 掩码 0xF0：只选 plane 1 的 4 个 CW（绝对编号，不是 0x0F）→ 4 条 desc 全在 plane 1 */
static MunitResult test_cw_mask_plane1_only(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = test_page_cmd_ex(TEST_BLOCK, TEST_PAGE, TEST_PLANE_BM_ALL, 1U, (uint8_t)OP_NORMAL_PROGRAM_PAGE, 0xF0U, L2_META);

    uint8_t plane_out[CW_TOTAL_MAX];
    uint8_t cw_out[CW_TOTAL_MAX];
    uint8_t n = test_collect_chain(cmd, plane_out, cw_out);

    munit_assert_uint32(n, ==, 4U);
    for (uint8_t i = 0U; i < n; i++)
    {
        munit_assert_uint32(plane_out[i], ==, 1U);
        munit_assert_uint32(cw_out[i], ==, i);
    }

    fc_command_release(cmd);
    return MUNIT_OK;
}

/* mask == 0 回归（v23 的核心正向用例）：multi_plane=2 + mask=0 必须建出 8 条 desc
 * —— 钉住"0 = 本命令 plane 段内的全部 CW"这条不变量（不是 4 条，也不是 0 条）。
 * ⚠ 本用例**不再**断言 config.cw 参与推导：v23 起 0 的展开与 config.cw 无关，
 *   故把 config.cw 写成 0（"没给提示"）也必须得到 8 条。 */
static MunitResult test_cw_mask_zero_multi_plane(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t *cmd = test_page_cmd_ex(TEST_BLOCK, TEST_PAGE, TEST_PLANE_BM_ALL, 1U, (uint8_t)OP_NORMAL_PROGRAM_PAGE, 0U /* 未指定 = 全部 */, L2_META);

    munit_assert_uint32(cmd->config.cw, ==, 0U); /* 建链不读它 */

    uint8_t plane_out[CW_TOTAL_MAX];
    uint8_t cw_out[CW_TOTAL_MAX];
    uint8_t n = test_collect_chain(cmd, plane_out, cw_out);

    munit_assert_uint32(n, ==, 8U);
    for (uint8_t i = 0U; i < n; i++)
    {
        munit_assert_uint32(plane_out[i], ==, (i < CW_NUM) ? 0U : 1U);
        munit_assert_uint32(cw_out[i], ==, (uint8_t)(i % CW_NUM));
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
  /* ── encode ── */
    {         "encode_random_off",          test_encode_random_off, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "encode_random_on",           test_encode_random_on, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "encode_single_cw",           test_encode_single_cw, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "encode_crc_disabled",        test_encode_crc_disabled, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── process_read ── */
    { "process_read_ok_random_on",  test_process_read_ok_random_on, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"process_read_ok_random_off", test_process_read_ok_random_off, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {          "process_read_unc",           test_process_read_unc, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "process_read_single_cw",     test_process_read_single_cw, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    { "process_read_masked_chain",  test_process_read_masked_chain, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {   "process_read_mata_error",    test_process_read_mata_error, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {    "process_read_crc_error",     test_process_read_crc_error, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {     "process_read_ldpc_raw",      test_process_read_ldpc_raw, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── raid_recover ── */
#if CONFIG_FC_RAID
    {    "raid_recover_no_random",     test_raid_recover_no_random, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* ── stripe parity ── */
    {    "generate_stripe_parity",     test_generate_stripe_parity, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
#endif  /* CONFIG_FC_RAID */

  /* ── flat CW 掩码建链（v7）── */
    {        "cw_mask_flat_mixed",         test_cw_mask_flat_mixed, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {       "cw_mask_plane1_only",        test_cw_mask_plane1_only, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {  "cw_mask_zero_multi_plane",   test_cw_mask_zero_multi_plane, test_setup, test_tear_down, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

    {                        NULL,                            NULL,       NULL,           NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
