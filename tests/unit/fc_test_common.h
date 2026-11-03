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
 * 字段：ch/ce/lun（取自 addr），以及
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
 * 自己传该 CW 的 flat 绝对位 `(uint32_t)1U << ((uint32_t)plane * CW_NUM + cw)`
 * （⚠ 不是 `1U << cw`——那是 plane 盲的，plane 1 会越段）。
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
 *  六、meta 的语义（v24/v25 契约；**单测侧取页 PBA**）
 * ══════════════════════════════════════════════════════════════════════
 *
 *   desc->meta = (command->meta & ~PLANE_CW_MASK) | plane_cw，在建链那一刻铸出。
 *   它是本 CW 的**页 PBA**（pba_from_addr 打包；低 3 位天然是 plane/cw，与
 *   PLANE_CW_MASK 同构），同时是加扰种子（fc_random_seed_from_meta）与读回校验
 *   基准（Pass 4 ②：META 与本命令声明的 PBA 不符 = 读到了别的页）。
 *
 *   为什么是 PBA 而不是 LBA：本夹具只验 **FC 的机制**——「写进去的 tag 必须能原样
 *   读回来」。PBA 是唯一地址、可查表，做这件事已经够用，且不必让单测假装知道 LBA
 *   分配。（**生产路径仍是 LBA**：FTL 交 LBA 建链，见 include/fc/fc_command.h。）
 *
 *   页 helper 一律取 `meta = pba_from_addr(本页地址)` —— 写读同页自然一致，
 *   且**页地址整体进了 META**，于是「读错页」会被 Pass 4 抓到。
 *   ⚠ 唯一例外是 test_read_page：meta 提成了形参，因为**内部搬移**要让读命令声明
 *     **源页** PBA（那页的 META 随数据一起搬到了目标块）。详见 test_fc_level3.c。
 *
 *   内联用例想跳过 Pass 4 ② 可传 FC_META_UNKNOWN（不知道页身份的路径）。
 *   ⚠ 写路径不能用 FC_META_UNKNOWN——Stage 0 会把它原样写进 NAND，
 *     读回时 Pass 4 ①（自洽：低 3 位 == plane_cw）当场不过。
 */

#include "assertion.h"
#include "fc.h"
#include "fc_command.h"
#include "ftl_config.h" /* pba_make（见 pba_from_addr 薄壳） */
#include "ftl_fc_interface.h"
#include "munit.h"
#include "nand_config.h"
#include "onfi_1_0.h"

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

/* nand_addr_t → DWORD（页 PBA）。
 *
 * v0.73 把 FTL 层地址全面 PBA 化时**删掉了生产的 `pba_from_addr()`**（它把
 * nand_addr_t 拖进 FTL 闭包）。但 FC 层的货币仍是 nand_addr_t，而页 helper 要拿
 * 「本页 PBA」当 meta ⇒ 测试侧保留同名薄壳。**逐位等价**：pba_make 就是旧函数那七个
 * 字段按同一组 PBA_*_SHIFT 打包，`addr->cw` 传 0 时即页 PBA。 */
static inline DWORD pba_from_addr(const nand_addr_t *addr)
{
    return pba_make(addr->ch, addr->ce, addr->lun, addr->block, addr->plane, addr->page, addr->cw);
}

/* ── 只建链、不提交的原语 ──
 * buffer 级用例（数据路径单测）与不走页 helper 的命令（RESET / READ_ID /
 * GET_FEATURES / cache 序列 / 内部搬移 / RANDOM_DATA_READ）用它拿一条**已建好
 * desc 链、尚未提交**的命令，之后自行调 fc_data_path_* 或提交。
 *   · 地址走 nand_addr_t（block/plane/page + ch/ce/lun）。addr->cw **必须为 0**——
 *     链上每条 desc 的 CW 由 cw_bitmap 逐位铸出，地址里不带。
 *   · operation 与全部模式位走 config（const，可传 static 常量）。
 *   · **multi_plane 由 operation 派生**（三个 MULTI 变体 op ⇒ PLANE_NUM，其余 ⇒ 0），
 *     config 里填的值被覆盖——与 5 个页 helper 同一条规则，只有这一处实现。
 *   · meta 原样写进命令（写读同页须一致；mata 用例正需要「同页不同 PBA」）。
 *   · 无 desc 链的 op（ERASE / RESET / SET·GET_FEATURES）传 cw_bitmap = 0：
 *     ftl_cmd_needs_desc_chain 短路，desc_id 置 INVALID。
 *   ⚠ 返回的命令**未提交**。写命令别先手工 fc_data_path_encode 再提交（FC 会再编一次，
 *     见「五」）。 */
