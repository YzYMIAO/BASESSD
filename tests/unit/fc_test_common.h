#ifndef FC_TEST_COMMON_H
#define FC_TEST_COMMON_H

/*
 * fc_test_common.h — FC-NAND 单元测试共享夹具（Level 2 / Level 3 共用）
 *
 * 本模块只做两件事：
 *   ① 把「测试用的读写擦」塌缩成 5 个按语义命名的页 helper；
 *   ② 给它们一套「数据即地址」的源数据约定，使读错 CW / plane / page / block
 *      当场显形。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  一、5 个页 helper
 * ══════════════════════════════════════════════════════════════════════
 *
 *   fc_command_status_t test_erase_block(block, plane_bitmap, config);
 *   fc_command_status_t test_prog_page  (block, plane_bitmap, page, config, &data);
 *   fc_command_status_t test_read_page  (block, plane_bitmap, page, config, &data);
 *   void                test_prog_block (block, plane_bitmap, config, &data);
 *   void                test_read_block (block, plane_bitmap, config, &data);
 *
 * 每个 helper 自己走完整条生命周期，**不把命令交回调用者**：
 *
 *   fc_command_alloc → 拷 config → 定 pba → 建链 → 搬数据
 *     → fc_submit → fc_service 轮询 → 取完成 → fc_command_release
 *
 * 返回 fc_command_status_t（= CQ 完成项，聚合 status / param 都在里面）。
 * 要检查每 CW 细节请在 release 之前由 helper 内部做（见「校验」）。
 *
 * 不走页路径的用例（RESET / READ_ID / GET_FEATURES / cache 序列 / 内部搬移 /
 * RANDOM_DATA_READ，以及 Level 2 的 buffer 级用例）走下面的 `test_cmd_build()`——
 * 它只建链、不提交，调用者拿到命令后自行 `fc_data_path_*` 或提交。
 * 5 个页 helper 本身就建立在它之上。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  二、config 由调用者给，multi_plane 由 operation 派生
 * ══════════════════════════════════════════════════════════════════════
 *
 * config 类型就是 fc_command_t.config 的类型 fc_cmd_conf_t（fc_command.h），
 * 不是新造的类型。`test_cmd_build` 做 `cmd->config = *config` 后**只覆盖**几个
 * 字段：ch/ce/lun（取自 addr）、module（固定 HOST_IO），以及
 *
 *     cmd->config.multi_plane = test_is_multi_op(operation) ? PLANE_NUM : 0;
 *
 * 即「multi-plane 与否由 operation 本身决定」——调用者只要选对 op 变体
 * （OP_PROGRAM_PAGE_MULTI_PLANE / OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT /
 *   OP_ERASE_BLOCK_MULTI_PLANE 为 multi，其余为单 plane），不必自己填 multi_plane。
 * 页 helper 与 buffer 级用例共用这一处派生（`test_is_multi_op` 只有一份实现）。
 *
 * ⚠ enum 反极性：FC_CRC_ENABLE == 0、FC_RANDOM_ENABLE == 0（**0 是「开」**）；
 *   FC_ECC_MODE_BCH == 0、FC_RAID_MODE_NORMAL == 0、FC_CHAIN_END == 0、
 *   FC_PAGE_TYPE_LP == 0。fc_command_alloc 把结构体整体清零，所以 config 里
 *   不填的位域默认就是「CRC 开 / 加扰开 / BCH / 不 RAID / 单 plane / LP」。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  三、plane_bitmap 与 cw_bitmap
 * ══════════════════════════════════════════════════════════════════════
 *
 *   multi-plane op：plane_bitmap 必须 == TEST_PLANE_BM_ALL，命令 plane 段 = [0, PLANE_NUM)。
 *   单 plane op   ：plane_bitmap 必须**只有一位**，命令 plane 段 = [该 plane, +1)。
 *
 * 后者这条断言挡的是「multi_plane = 2 配只覆盖 plane 0 的位图」——那种组合会让
 * fc_build_request_data_planes 给未覆盖的 plane 补一条同行回退地址，写路径于是
 * **静默**给该 plane 编一页全 0xFF。
 *
 * v27 起 cw_bitmap 是**字面位图**（置 1 的位 = 要访问的 CW，0 = 一个都不访问），
 * v23~v26 的「0 = 本命令 plane 段内全部 CW」重载已取消 ⇒ 整段访问必须**显式**算出来。
 * v28 起它**不再存进命令结构体**，而是 ftl_cmd_configure_descriptors(cmd, cw_bitmap)
 * 的**形参**（建链的输入，用完即弃）。helper 按 plane_bitmap 算出「选中 plane 段内的
 * 全部 CW」（plane 1 单 plane ⇒ 0xF0，两 plane ⇒ 0xFF）当实参传进去。好处是它与 helper
 * 自己推出的 multi_plane / pba.plane 互为**交叉检查**——两者不一致时
 * ftl_cmd_resolve_cw_bitmap 的越段断言当场炸，而不是静默访问错的 plane。
 *
 * 要访问**离散** CW（如 RANDOM_DATA_READ 只读某个 cw）的用例直接调 test_cmd_build，
 * 自己传 `(uint32_t)1U << cw`。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  四、源数据约定：每个 CW 的 USER 区 = 该 CW 自己的 PAA 重复 128 次
 * ══════════════════════════════════════════════════════════════════════
 *
 *   g_source_data —— test_setup() 里 malloc 一块
 *   FC_BUFFER_USER_SIZE × CW_NUM × PLANE_NUM 的数组，每个 (plane, cw) 一个 512B 槽；
 *   test_tear_down() 释放。槽内**初始无内容**——内容由写路径填。
 *
 *   写（test_prog_page / test_prog_block）：
 *       逐 desc：槽 ← test_fill_paa(slot, desc->paa)    （128 个 dword 全是本 CW 的 PAA）
 *                desc 的 USER 区 ← 槽                    （明文）
 *       提交后 FC 自己跑 fc_data_path_encode（原地把 META/CRC/加扰/ECC 写进 CW），
 *       所以 **desc buffer 里此后不再是明文**，而 g_source_data 槽保持明文不变
 *       —— 这就是 `*data` 必须指向槽、不能从 buffer 拷回的原因。
 *
 *   读（test_read_page / test_read_block）：
 *       提交前把 desc 的 USER 区预填 0xCC（残留可见）；
 *       提交后 FC 自己跑 fc_data_path_process_read（原地解码），desc 的 USER 区
 *       就是明文；
 *       逐 desc：槽 ← desc 的 USER 区；`*data` 指回 g_source_data。
 *
 *   于是「读回来逐 dword 与期望 PAA 比」就同时验了 block / plane / page / cw 四维。
 *   （旧夹具是 4 字节周期且不含 CW 维度，而 cw × 512 是 4 的倍数 ⇒ CW 偏移在旧判据里
 *    完全不可见。）
 *
 *   校验入口：
 *     test_verify_page_paa(data, block, page, plane_bitmap) —— data 是 helper 交回的
 *        指针；按 plane_bitmap × CW_NUM 逐槽比「该 (block, page, plane, cw) 的 PAA」。
 *     test_check_paa(slot, paa) —— 单槽版，内联用例用。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  五、FC 会自己跑数据路径（重要）
 * ══════════════════════════════════════════════════════════════════════
 *
 *   fc_poll_dispatch_single() 对 page write 调 fc_data_path_encode，
 *   fc_complete_from_nand()    对 page read  调 fc_data_path_process_read。
 *   ⇒ **提交出去的命令绝不能再手工调这两个函数**（会重复编码 / 重复解扰）。
 *     要单测数据路径的错误分支，另建一条命令、**不提交**，自行调用。
 *
 * ══════════════════════════════════════════════════════════════════════
 *  六、meta 的语义（v24/v25 契约）
 * ══════════════════════════════════════════════════════════════════════
 *
 *   desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw，在建链那一刻铸出。
 *   它是本 CW 的**扇区 LBA**，同时是加扰种子（fc_random_seed_from_meta）与
 *   读回校验基准（Pass 4 ②：META 与本命令 LBA 不符 = 读到了别的页）。
 *   页 helper 一律取 `meta = page << CW_WIDTH` —— 写读同页自然一致，且**页号进了
 *   META**，于是「读错页」会被 Pass 4 抓到。
 *   内联用例想跳过 Pass 4 ② 可传 FC_META_UNKNOWN（不知道 LBA 的路径）。
 *   ⚠ 写路径不能用 FC_META_UNKNOWN——Stage 0 会把它原样写进 NAND，
 *     读回时 Pass 4 ①（自洽：低 3 位 == plane_cw）当场不过。
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

void *test_setup(const MunitParameter params[], void *user_data); /* fc_init 一次 + set_armed + malloc */
void  test_tear_down(void *fixture);                              /* free(g_source_data) */

