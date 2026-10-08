#ifndef FC_TEST_COMMON_H
#define FC_TEST_COMMON_H

/*
 * fc_test_common.h — FC-NAND 单元测试共享夹具（Level 2 / Level 3 共用）
 *
 * 本模块把「测试用的读写擦」收敛成一份，形如 src/ftl/ftl_fc_interface.c 的
 * ftl_sync_* 页级原语，但多了两件测试才需要的东西：
 *
 *   ① 全局源数据 g_source_data：test_setup() 里 malloc 一块
 *      FC_BUFFER_USER_SIZE × CW_NUM × PLANE_NUM 的数组，每个 (plane, cw) 一个
 *      512B 槽，槽内图案按槽号区分；test_tear_down() 释放。
 *      用例把「将要写下去的各个 PAA」对应的槽填进各 CW 的 USER 区，
 *      读回后再按 PAA 逐字节比对 —— 于是「读回了别的 CW / 别的 plane」
 *      一定会被抓到。
 *      （旧夹具是 4 字节周期且不含 CW 维度的图案，而 cw × 512 是 4 的倍数
 *        ⇒ CW 偏移在旧判据里完全不可见。）
 *
 *   ② 命令交回调用者：helper 只 submit + service + poll，**不 release**，
 *      测试函数拿回 cmd 后可继续读 desc 链的 buffer / status。
 *
 * ── 建 / 跑两段式 ──
 *   test_page_cmd()  只建链、不提交（地址在此时就定死进 desc->paa）
 *   test_submit_wait() 提交 + 推进 + 取完成（不 release）
 * 为什么必须分开：desc->paa 在建链那一刻由 cmd->pba 铸出
 * （fc_interface.c 的 fc_command_configure_descriptor），之后改 cmd->pba 不会
 * 回写 desc。而 fc_data_path_encode 又必须插在「建链之后、提交之前」——
 * 两段式是唯一能同时满足这两条的形态。
 *
 * **两段式还带来一条约定**：建链与「设 config」互不依赖——config 只在
 * encode / process_read / dispatch 改写时被读，desc->config 那份快照没有人回读。
 * 所以调用者可以在 test_page_cmd*() 返回之后、test_submit_wait() 之前自由覆盖
 * cmd->config.{crc,random,ldpc,raid}（本模块的默认值是「裸页」：CRC 开、
 * 不加扰、LDPC_RAW、不 RAID）。
 *
 * ── meta 的语义（v24/v25 契约）──
 *   desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw，在建链那一刻铸出。
 *   它是本 CW 的**扇区 LBA**，同时是加扰种子（fc_random_seed_from_meta）与
 *   读回校验基准（fc_data_path 的 Pass 4 ②：META 与本命令 LBA 不符 = 读到了别的页）。
 *   - test_page_cmd()：meta = page << CW_WIDTH（页 LBA）——写读同页自然一致，
 *     且**页号进了 META**，于是「读错页」会被 Pass 4 抓到。
 *   - test_page_cmd_ex()：meta **原样使用**，不做任何派生。想跳过 Pass 4 ②
 *     就传 FC_META_UNKNOWN（不知道 LBA 的路径，如 cache 读序列的中间项）。
 *     ⚠ 写路径不能用 FC_META_UNKNOWN——Stage 0 会把它原样写进 NAND，
 *       读回时 Pass 4 ①（自洽：低 3 位 == plane_cw）当场不过。
 *
 * ── FC 会自己跑数据路径（重要）──
 *   fc_poll_dispatch_single() 对 page write 调 fc_data_path_encode，
 *   fc_complete_from_nand() 对 page read 调 fc_data_path_process_read。
 *   所以**走本模块提交的命令不要再手工调这两个函数**（会重复编码/重复解扰）。
 *   要直接单测数据路径的错误分支，请另建命令、不提交，自行调用。
 *
 * ── multi_flag 与 plane_bitmap 的契约 ──
 *   multi_flag == 0：plane_bitmap 必须**只有一位**，即该 plane；命令 plane 段
 *                    = [该 plane, +1)，op 用单 plane 变体。
 *   multi_flag != 0：plane_bitmap 必须**全 plane**（TEST_PLANE_BM_ALL），
 *                    config.multi_plane = PLANE_NUM，op 用 *_MULTI_PLANE 变体。
 *   后者这条断言是为了挡「multi_plane=2 + 只覆盖 plane 1 的掩码」——
 *   那种组合会让 fc_build_request_data_planes 给未覆盖的 plane 补一条同行回退
 *   地址，写路径于是给该 plane 静默编一页全 0xFF。
 */

#include "common/assertion.h"
#include "fc/fc.h"
#include "fc/fc_command.h"
#include "ftl/ftl_fc_interface.h"
#include "munit.h"
#include "nand/nand_config.h"
#include "nand/onfi_1_0.h"

#include <stdint.h>

/* ── 测试用几何常量 ── */

#define TEST_PLANE_BM(p)  ((uint8_t)(1U << (p)))
#define TEST_PLANE_BM_ALL ((uint8_t)((1U << PLANE_NUM) - 1U))

#define TEST_BLOCK      (100U) /* 主测试块（与 Level 3 的 BLOCK_TEST 一致） */
#define TEST_BLOCK_DEST (102U) /* 内部搬移 / 目标块 */
#define TEST_PAGE       (32U)  /* 单页用例的默认页；非 0 使加扰种子非 0 */