fc_command_t *test_cmd_build(const nand_addr_t *addr, uint32_t cw_bitmap, uint32_t meta, const fc_cmd_conf_t *config);

/* 与 test_cmd_build 同配置校验，但**资源申请失败返回 NULL**（不 abort）——供压力用例
 * 「发到池满 → 抽干 CQ 腾资源 → 重试同一条」的背压闭环使用。失败时命令已整条 release
 * （沿链回收 desc/buffer + 归还命令槽），调用者拿到 NULL 即可，无需再释放。 */
fc_command_t *test_cmd_try_build(const nand_addr_t *addr, uint32_t cw_bitmap, uint32_t meta, const fc_cmd_conf_t *config);

/* ── 内联用例用的低层原语 ──
 * 需要**自己拿着命令**做事的用例（Level 3 的 READ_ID / GET_FEATURES / cache 序列 /
 * 内部搬移 / RANDOM_DATA_READ 等）走 test_cmd_build() 建链，再用这里的零件提交 /
 * 校验 / 释放。页 helper 只是把「建链 → 搬数据 → 提交 → 轮询 → 校验 → 释放」打包
 * 好；形态对不上时（如「擦除页的聚合 status 必然带 MATA_ERROR，只能在 release 前
 * 逐 CW 查 SUCCESS 位」）就用这套零件自己拼。 */

/* plane_bitmap → flat CW 位图：选中 plane 段内的全部 CW（plane 1 单 plane ⇒ 0xF0）。
 * 与 test_cmd_build 内部按 cmd->paa/config.multi_plane 反推的段互为**交叉检查**。 */
uint32_t test_plane_cw_bitmap(uint8_t plane_bitmap);

/* plane_bitmap 里序号最小的被选中 plane（test_cmd_build 的 addr.plane 用）。
 * plane_bitmap 必须非 0（不可达时返回 0）。 */
uint8_t test_first_plane(uint8_t plane_bitmap);

/* plane_bitmap 里被选中的 plane 个数（1 或 PLANE_NUM）——算「本次覆盖多少字节」用。 */
uint8_t test_plane_count(uint8_t plane_bitmap);

/* 提交 + 轮询到完成，**命令仍归调用者**（返回 CQ 完成项）。
 * 用毕调用者自己 fc_command_release()——本函数不释放。 */
fc_command_status_t test_submit_wait(fc_command_t *cmd);

/* ── 7 个页 helper（建立在 test_cmd_build 之上） ──
 * 内部 alloc / 建链 / 搬数据 / 提交 / 轮询 / 拷回 / 释放，命令不交回调用者。
 * 返回 CQ 完成项；`data` 为出参（可传 NULL），读回 / 写下的明文在 g_source_data 里。 */

fc_command_status_t test_erase_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config);

fc_command_status_t test_prog_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config, uint8_t **data);

/* meta 是本命令**声明的页 PBA**。普通读传本页 PBA（pba_from_addr(本页地址)）；
 * 内部搬移用例传**源页** PBA——目标块那页的 META 是源页写入时铸的。 */
fc_command_status_t test_read_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, uint32_t meta, const fc_cmd_conf_t *config, uint8_t **data);

/* 遍历整块：逐页调对应页 helper（页间自然轮询）。
 * test_read_block 逐页读并在**页内**校验 PAA 图案（否则 g_source_data 只剩最后一页可比）；
 * 擦除态（全 0xFF）的整块校验不由它承担——用例自己循环 test_read_page + 逐槽比 0xFFFFFFFF。 */
void test_prog_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);
void test_read_block(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);