/* ── 步骤级 CW 暂存区 ──
 *
 * 一块夹具分配的 CW 缓冲区（FC_BUFFER_POOL_CW_SIZE 字节，528B），setup 分配、
 * teardown 释放，内容**不预填**（用例自己负责）。
 *
 * 步骤级用例（直接调 fc_data_path_* 单 CW 原语，见 include/fc/fc_data_path.h）
 * 拿它当被测对象——不需要命令 / desc / 链，也不需要任何 config 位：
 * **调了哪些原语就是开了哪些功能**。
 *   ⚠ 不能放用例栈上：一个 CW 528B、多块就上千字节，嵌入式用例栈帧吃不消。
 *
 * idx < CW_TOTAL_MAX（8）；越界 ASSERT。 */
uint8_t *test_cw_scratch(uint8_t idx);

/* ── 只建链、不提交的原语 ──
 * buffer 级用例（数据路径单测）与不走页 helper 的命令（RESET / READ_ID /
 * GET_FEATURES / cache 序列 / 内部搬移 / RANDOM_DATA_READ）用它拿一条**已建好
 * desc 链、尚未提交**的命令，之后自行调 fc_data_path_* 或提交。
 *   · 地址走 nand_addr_t（block/plane/page + ch/ce/lun）。addr->cw **必须为 0**——
 *     链上每条 desc 的 CW 由 cw_bitmap 逐位铸出，地址里不带。
 *   · operation 与全部模式位走 config（const，可传 static 常量）。
 *   · **multi_plane 由 operation 派生**（三个 MULTI 变体 op ⇒ PLANE_NUM，其余 ⇒ 0），
 *     config 里填的值被覆盖——与 5 个页 helper 同一条规则，只有这一处实现。
 *   · meta 原样写进命令（写读同页须一致；mata 用例正需要「同页不同 LBA」）。
 *   · 无 desc 链的 op（ERASE / RESET / SET·GET_FEATURES）传 cw_bitmap = 0：
 *     ftl_cmd_needs_desc_chain 短路，desc_id 置 INVALID。
 *   · cmd->module 固定 HOST_IO、cmd->feature_addr 不设——需要时调用者自行覆盖。
 *   ⚠ 返回的命令**未提交**。写命令别先手工 fc_data_path_encode 再提交（FC 会再编一次，
 *     见「五」）。 */