#define TEST_SERVICE_LOOP_MAX (5000U)

/* 页级操作种类：helper 按 multi_flag 自动选 multi / normal 变体
 * （对应请求里「multi_flag 使能即使用 multi 类型的 op」）。 */
typedef enum
{
    TEST_OP_PROGRAM = 0U, /* OP_NORMAL_PROGRAM_PAGE / OP_PROGRAM_PAGE_MULTI_PLANE */
    TEST_OP_READ    = 1U, /* OP_NORMAL_READ_PAGE_WITH_DATA_OUT / OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT */
    TEST_OP_ERASE   = 2U, /* OP_ERASE_BLOCK / OP_ERASE_BLOCK_MULTI_PLANE */
} test_page_op_t;

/* ── 全局源数据（setup 分配 / teardown 释放） ── */

extern uint8_t *g_source_data;

/* 按 (plane, cw) 取源数据槽。索引 = plane * CW_NUM + cw，每槽 FC_BUFFER_USER_SIZE 字节。 */
static inline uint8_t *test_source_slot(uint8_t plane, uint8_t cw)
{
    ASSERT(g_source_data != NULL);
    ASSERT((plane < PLANE_NUM) && (cw < CW_NUM));
    return &g_source_data[((uint32_t)plane * CW_NUM + (uint32_t)cw) * FC_BUFFER_USER_SIZE];
}

/* ── setup / teardown（逐用例挂到 MunitTest 的 setup / tear_down 槽） ──
 * 本 fork 的 MunitSuite 没有 setup 字段（tests/framework/munit.h），故只能逐用例挂。 */

void *test_setup(const MunitParameter params[], void *user_data); /* fc_init 一次 + set_armed + malloc + 填图案 */
void  test_tear_down(void *fixture);                              /* free(g_source_data) */

/* ── 命令构造（建链，不提交） ── */

/* 主入口：按 multi_flag 选 op 变体，meta 由 page 派生（page << CW_WIDTH），
 * cw_mask = 0（= 本命令 plane 段内的全部 CW，v23）。 */
fc_command_t *test_page_cmd(uint16_t block_index, uint8_t page, uint8_t plane_bitmap, uint8_t multi_flag, test_page_op_t operation);

/* 逃逸口：operation 显式给定（cache / 内部搬移 / 随机列读等无 multi 变体的操作），
 * 且可显式给 cw_mask（非连续掩码、cw_mask == 0 回归）与 meta（**原样使用**）。 */
fc_command_t *test_page_cmd_ex(uint16_t block_index, uint8_t page, uint8_t plane_bitmap, uint8_t multi_flag, uint8_t operation, uint32_t cw_mask, uint32_t meta);

/* ── 提交 / 一步到位 ── */

/* submit + fc_service 循环 + poll + 校验 command_id。**不 release**。
 * 返回完成项本身（不需要的调用者直接忽略返回值）——聚合 status / param 在它里面。 */
fc_command_status_t test_submit_wait(fc_command_t *cmd);

/* submit + wait + release（不需要检查 completion 数据时用）。 */
void test_run_lifecycle(fc_command_t *cmd);

/* 建链 → 0xCC 预填 → 提交（普通读页 op）。返回 cmd（调用者负责 fc_command_release）。
 *
 * 只提供读侧的「一步到位」，**不提供写侧的**：写路径的 config 形态逐用例不同
 * （加扰 / CRC / 内部搬移 op / cache op），一律要在「建链之后、提交之前」改
 * cmd->config 或换 op，一步到位的封装反而挡路。读侧所有调用者要的形态恰好就是
 * 本模块的默认值（LDPC_RAW + 不加扰 + CRC 开），故封得住。 */
fc_command_t *test_read_page(uint16_t block_index, uint8_t page, uint8_t plane_bitmap, uint8_t multi_flag);

/* 建链 → 提交 → release（无数据链）。 */
void test_erase_block(uint16_t block_index, uint8_t plane_bitmap, uint8_t multi_flag);

/* ── 数据搬运 / 校验（按 desc->paa 的 plane/cw 索引源数据槽） ── */

void    test_fill_user_from_source(fc_command_t *cmd);
void    test_verify_against_source(fc_command_t *cmd);
void    test_verify_erased(fc_command_t *cmd);      /* 逐 desc 断言 USER 区全 0xFF */
void    test_memset_read_buffer(fc_command_t *cmd); /* 逐 desc USER 区预填 0xCC */
uint8_t test_verify_cw_status_all_success(fc_command_t *cmd);

/* ── desc 链小工具 ── */

fc_descriptor_t *test_cw_desc(const fc_command_t *cmd, uint8_t idx); /* 链上第 idx 条；越界 NULL */
uint8_t         *test_cw_user(const fc_command_t *cmd, uint8_t idx); /* 第 idx 条的 USER 区（512B） */
uint8_t         *test_cw_base(const fc_command_t *cmd, uint8_t idx); /* 第 idx 条的 528B CW 基址 */
uint8_t          test_cw_count(const fc_command_t *cmd);             /* 链长 */
uint8_t          test_collect_chain(const fc_command_t *cmd, uint8_t *plane_out, uint8_t *cw_out);
uint32_t         test_cw_seed(const fc_command_t *cmd, uint8_t idx); /* 该 CW 的加扰种子（= META 派生） */
uint8_t          test_check_pattern(const uint8_t *buffer, uint16_t length, uint8_t pattern);

#endif /* FC_TEST_COMMON_H */