/* ── cache 整块读写（ONFI cache + 器件 two-plane 扩展）──
 * 两层依据别混：
 *   · **ONFI 1.0 根本没有 multi-plane**——Table 14 只有单 plane 的
 *     `Read Cache` 31h / `Read Cache End` 3Fh / `Read Cache Enhanced` 00h-31h /
 *     `Page Cache Program` 80h-15h。
 *   · **本器件 MT29F4G08 有 two-plane cache 读和写**：
 *     - Figure 74 `80h-11h-80h-15h`（中间页）+ `80h-11h-80h-10h`（末页）；
 *     - Two-Plane Read Cache：`00h-00h-30h` + `31h/3Fh`，用 `06h-E0h` 选 plane 输出。
 *   单 plane helper 要求 plane_bitmap 恰好一个 plane；multi-plane helper 固定
 *   TEST_PLANE_BM_ALL。
 *
 * 单 plane 顺序：
 *   START_SEQ(p0) -> 输出 p0、data=p1；
 *   SEQ ×(N-2)    -> 输出 p1..pN-2；
 *   LAST          -> 输出 pN-1。
 *   共 N 条命令输出 N 页（START_SEQ 已含第一条 31h）。
 *
 * 双 plane 顺序：
 *   START_SEQ_MP(p0) -> 输出 pair0；
 *   SEQ_MP ×(N-2)    -> 输出 pair1..pairN-2；
 *   LAST_MP          -> 输出 pairN-1。
 *   每条输出命令内部用 `06h P0 E0h` / `06h P1 E0h` 分别选两个 plane 的 cache register。
 *
 * test_prog_block_cache：单 plane：前 N-1 页 OP_PROGRAM_PAGE_CACHE（80h-15h），
 *   末页 OP_NORMAL_PROGRAM_PAGE（10h）。
 * test_prog_block_cache_multi_plane：双 plane：前 N-1 页 OP_TWO_PLANE_PROGRAM_CACHE
 *   （80h-11h-80h-15h），末页 OP_PROGRAM_PAGE_MULTI_PLANE（80h-11h-80h-10h）。
 *   末页的 10h 是 batch 结束判据（tLPROG），不是可选的“排空”。
 *
 * 两者都是**同步**的（逐条 submit → 轮询 → 释放）⇒ cache 的流水线收益在本夹具里测不到；
 * 这里验的是**数据正确性**：cache 命令序列走得通、META / CRC / 加扰 / ECC 全程不串。 */
void test_prog_block_cache(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);
void test_read_block_cache(uint16_t block_index, uint8_t plane_bitmap, const fc_cmd_conf_t *config, uint8_t **data);
void test_prog_block_cache_multi_plane(uint16_t block_index, const fc_cmd_conf_t *config, uint8_t **data);
void test_read_block_cache_multi_plane(uint16_t block_index, const fc_cmd_conf_t *config, uint8_t **data);

/* 跨块 pair 版：两 plane 各在一个 block（Policy B 的真实形态，生产 FTL 恒如此）。
 *   与上面同块版的差别只有地址来源：行地址逐 plane 取 blocks[plane]（走
 *   ftl_cmd_setup_ppa_pair_cw，与生产同一个建链入口），故 meta 只能取整对共用的
 *   LBA 口径（一个命令只有一个 command->meta）。
 *   覆盖点：05h-E0h 的 plane 选择与 31h 预取推进在「两 plane 不同块」下是否仍按页 +1。 */
fc_command_t *test_cmd_build_pair_cross_block(const uint16_t blocks[PLANE_NUM], uint16_t page_index, uint32_t lba_meta, const fc_cmd_conf_t *config);
void          test_prog_block_cache_multi_plane_pair(const uint16_t blocks[PLANE_NUM], const fc_cmd_conf_t *config, uint8_t **data);
void          test_read_block_cache_multi_plane_pair(const uint16_t blocks[PLANE_NUM], const fc_cmd_conf_t *config);
void          test_verify_page_pair_paa(const uint8_t *data, const uint16_t blocks[PLANE_NUM], uint16_t page_index, uint8_t plane_bitmap);