fc_command_t *test_cmd_build(const nand_addr_t *addr, uint32_t cw_bitmap, uint32_t meta, const fc_cmd_conf_t *config);

/* ── 内联用例用的低层原语 ──
 * 需要**自己拿着命令**做事的用例（Level 3 的 READ_ID / GET_FEATURES / cache 序列 /
 * 内部搬移 / RANDOM_DATA_READ 等）走 test_cmd_build() 建链，再用这里的零件提交 /
 * 校验 / 释放。页 helper 只是把「建链 → 搬数据 → 提交 → 轮询 → 校验 → 释放」打包
 * 好；形态对不上时（如「擦除页的聚合 status 必然带 MATA_ERROR，只能在 release 前
 * 逐 CW 查 SUCCESS 位」）就用这套零件自己拼。 */

/* plane_bitmap → flat CW 位图：选中 plane 段内的全部 CW（plane 1 单 plane ⇒ 0xF0）。
 * 与 test_cmd_build 内部按 cmd->pba/config.multi_plane 反推的段互为**交叉检查**。 */
uint32_t test_plane_cw_bitmap(uint8_t plane_bitmap);

/* plane_bitmap 里序号最小的被选中 plane（test_cmd_build 的 addr.plane 用）。
 * plane_bitmap 必须非 0（不可达时返回 0）。 */
uint8_t test_first_plane(uint8_t plane_bitmap);

/* 提交 + 轮询到完成，**命令仍归调用者**（返回 CQ 完成项）。
 * 用毕调用者自己 fc_command_release()——本函数不释放。 */
fc_command_status_t test_submit_wait(fc_command_t *cmd);

/* ── 7 个页 helper（建立在 test_cmd_build 之上） ──
 * 内部 alloc / 建链 / 搬数据 / 提交 / 轮询 / 拷回 / 释放，命令不交回调用者。
 * 返回 CQ 完成项；`data` 为出参（可传 NULL），读回 / 写下的明文在 g_source_data 里。 */

fc_command_status_t test_erase_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config);

fc_command_status_t test_prog_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config, uint8_t **data);

fc_command_status_t test_read_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config, uint8_t **data);

/* 遍历整块：逐页调对应页 helper（页间自然轮询）。
 * test_read_block 逐页读并在**页内**校验 PAA 图案（否则 g_source_data 只剩最后一页可比）；
 * 擦除态（全 0xFF）的整块校验不由它承担——用例自己循环 test_read_page + 逐槽比 0xFFFFFFFF。 */