/* test_read_cache_random：cache 读的「随机跳页」入口——OP_READ_PAGE_CACHE_RANDOM（00h-addr-31h）。
 *
 *   手册原文：「reads the specified block and page **into the data register** while the
 *   **previous page is output from the cache register**」⇒ 地址页只进 data register（预取），
 *   当场输出 cache 里的上一页；命令必须在 00h-30h/00h-31h 之后下发。
 *
 *   本夹具的管线（START_SEQ 已含 31h）：
 *     ① START_SEQ(prev)   输出 prev、data=prev+1
 *     ② RANDOM(addr)      输出 prev+1、data=addr
 *     ③ SEQ(31h)          输出 addr、data=addr+1
 *     ④ LAST(3Fh)         输出 addr+1
 *   prev_page / addr_page 都不得是块尾；addr 不能等于 prev+1，否则③④会重复。
 *   ② 的 meta 取 **prev+1** 的 PBA；后续按实际输出页取 meta。
 *   31h 可以跨 block，但不能跨 die/LUN（datasheet）。 */
void test_read_cache_random(uint16_t block_index, uint8_t plane_bitmap, uint16_t prev_page, uint16_t addr_page, const fc_cmd_conf_t *config);

/* ── 加载 / 取数两段式（v45） ──
 * 把生产读路径「先加载、再逐 CW 取数」拆成**两条可独立下发的 FC 命令**：
 *
 *   test_load_page        —— 纯加载：op 33（单 plane）/ op 29（多 plane），**不带 desc 链**
 *                            （op 表 has_data = 0 ⇒ 无 host 数据搬运、无 CW 位图）。器件只把
 *                            目标页搬进页缓冲，**不输出到总线**。config->operation 必须是
 *                            二者之一（其余值 ASSERT）。
 *   test_random_data_read —— 取数：op 8（单 plane，05h-E0h）/ op 41（多 plane，06h-E0h），
 *                            **带整段掩码 desc 链**。单条命令由 NAND CW 链自动遍历掩码内所有
 *                            CW（每完成一个取数 op 推进到下一个置位）。逐 CW PAA 校验。
 *
 * 两段必须**紧邻**：加载与取数是两条独立命令，中间若有别的命令碰器件页缓冲，取数会读到错页。
 * 多 plane 段（op 29/41）plane_bitmap 必须 == TEST_PLANE_BM_ALL；单 plane 段（op 33/8）必须
 * 恰一个 plane（05h 无 plane 位，06h 按行地址 plane 位选页缓冲）。 */
void test_load_page(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config);
void test_random_data_read(uint16_t block_index, uint8_t plane_bitmap, uint16_t page_index, const fc_cmd_conf_t *config);

/* ── META 只读回读（v46） ──
 * test_read_meta：op 11（OP_READ_META，自带 00h-30h 页加载 + 4 字节输出），**不带 desc 链**
 *   （op 表 has_data = 0）。回读指定页、指定 CW 的前 4 字节 META，从 CQ 的 param.bytes 取回，
 *   写入 out[4]。cw 由 pba 携带 → FC 转成列地址（paa_to_cw(plane_paa[plane]) × CW_SIZE）。
 *   单 plane（plane ∈ [0, PLANE_NUM)）。status 无错误位才继续（读失败 = 坏块信号）。 */
void test_read_meta(uint16_t block_index, uint8_t plane, uint16_t page_index, uint8_t cw, uint8_t out[4]);

/* ── 块锁族（v49） ──
 * test_block_lock_cmd —— op 21（23h）/ 22（24h）/ 23（2Ah）：**无 desc 链**（op 表 has_data = 0）、
 *   不搬 host 数据，只下发命令并断言无错误位。config->operation 必须是三者之一。
 * test_block_lock_status —— op 25（7Ah）：同无链，返回状态字节（从 CQ 的 param.bytes[0] 取）。
 *   位定义（MT29F4G08 手册）：I/O2 = Lock#（0 = 锁定，1 = 未锁）、I/O1 = LT#、I/O0 = LT（1 = 紧锁）。
 * 两者都按 (block, plane) 设 cmd->paa —— FC 的块锁族分支据此算行地址（23h/24h/7Ah 带 3 个
 * 地址周期），给错会静默打到 block 0。 */
void    test_block_lock_cmd(uint16_t block_index, uint8_t plane, const fc_cmd_conf_t *config);
uint8_t test_block_lock_status(uint16_t block_index, uint8_t plane, const fc_cmd_conf_t *config);

/* ── 数据搬运 / 校验 ── */

/* 单槽版：把 512B 槽填成 FC_BUFFER_USER_SIZE/4 个 paa。 */
void test_fill_paa(uint8_t *slot, DWORD paa);
/* 单槽版：1 = 槽内 FC_BUFFER_USER_SIZE/4 个 dword 全等 paa。 */
uint8_t test_check_paa(const uint8_t *slot, DWORD paa);

/* 活链版（内联用例用）：
 * test_fill_user_paa —— 逐 desc 填槽 + 拷进 desc 的 USER 区（写路径的「搬数据」那一步）；
 * test_memset_read_buffer —— 逐 desc 把 USER 区预填 0xCC（读路径的残留哨兵）。 */
void test_fill_user_paa(fc_command_t *cmd);
void test_memset_read_buffer(fc_command_t *cmd);

/* 读路径收尾（批量路径用）：FC 已原地解码，逐 desc 把 USER 区拷回 g_source_data 槽。
 * 页 helper（test_read_page 等）内部收尾也走它；批量读须在 release 前显式调用，
 * 之后再用 test_verify_page_paa 校验。 */
void test_capture_cmd(const fc_command_t *cmd);

/* 按 (block, page, plane_bitmap) 逐槽校验 data 里的 PAA 图案——页 helper 的读回判据。 */
void test_verify_page_paa(const uint8_t *data, uint16_t block_index, uint16_t page_index, uint8_t plane_bitmap);

/* 活链版：逐 desc 断言 USER 区全 0xFF（擦除态） / 每 CW 的 status == FC_CW_STATUS_UNUSED（成功）。 */
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

/* ── 证据打印（evidence logging）──
 *
 * 单测只报 [ OK ] 时看不出「到底证明了什么」。这三个原语把用例**断言所依据的量**
 * 打出来（页数、字节数、坐标、位图、实读到的 ID 字节……），于是绿灯本身可被核对。
 *
 * 开关复用 munit 的 `--log-visible`，**默认（error）一个字节都不输出**，报告格式不变：
 *     test --log-visible info  /fc-nand/    → 每条用例 1-3 行摘要
 *     test --log-visible debug /fc-nand/    → 再加逐页 / 逐 CW 明细
 *
 *   test_trace_begin()   —— **每个用例开头调一次**：证据档下立刻给 munit 那条没有换行的
 *                           前缀行收尾（用例体内可能先冒出项目自己的 LOGI，如 nand 驱动的
 *                           READ_ID 行）；默认档一个字节都不发。忘了调只是首行粘在补齐的
 *                           前缀后面，不会出错。
 *   test_trace_summary() —— MUNIT_LOG_INFO：这条用例证明了什么（汇总量）。
 *   test_trace_detail()  —— MUNIT_LOG_DEBUG：逐页 / 逐 CW 明细。
 *   test_result()        —— **总是输出**（不看 `--log-visible`）：基准类用例的关键指标
 *                           （`/speed` 的读/写 KB/s）。它属于"测试结果"而不是诊断 ——
 *                           默认档只报 `[ OK ]` 看不到速度并不合理。
 *
 * ⚠ 消息自带换行与 `[EV] ` 前缀，**调用者不要自己加 \n**。
 * ⚠ 该 fork 关掉了 fork、无 stderr 捕获（munit.c），故用例内的输出立刻到 UART。
 * ⚠ 证据打印走**阻塞式** UART（uart_log_write 环满自旋）⇒ 开了 debug 档后
 *   `[ N ms ]` 的计时含 TX 时间，不可再当性能基线。
 * ⚠ 整套跑 debug 时输出 ≈ 290 KB，链路（115200 8N1 ≈ 11.5 KB/s）被打满 ⇒ 整轮 25 s
 *   几乎全是 TX。此时**上位机日志里会出现周期性折行**（约每 4 KB 一次，实测 0/1/80 次
 *   对应 default/info/debug 三档）：**内容不丢**，被折断的行在下一行续完。读日志时别
 *   把这种折行当成截断；要看明细请单跑一条用例
 *   （`test --log-visible debug /fc-nand/lifecycle/block_write_read`）。 */

void test_trace_begin(void);

MUNIT_PRINTF(1, 2)
void test_trace_summary(const char *format, ...);

MUNIT_PRINTF(1, 2)
void test_trace_detail(const char *format, ...);

/* 关键指标行：无论 --log-visible 取值都输出（前缀仍是 "[EV] "）。 */
MUNIT_PRINTF(1, 2)
void test_result(const char *format, ...);

#endif /* FC_TEST_COMMON_H */