void test_prog_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);
void test_read_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);

/* ── cache 整块读写（ONFI cache 序列）──
 * **只有单 plane 变体**（ONFI 无 multi-plane cache 读 / 编程）⇒ plane_bitmap 必须恰好一个 plane。
 * config 只提供模式位（crc / random / ldpc / …）；**operation 被逐步覆盖**，调用者填什么都被忽略。
 *
 * test_prog_block_cache：前 PAGES_PER_BLOCK-1 页 OP_PROGRAM_PAGE_CACHE（80h-addr-data-15h），
 *   末页 OP_NORMAL_PROGRAM_PAGE（10h）收尾——15h 编的是 cache 寄存器里的当前内容，没有末页的
 *   10h 就没有最后一页真正落盘。meta 与普通写同口径（= 页 LBA）。
 *
 * test_read_block_cache：**不能**由 test_read_page 循环拼出——31h / 3Fh 无地址周期（设备自增），
 *   且 31h 输出的是**上一轮加载的页**，「命令地址 = 页、校验该页」的模型在此结构性不成立。
 *   故显式编排 START(p0) → SEQ ×(PAGES_PER_BLOCK-1) → LAST：
 *     · START（00h-addr-30h）把 page 0 载入 cache，**不输出数据**（cw_bitmap = 0，FC 不跑 process_read）；
 *     · 第 i 次 SEQ（i = 1..PAGES_PER_BLOCK-1）输出 page i-1、同时载入 page i ⇒ meta 取
 *       **(i-1) << CW_WIDTH**（真正输出的那页的 LBA）；命令地址页号对设备无意义，传 0；
 *     · LAST（3Fh）输出 cache 里最后一页（PAGES_PER_BLOCK-1）。
 *   共 65 条命令输出 64 页。`data` 收尾指向最后一页的明文。
 *
 * 两者都是**同步**的（逐条 submit → 轮询 → 释放）⇒ cache 的流水线收益在本夹具里测不到；
 * 这里验的是**数据正确性**：cache 命令序列走得通、META / CRC / 加扰 / ECC 全程不串。 */
void test_prog_block_cache(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);
void test_read_block_cache(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);

/* ── 数据搬运 / 校验 ── */

/* 单槽版：把 512B 槽填成 FC_BUFFER_USER_SIZE/4 个 paa。 */
void test_fill_paa(uint8_t *slot, nand_paa_t paa);
/* 单槽版：1 = 槽内 FC_BUFFER_USER_SIZE/4 个 dword 全等 paa。 */
uint8_t test_check_paa(const uint8_t *slot, nand_paa_t paa);

/* 活链版（内联用例用）：
 * test_fill_user_paa —— 逐 desc 填槽 + 拷进 desc 的 USER 区（写路径的「搬数据」那一步）；
 * test_memset_read_buffer —— 逐 desc 把 USER 区预填 0xCC（读路径的残留哨兵）。 */
void test_fill_user_paa(fc_command_t *cmd);
void test_memset_read_buffer(fc_command_t *cmd);

/* 按 (block, page, plane_bitmap) 逐槽校验 data 里的 PAA 图案——页 helper 的读回判据。 */
void test_verify_page_paa(const uint8_t *data, uint16_t block_index, uint16_t page_index, uint8_t plane_bitmap);

/* 活链版：逐 desc 断言 USER 区全 0xFF（擦除态） / 每 CW 的 status 含 FC_CW_STATUS_SUCCESS。 */
void    test_verify_erased(const fc_command_t *cmd);
uint8_t test_verify_cw_status_all_success(const fc_command_t *cmd);

/* ── desc 链小工具（内联用例用） ── */

fc_descriptor_t *test_cw_desc(const fc_command_t *cmd, uint8_t idx); /* 链上第 idx 条；越界 NULL */
uint8_t         *test_cw_user(const fc_command_t *cmd, uint8_t idx); /* 第 idx 条的 USER 区（512B） */
uint8_t         *test_cw_base(const fc_command_t *cmd, uint8_t idx); /* 第 idx 条的 528B CW 基址 */
uint8_t          test_cw_count(const fc_command_t *cmd);             /* 链长 */
uint8_t          test_collect_chain(const fc_command_t *cmd, uint8_t *plane_out, uint8_t *cw_out);
uint32_t         test_cw_seed(const fc_command_t *cmd, uint8_t idx); /* 该 CW 的加扰种子（= META 派生） */
uint8_t          test_check_pattern(const uint8_t *buffer, uint16_t length, uint8_t pattern);

#endif /* FC_TEST_COMMON_H */
