/*
 * fc_test_common.c — FC-NAND 单元测试共享夹具（实现）
 *
 * 接口与契约见 fc_test_common.h。本文件只放「测试脚手架」，不含被测逻辑。
 */

#include "fc_test_common.h"

#include "fault_inject.h"
#include "fc_nand_dispatch.h"
#include "fc_nand_engine.h"
#include "fc_random_engine.h"
#include "fi_wrap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test_fill_paa / test_check_paa 按 **dword** 步进（`FC_BUFFER_USER_SIZE / 4U` 次 ×
 * `sizeof(DWORD)` 字节）：只有 PAA 正好 4 字节、且 USER 区是 4 的整数倍，
 * 「128 个 dword 填满 512B 槽」才成立。两者任一漂移都会**静默**写穿 / 漏填槽。 */
_Static_assert(sizeof(DWORD) == sizeof(DWORD), "DWORD 非 4 字节：dword 步进填槽失效");
_Static_assert((FC_BUFFER_USER_SIZE % sizeof(DWORD)) == 0U, "USER 区非 4 的整数倍");

BYTE *g_source_data = NULL;

/* 步骤级用例的 CW 暂存区（见 fc_test_common.h「test_cw_scratch」）。
 * 与 g_source_data 同处分配 / 释放；**不放栈上**（4KB+ 会撑爆用例栈帧）。 */
static BYTE *g_cw_scratch = NULL;

/*************************************************************************
 *  证据打印（接口与用法见 fc_test_common.h 末节）
 **************************************************************************/

/* 本次用例是否已经与 munit 的结果行断过行。
 * munit 打印参数前缀（"    block=100, plane=0"）时**不带换行**并补齐到
 * MUNIT_TEST_NAME_LEN(55)，随后才跑用例、最后才打 "[ OK ]"
 * （munit.c:900-926 打印前缀，:935-982 打印结果）⇒ 用例体内的第一行输出会插进
 * 结果行中间——崩溃时看到的就是 "    block=100, plane=0   [ERR] assertion failed"。
 * 由 test_trace_begin() 在每个用例开头复位。 */
static BOOL s_ev_line_started = 0U;

/** 交叉块 pair 读回校验（定义在文件后段，test_read_block_cache_multi_plane_pair 先行调用）。 */
static void test_verify_page_pair_paa(const BYTE *data, const WORD blocks[PLANE_NUM], WORD page_index, BYTE plane_bitmap);

void test_trace_begin(void)
{
    /* 证据档下**立刻**给 munit 那条没有换行的前缀行收尾，而不是等第一条证据行。
     * 原因：用例体内还有别的写者——项目自己的日志会抢先输出（如 nand 驱动的
     * "READ_ID data=2C DC 90 95 56"，见 test_init_sequence；那条是 LOGD，
     * 只在 debug 档可见，但仍会与第一条 [EV] 争行），它排在第一条 [EV] 之前，
     * 于是会粘在结果行后面。这里先断行，那种行就落在自己的一行上。
     *
     * 判据用 INFO：DEBUG 可见 ⇒ INFO 也可见（阈值比较），所以这一条覆盖 info/debug 两档。
     * 默认档（visible = error）恒为假 ⇒ 一个字节都不输出，报告格式保持原样。 */
    s_ev_line_started = munit_log_visible(MUNIT_LOG_INFO);

    if (s_ev_line_started)
    {
        fputc('\n', MUNIT_OUTPUT_FILE);
        (void)fflush(MUNIT_OUTPUT_FILE);
    }
}

static void test_trace_emit(BOOL force, MunitLogLevel level, const char *tag, const char *format, va_list ap)
{
    /* 可见性判断放在**最前面**：默认档（visible = error）连下面那个 '\n' 都不输出，
     * 于是报告格式保持原样。force = 关键指标行（/bench/speed 的速度）：属于"结果"，
     * 不受档位过滤。 */
    if ((force == 0U) && !munit_log_visible(level))
    {
        return;
    }

    if (!s_ev_line_started)
    {
        fputc('\n', MUNIT_OUTPUT_FILE); /* 给 munit 那条没有换行的前缀行收尾 */
        s_ev_line_started = 1U;
    }

    fputs(tag, MUNIT_OUTPUT_FILE);
    (void)vfprintf(MUNIT_OUTPUT_FILE, format, ap);
    fputc('\n', MUNIT_OUTPUT_FILE);
    (void)fflush(MUNIT_OUTPUT_FILE); /* 立刻出队，保证排在 [ OK ] 之前 */
}

void test_trace_summary(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    test_trace_emit(0U, MUNIT_LOG_INFO, "[EV] ", format, ap);
    va_end(ap);
}

void test_trace_detail(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    test_trace_emit(0U, MUNIT_LOG_DEBUG, "[EV]   ", format, ap);
    va_end(ap);
}

void test_result(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    test_trace_emit(1U, MUNIT_LOG_INFO, "[EV] ", format, ap);
    va_end(ap);
}

/*************************************************************************
 *  内部小工具
 **************************************************************************/

/** plane_bitmap 中最低置位的 plane 号（调用方已断言非 0） */
BYTE test_first_plane(BYTE plane_bitmap)
{
    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(p)) != 0U)
        {
            return p;
        }
    }
    return 0U; /* 不可达 */
}

BYTE test_plane_count(BYTE plane_bitmap)
{
    BYTE count = 0U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(p)) != 0U)
        {
            count++;
        }
    }
    return count;
}

/** (block, plane_bitmap 的首个 plane, page) → 页 PBA。
 * 只需 meta（页 PBA）、不需要整条 nand_addr_s 的调用点用它，避免在每个调用点
 * 手写一次「memset + 三个字段」。 */
static DWORD test_page_pba(WORD block_index, BYTE plane_bitmap, WORD page_index)
{
    nand_addr_s addr;

    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);
    addr.page  = page_index;

    return pba_from_addr(&addr);
}

/**
 * @brief plane_bitmap → flat 绝对 CW 位图：选中 plane 段内的全部 CW。
 * @param plane_bitmap 非 0、不含段外位的 plane 位图。
 * @return flat 位图（plane 1 单 plane ⇒ 0xF0，两 plane ⇒ 0xFF）。
 * @note 刻意不复用 fc_command_plane_span_cw_bitmap(cmd)：那份从 cmd->paa 的 plane 位与
 *       config.multi_plane **反推**段，这份从 plane_bitmap **正推**段；两条独立推导必须
 *       一致，不一致时 ftl_cmd_resolve_cw_bitmap 的越段断言当场炸，而不是静默访问错的 plane。
 */
DWORD test_plane_cw_bitmap(BYTE plane_bitmap)
{
    DWORD mask = 0U;

    for (BYTE p = 0U; p < PLANE_NUM; p++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(p)) != 0U)
        {
            mask |= ((DWORD)((1U << CW_NUM) - 1U)) << ((DWORD)p * CW_NUM);
        }
    }
    return mask;
}

/** 该 op 是否是多 plane 变体——决定 config.multi_plane（= 命令 plane 段宽度）。
 * 全仓没有这份分类（FTL 是内联 `pages == 1 ? single : multi` 挑的），故此处自带一份。 */
static BYTE test_is_multi_op(BYTE operation)
{
    switch (operation)
    {
        case OP_PROGRAM_PAGE_MULTI_PLANE:
        case OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT:
        case OP_ERASE_BLOCK_MULTI_PLANE:
        case OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT: /* 多 plane 加载 */
        case OP_RANDOM_DATA_READ_MULTI_PLANE:           /* 06h-E0h 多 plane 取数 */
        case OP_TWO_PLANE_PROGRAM_CACHE:                /* Figure 74：双 plane cache 写 */
        case OP_TWO_PLANE_PROGRAM_CACHE_END:            /* Figure 74：末页 10h 收尾 */
        case OP_TWO_PLANE_PROGRAM_CACHE_FIRST:          /* Figure 74：首页（只查 FAIL） */
        case OP_READ_PAGE_CACHE_START_SEQ_MP:           /* 双 plane cache 读首 pair */
        case OP_READ_PAGE_CACHE_SEQ_MP:                 /* 双 plane cache 读顺序 pair */
        case OP_READ_PAGE_CACHE_LAST_MP:                /* 双 plane cache 读末 pair */
            return 1U;
        default:
            return 0U;
    }
}

/** submit + fc_service 循环 + poll + 校验 command_id。**不 release**。
 * 5 个 helper 内部收尾用；不对外导出（用例的提交原语见头文件说明）。 */
fc_cmd_status_s test_submit_wait(fc_cmd_s *cmd)
{
    fc_cmd_status_s completion = {0};
    fc_cmd_id_t     expect_id  = FC_CMD_ID_INVALID;
    BYTE            completed  = 0U;

    munit_assert_not_null(cmd);
    munit_assert_int(fc_submit(cmd->cmd_id), ==, FC_STATUS_OK);

    for (DWORD i = 0U; i < TEST_SERVICE_LOOP_MAX; i++)
    {
        (void)fc_service();

        if (fc_poll_completion(&completion) == FC_STATUS_OK)
        {
            completed = 1U;
            break;
        }
    }

    munit_assert_uint32(completed, ==, 1U);

    /* 命令 ID 由命令对象携带；completion 里的是建链时算好的快照。 */
    expect_id = cmd->cmd_id;
    munit_assert_uint32(completion.cmd_id, ==, expect_id);

    return completion;
}

/** 读路径收尾：FC 已在 fc_complete_from_nand 里跑过 process_read（原地解码），
 * 此刻 desc 的 USER 区是明文——逐 desc 拷回 g_source_data 槽，供调用者校验。
 * 导出供**批量**路径使用：批量读要「先捕获、再校验、最后才 release」，
 * 必须在提交-抽取循环里单独调用它（页 helper 内部收尾也走这里）。 */
void test_capture_cmd(const fc_cmd_s *cmd)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    BYTE         idx     = 0U;

    while ((desc_id != FC_DESC_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        BYTE *slot = test_source_slot(paa_to_plane(desc->paa), paa_to_cw(desc->paa));
        BYTE *user = fc_buffer_get_user_data(desc->buffer_id);
        munit_assert_not_null(user);

        memcpy(slot, user, FC_BUFFER_USER_SIZE);

        desc_id = (fc_desc_id_t)desc->next_id;
        idx++;
    }
}

/**************************************************************************
 *  setup / teardown
 **************************************************************************/

void *test_setup(const MunitParameter params[], void *user_data)
{
    /* fc_init() 只在第一次调用时执行：本 fork 的 MunitSuite 没有 setup 字段，
     * 逐用例挂 setup 就退化成「每用例一次」，而 fc_init 会重置全部资源池——
     * 每用例重初始化会把「上一个用例漏还的命令/desc/buffer」洗掉，掩盖泄漏。 */
    static BYTE inited = 0U;

    (void)params;
    (void)user_data;

    if (inited == 0U)
    {
        fc_init();
        inited = 1U;
    }

    /* 注入状态是进程级的：每个 FC 用例都从干净状态开始（D2 seam 会武装
     * completion drop/late 等跨 poll 状态，漏复位会静默污染下一条用例）。 */
    fi_reset();
    fi_wrap_reset();

    /* 异步 dispatch 放行闩锁（main.c 在 NAND 初始化后已置 1，此处幂等重申）。
     * 本套件走 fc_service() → nand_fc_start/step 异步路径，闩锁必须为 1。 */
    fc_nand_engine_set_armed(1U);

    if (g_source_data == NULL)
    {
        const DWORD slots = CW_NUM * PLANE_NUM;

        g_source_data = (BYTE *)malloc((size_t)FC_BUFFER_USER_SIZE * slots);
        munit_assert_not_null(g_source_data);

        /* 内容**不在这里填**：槽内容 = 各 CW 自己的 PAA，只有建链之后才知道。
         * 写路径（test_fill_user_paa）负责填。此处只清零，让「没写过」可辨认。 */
        memset(g_source_data, 0x00, (size_t)FC_BUFFER_USER_SIZE * slots);
    }

    if (g_cw_scratch == NULL)
    {
        g_cw_scratch = (BYTE *)malloc((size_t)CW_TOTAL_MAX * FC_BUFFER_POOL_CW_SIZE);
        munit_assert_not_null(g_cw_scratch);

        /* 同样不预填：步骤级用例自己决定写什么（未写处应可见残留） */
        memset(g_cw_scratch, 0x00, (size_t)CW_TOTAL_MAX * FC_BUFFER_POOL_CW_SIZE);
    }

    return NULL;
}

void fc_ut_assert_clean(void)
{
    fc_resource_stats_s rs;

    memset(&rs, 0, sizeof(rs));
    fc_get_resource_stats(&rs);

    munit_assert_uint16(rs.sq_used, ==, 0U);
    munit_assert_uint16(rs.hsq_used, ==, 0U);
    munit_assert_uint16(rs.cq_used, ==, 0U);
    munit_assert_uint16(rs.command_used, ==, 0U);
    munit_assert_uint16(rs.descriptor_used, ==, 0U);
    munit_assert_uint16(rs.buffer_used, ==, 0U);
}

void test_tear_down(void *fixture)
{
    (void)fixture;

    /* 每条用例结束时资源池必须归零；漏 release 在这里当场暴露，
     * 不留给下一条用例继承脏池。 */
    fc_ut_assert_clean();

    if (g_source_data != NULL)
    {
        free(g_source_data);
        g_source_data = NULL;
    }

    if (g_cw_scratch != NULL)
    {
        free(g_cw_scratch);
        g_cw_scratch = NULL;
    }
}

/**
 * @brief 取步骤级 CW 暂存区的第 idx 槽（FC_BUFFER_POOL_CW_SIZE 字节）。
 * @param idx 槽下标，必须 < CW_TOTAL_MAX。
 * @return 该槽基址。
 * @note 内容不预填，由用例自己负责；槽在 setup 分配、teardown 释放，不能放用例栈上。
 */
BYTE *test_cw_scratch(BYTE idx)
{
    ASSERT(g_cw_scratch != NULL);
    ASSERT(idx < CW_TOTAL_MAX);

    return &g_cw_scratch[(DWORD)idx * FC_BUFFER_POOL_CW_SIZE];
}

/*************************************************************************
 *  只建链、不提交的原语
 **************************************************************************/

/**
 * @brief 建一条命令 + desc 链但不提交；资源不足时返回 NULL。
 * @param addr 命令地址（ch/ce/lun/block/plane/page；cw 必须为 0）。
 * @param cw_bitmap 字面 CW 位图；无链 op 传 0（ftl_cmd_needs_desc_chain 短路）。
 * @param meta 写进 command->meta（页 PBA，或跨块 pair 的整对共用 LBA）。
 * @param config 模式位；multi_plane 由 operation 派生后覆盖。
 * @return 建好的命令；命令 / desc / buffer 池空返回 NULL（已挂资源随 release 回收）。
 * @note 配置错误（坐标越界 / 掩码漏 plane）是 ASSERT，不返回 NULL——那是调用者 bug。
 *       压力用例靠 NULL 做背压闭环：发到池满 → 抽干 CQ 腾资源 → 重试同一条。
 */
fc_cmd_s *test_cmd_try_build(const nand_addr_s *addr, DWORD cw_bitmap, DWORD meta, const fc_cmd_cfg_s *config)
{
    fc_cmd_s *cmd;
    BYTE      is_multi;

    ASSERT(addr != NULL);
    ASSERT(config != NULL);
    ASSERT(addr->block < BLOCKS_PER_PLANE);
    ASSERT(addr->plane < PLANE_NUM);
    ASSERT(addr->page < PAGES_PER_BLOCK);
    /* 链上每条 desc 的 CW 由 cw_bitmap 逐位铸出；行地址的 CW 位在建链时被
     * `paa_with_plane(cmd->paa, plane)` 保留但 desc 的 cw 由实参覆盖（ftl_fc_interface.c）
     * ⇒ 地址里带 cw 只会让调用者以为它有效。断言 0 把这个误解挡在建链之前。
     * **例外**：OP_READ_META 无 desc 链，其 cw 就是列选择器（paa.cw → 列地址），
     * 必须允许携带。 */
    ASSERT((addr->cw == 0U) || (config->operation == OP_READ_META));

    is_multi = test_is_multi_op((BYTE)config->operation);

    /* multi-plane 命令的位图必须**每个 plane 都被覆盖到**：未覆盖的 plane 会被
     * fc_build_request_data_planes 补一条同行回退地址，写路径静默给该 plane 编一页
     * 全 0xFF（见 fc_test_common.h「三」）。cw_bitmap == 0 是无链命令（ERASE 等），跳过。 */
    if ((is_multi != 0U) && (cw_bitmap != 0U))
    {
        for (BYTE p = 0U; p < PLANE_NUM; p++)
        {
            ASSERT((cw_bitmap & (((DWORD)((1U << CW_NUM) - 1U)) << ((DWORD)p * CW_NUM))) != 0U);
        }
    }

    cmd = fc_command_alloc();
    if (cmd == NULL)
    {
        return NULL; /* 命令池空 */
    }

    /* 照 ftl_cmd_setup_ppa 的口径：地址信息既进 paa，也镜像进 ch/ce/lun。
     * FTL 侧边界会 pba_to_paa 后赋给 cmd->paa，本夹具照抄（paa_from_addr 忽略
     * ch/ce——上面三行已把 ch/ce/lun 镜像进 config，无拓扑损失）。 */
    cmd->config             = *config;
    cmd->ch                 = addr->ch;
    cmd->ce                 = addr->ce;
    cmd->lun                = addr->lun;
    cmd->config.multi_plane = (is_multi != 0U) ? PLANE_NUM : 0U;
    cmd->paa                = paa_from_addr(addr);
    cmd->meta               = meta;

    /* 无数据 op 走到 ftl_cmd_needs_desc_chain 的短路分支（cw_bitmap 不看），
     * desc_id 保持 fc_command_alloc 置的 INVALID。 */
    if (ftl_cmd_configure_descriptors(cmd, cw_bitmap) != FTL_OK)
    {
        /* 建链中途失败（desc / buffer 池空）：不就地回滚，整条 release 沿链回收
         * 已挂的 desc/buffer + 归还命令槽，再向上报「建不出来」。 */
        fc_command_release(cmd->cmd_id);
        return NULL;
    }

    return cmd;
}

/**
 * @brief test_cmd_try_build 的断言版：资源不足直接 abort 用例。
 * @param addr 命令地址（cw 必须为 0）。
 * @param cw_bitmap 字面 CW 位图；无链 op 传 0。
 * @param meta command->meta。
 * @param config 模式位；multi_plane 由 operation 派生后覆盖。
 * @return 建好的**未提交**命令；永不返回 NULL。
 */
fc_cmd_s *test_cmd_build(const nand_addr_s *addr, DWORD cw_bitmap, DWORD meta, const fc_cmd_cfg_s *config)
{
    fc_cmd_s *cmd = test_cmd_try_build(addr, cw_bitmap, meta, config);

    munit_assert_not_null(cmd);
    return cmd;
}

/**************************************************************************
 *  5 个页 helper（建立在 test_cmd_build 之上）
 **************************************************************************/

fc_cmd_status_s test_erase_block(WORD block_index, BYTE plane_bitmap, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    BYTE            is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U); /* 不含段外 plane */
    ASSERT(block_index < BLOCKS_PER_PLANE);
    ASSERT((config->operation == OP_ERASE_BLOCK) || (config->operation == OP_ERASE_BLOCK_MULTI_PLANE));

    is_multi = test_is_multi_op((BYTE)config->operation);
    if (is_multi != 0U)
    {
        /* multi-plane 擦除按定义就擦两个 plane（fc_build_dispatch_request 的
         * OP_ERASE_BLOCK_MULTI_PLANE 分支无条件填 plane_paa[0]/[1]），半掩码
         * 表达不出别的意思 ⇒ 自洽性检查。 */
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* 擦除**无数据传输** ⇒ cw_bitmap = 0：test_cmd_build 走到 ftl_cmd_needs_desc_chain
     * 的短路分支，desc_id 保持 INVALID，不建链、不申请 buffer。meta 同理不参与
     * （无数据路径 ⇒ 无 encode / 无 Pass 4）。
     * 请求位图在 NAND 侧是**合法空集**：FC 的 ERASE 分支只填 plane_paa[]、
     * 不伪造 CW0 位，completion_all_cws 的空集分支写 cw_status[0] 兜住成败。
     * multi_plane 由 test_cmd_build 按 operation 派生——它是「本命令跨几个 plane」的
     * 真实属性，填它不为任何读者，只为让命令的声明几何不撒谎。 */
    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);

    cmd = test_cmd_build(&addr, 0U, 0U, config);

    completion = test_submit_wait(cmd);

    fc_command_release(cmd->cmd_id);
    return completion;
}

fc_cmd_status_s test_prog_page(WORD block_index, BYTE plane_bitmap, WORD page_index, const fc_cmd_cfg_s *config, BYTE **data)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    BYTE            is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    ASSERT((block_index < BLOCKS_PER_PLANE) && (page_index < PAGES_PER_BLOCK));
    ASSERT((config->operation == OP_NORMAL_PROGRAM_PAGE) || (config->operation == OP_PROGRAM_PAGE_MULTI_PLANE) || (config->operation == OP_PROGRAM_PAGE_CACHE) || (config->operation == OP_PROGRAM_PAGE_CACHE_FIRST) || (config->operation == OP_TWO_PLANE_PROGRAM_CACHE) || (config->operation == OP_TWO_PLANE_PROGRAM_CACHE_END) || (config->operation == OP_TWO_PLANE_PROGRAM_CACHE_FIRST));

    is_multi = test_is_multi_op((BYTE)config->operation);
    if (is_multi != 0U)
    {
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* meta = 本页 PBA：写读同页自然一致，且**页地址整体进了 META** ⇒「读错页」被 Pass 4 ② 抓到。
     * cw_bitmap 由 plane_bitmap 正推（test_plane_cw_bitmap），与 test_cmd_build 内部
     * 按 cmd->paa/config.multi_plane 的段推导互为交叉检查。 */
    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);
    addr.page  = page_index;

    cmd = test_cmd_build(&addr, test_plane_cw_bitmap(plane_bitmap), pba_from_addr(&addr), config);

    /* 明文 PAA 图案：填 g_source_data 槽，同时拷进各 desc 的 USER 区。
     * 提交后 FC 会原地编码，desc buffer 不可读作明文 ⇒ `*data` 只能指向槽。 */
    test_fill_user_paa(cmd);

    completion = test_submit_wait(cmd);

    if (data != NULL)
    {
        *data = g_source_data;
    }

    /* 证据明细（--log-visible debug）：这条命令实际写了哪一页 / 几个 CW / 什么状态。 */
    test_trace_detail("prog p%02hu PBA=0x%08lX bm=0x%02hhX cw=%hhu status=0x%hhX", page_index, pba_from_addr(&addr), plane_bitmap, test_cw_count(cmd), completion.status);

    fc_command_release(cmd->cmd_id);
    return completion;
}

fc_cmd_status_s test_read_page(WORD block_index, BYTE plane_bitmap, WORD page_index, DWORD meta, const fc_cmd_cfg_s *config, BYTE **data)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    BYTE            is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    ASSERT((block_index < BLOCKS_PER_PLANE) && (page_index < PAGES_PER_BLOCK));
    ASSERT((config->operation == OP_NORMAL_READ_PAGE_WITH_DATA_OUT) || (config->operation == OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT));

    is_multi = test_is_multi_op((BYTE)config->operation);
    if (is_multi != 0U)
    {
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* cw_bitmap 与 test_prog_page 同口径。meta 由调用者给：普通读传**本页 PBA**
     * （与写路径一致，否则 Pass 4 ② 报错）；内部搬移用例要传**源页 PBA**——那页的
     * META 是源页写入时铸的、随数据一起搬了过来（详见 test_fc_lifecycle.c 的搬移用例）。 */
    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);
    addr.page  = page_index;

    cmd = test_cmd_build(&addr, test_plane_cw_bitmap(plane_bitmap), meta, config);

    /* 读前 USER 区预填 0xCC：NAND 没填满时残留可见，便于区分「读失败」与「读到真数据」。 */
    test_memset_read_buffer(cmd);

    completion = test_submit_wait(cmd);

    /* FC 已跑过 process_read（原地解码），此刻 desc 的 USER 区是明文。 */
    test_capture_cmd(cmd);

    if (data != NULL)
    {
        *data = g_source_data;
    }

    /* 证据明细（--log-visible debug）：打**本命令声明的 meta**（普通读 = 本页 PBA，
     * 内部搬移 = 源页 PBA），它是 Pass 4 ② 比对 META 的基准。 */
    test_trace_detail("read p%02hu declared_meta=0x%08lX bm=0x%02hhX cw=%hhu status=0x%hhX", page_index, meta, plane_bitmap, test_cw_count(cmd), completion.status);

    fc_command_release(cmd->cmd_id);
    return completion;
}

void test_prog_block(WORD block_index, BYTE plane_bitmap, const fc_cmd_cfg_s *config, BYTE **data)
{
    /* 逐页写，页间自然轮询（页 helper 内部即 submit + service 轮询 + release）。 */
    for (WORD page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_cmd_status_s completion = test_prog_page(block_index, plane_bitmap, page, config, NULL);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    }

    /* 证据摘要（--log-visible info）：证明「真的写了这么多」。 */
    const WORD cw_per_page = (test_plane_count(plane_bitmap) * CW_NUM);

    test_trace_summary("prog block=%hu bm=0x%02hhX: %d pages x %hu CW x %u B", block_index, plane_bitmap, PAGES_PER_BLOCK, cw_per_page, FC_BUFFER_USER_SIZE);

    if (data != NULL)
    {
        *data = g_source_data; /* 最后一页的明文 */
    }
}

void test_read_block(WORD block_index, BYTE plane_bitmap, const fc_cmd_cfg_s *config, BYTE **data)
{
    /* 逐页读，页间自然轮询。
     * **必须在页内校验**：g_source_data 每页都被覆写，攒到最后只剩最后一页可比。 */
    for (WORD page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        /* 普通读：meta 取本页 PBA（与 test_prog_page 的写口径一致）。 */
        DWORD           meta       = test_page_pba(block_index, plane_bitmap, page);
        fc_cmd_status_s completion = test_read_page(block_index, plane_bitmap, page, meta, config, data);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, page, plane_bitmap);
    }

    /* 证据摘要（--log-visible info）：页数 + **逐槽比过的字节数**——这是「读回真的
     * 与写入一致」的实证（判据是每个 CW 的 USER 区等于该 CW 自己的 PAA）。 */
    const DWORD user_bytes = (DWORD)PAGES_PER_BLOCK * test_plane_count(plane_bitmap) * CW_NUM * FC_BUFFER_USER_SIZE;

    test_trace_summary("read block=%hu bm=0x%02hhX: %d pages PAA-verified, %lu B compared", block_index, plane_bitmap, PAGES_PER_BLOCK, user_bytes);

    if (data != NULL)
    {
        *data = g_source_data; /* 最后一页的明文 */
    }
}

/**************************************************************************
 *  cache 整块读写（ONFI cache 序列）
 **************************************************************************/

void test_prog_block_cache(WORD block_index, BYTE plane_bitmap, const fc_cmd_cfg_s *config, BYTE **data)
{
    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    /* 本 helper 是单 plane cache 编程；ONFI 1.0 无 multi-plane，器件的 two-plane
     * 变体见 test_prog_block_cache_multi_plane()。不靠 test_prog_page 的 else 分支兜底：
     * 「非 multi op 只能单 plane」与「本 helper 只做单 plane」是两条不同的约束。 */
    ASSERT(test_plane_count(plane_bitmap) == 1U);
    ASSERT(block_index < BLOCKS_PER_PLANE);

    /* 前 PAGES_PER_BLOCK-1 页走 cache 编程（80h-addr-data-15h）：15h 确认后设备把 cache 寄存器里
     * 的数据编入阵列、同时腾出 cache 寄存器接下一页。末页必须用普通编程（10h）收尾——15h 编的是
     * 「当前 cache 内容」，没有末页的 10h 就没有最后一页真正落盘。
     * 首页用 OP_PROGRAM_PAGE_CACHE_FIRST：本批还没有上一页，只查 FAIL；若用中间页 op，
     * 上一批残留的 FAILC 会把好页误判失败。
     * 每页仍由 test_prog_page 提交 + 轮询到完成 ⇒ 命令间完全串行（cache 的流水线收益测不到，
     * 这里验的是 cache 命令序列走得通、编码管线全程不串）。meta = 本页 PBA，与普通写同口径。 */
    for (WORD page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_cmd_cfg_s    conf = *config;
        fc_cmd_status_s completion;

        conf.operation = (page == 0U) ? OP_PROGRAM_PAGE_CACHE_FIRST : ((page < (PAGES_PER_BLOCK - 1U)) ? OP_PROGRAM_PAGE_CACHE : OP_NORMAL_PROGRAM_PAGE);

        completion = test_prog_page(block_index, plane_bitmap, page, &conf, NULL);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    }

    /* 证据摘要（--log-visible info）：证明 cache 序列真的走了这么多条命令。 */
    test_trace_summary("cache prog block=%hu bm=0x%02hhX: %u x 80h-15h + 1 x 10h = %d pages", block_index, plane_bitmap, (PAGES_PER_BLOCK - 1U), PAGES_PER_BLOCK);

    if (data != NULL)
    {
        *data = g_source_data; /* 最后一页的明文 */
    }
}

void test_prog_block_cache_multi_plane(WORD block_index, const fc_cmd_cfg_s *config, BYTE **data)
{
    fc_cmd_cfg_s conf;

    ASSERT(config != NULL);
    ASSERT(block_index < BLOCKS_PER_PLANE);

    /* 前 PAGES_PER_BLOCK-1 页：Two-Plane Program Page Cache（80h-11h-80h-15h）；
     * 末页：OP_TWO_PLANE_PROGRAM_CACHE_END（80h-11h-80h-10h + FAILC 检查），
     * tLPROG 等整批完成。 */
    for (WORD page = 0U; page < (PAGES_PER_BLOCK - 1U); page++)
    {
        fc_cmd_status_s completion;

        conf           = *config;
        conf.operation = (page == 0U) ? OP_TWO_PLANE_PROGRAM_CACHE_FIRST : OP_TWO_PLANE_PROGRAM_CACHE;

        completion = test_prog_page(block_index, TEST_PLANE_BM_ALL, page, &conf, NULL);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    }

    {
        fc_cmd_status_s completion;

        conf           = *config;
        conf.operation = OP_TWO_PLANE_PROGRAM_CACHE_END;

        completion = test_prog_page(block_index, TEST_PLANE_BM_ALL, (PAGES_PER_BLOCK - 1U), &conf, NULL);
        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    }

    test_trace_summary("multi-plane cache prog block=%hu: %u x 80h-11h-80h-15h + 1 x 80h-11h-80h-10h = %d pairs", block_index, (PAGES_PER_BLOCK - 1U), PAGES_PER_BLOCK);

    if (data != NULL)
    {
        *data = g_source_data;
    }
}

/** cache 读的一步（SEQ/SEQ_MP/LAST/LAST_MP）：建链 → 预填 0xCC → 提交 → 轮询 →
 * 拷回 → 释放。output_page 是**本步真正输出的页**（单/双 plane 同页号）；
 * 31h/3Fh 本身无地址周期，但 desc 的 PAA 行地址要取 output_page：双 plane 的
 * 06h-E0h 要按行地址里的 plane 位选 cache register，页地址也保持与实际输出页一致。
 * meta 仍取 output_page 的 PBA（真正输出的那页）。
 * plane_bitmap == TEST_PLANE_BM_ALL 时 test_cmd_build 按 multi-plane op 展开两 plane；
 * scatter 由 NAND 侧按 plane*PAGE_SIZE 完成。 */
static fc_cmd_status_s test_read_cache_step(WORD block_index, BYTE plane_bitmap, WORD output_page, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;

    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);
    addr.page  = output_page;

    cmd = test_cmd_build(&addr, test_plane_cw_bitmap(plane_bitmap), pba_from_addr(&addr), config);

    test_memset_read_buffer(cmd);

    completion = test_submit_wait(cmd);

    /* FC 已跑过 process_read（原地解码），此刻 desc 的 USER 区是明文。 */
    test_capture_cmd(cmd);

    /* 证据明细（--log-visible debug）：命令地址页号恒为 0（31h/3Fh 无地址周期），
     * 真正输出的是 output_page——打出来才看得出「地址与 meta 刻意解耦」这件事。 */
    test_trace_detail("cache out=p%02hu declared_meta=0x%08lX bm=0x%02hhX cw=%hhu status=0x%hhX", output_page, pba_from_addr(&addr), plane_bitmap, test_cw_count(cmd), completion.status);

    fc_command_release(cmd->cmd_id);
    return completion;
}

/** cache 读的启动 + 第一页输出（START_SEQ / START_SEQ_MP）：
 * 00h-addr-30h → tR → 31h → tRCBSY → DATA_OUT（双 plane 再补 06h P1 + DATA_OUT）。
 * 输出页 == 下发地址页；后续页由 SEQ/SEQ_MP 输出，末页由 LAST/LAST_MP 输出。
 * meta 取**本页** PBA。 */
static fc_cmd_status_s test_cache_start_seq(WORD block_index, BYTE plane_bitmap, WORD page, DWORD operation, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_cfg_s    conf;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;

    memset(&addr, 0, sizeof(addr));
    addr.block     = block_index;
    addr.plane     = test_first_plane(plane_bitmap);
    addr.page      = page;
    conf           = *config;
    conf.operation = operation;

    cmd = test_cmd_build(&addr, test_plane_cw_bitmap(plane_bitmap), pba_from_addr(&addr), &conf);

    test_memset_read_buffer(cmd);

    completion = test_submit_wait(cmd);

    test_capture_cmd(cmd);

    test_trace_detail("cache start p%02hu op=%lu declared_meta=0x%08lX bm=0x%02hhX cw=%hhu status=0x%hhX", page, operation, pba_from_addr(&addr), plane_bitmap, test_cw_count(cmd), completion.status);

    fc_command_release(cmd->cmd_id);
    return completion;
}

/** 「输出页 == 声明的 meta」的自描述断言。
 *
 * cache 读的每一步，命令地址与**真正输出的页**都可以不是一回事（SEQ/LAST 无地址周期、
 * RANDOM 输出的是上一页）。单看 status 只能知道「META 对不上」，看不出**实际读回了哪一页**。
 * desc->meta 在 Pass 4 被写成从介质读回的 META（= 输出页的 LBA）⇒ 直接拿它跟期望值比：
 * 失败信息里 munit 会把 actual/expected 两个数都打出来，「输出的是哪一页」一目了然。 */
void test_read_block_cache(WORD block_index, BYTE plane_bitmap, const fc_cmd_cfg_s *config, BYTE **data)
{
    fc_cmd_cfg_s    conf;
    fc_cmd_status_s completion;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    /* 单 plane cache 读：段必须恰好一个 plane。 */
    ASSERT(test_plane_count(plane_bitmap) == 1U);
    ASSERT(block_index < BLOCKS_PER_PLANE);

    /* ① START_SEQ（00h-addr-30h + 31h）：输出 page 0，并预取 page 1。 */
    completion = test_cache_start_seq(block_index, plane_bitmap, 0U, OP_READ_PAGE_CACHE_START_SEQ, config);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    test_verify_page_paa(g_source_data, block_index, 0U, plane_bitmap);

    /* ② SEQ（31h）：第 i 次输出 page i、预取 page i+1；
     *    最后一次 SEQ 输出 page PAGES-2、预取 page PAGES-1，留给 LAST。 */
    conf           = *config;
    conf.operation = OP_READ_PAGE_CACHE_SEQ;

    for (WORD page = 1U; page < (PAGES_PER_BLOCK - 1U); page++)
    {
        completion = test_read_cache_step(block_index, plane_bitmap, page, &conf);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, page, plane_bitmap);
    }

    /* ③ LAST（3Fh）：输出 data register 里的最后一页（PAGES_PER_BLOCK-1）。 */
    {
        const WORD last_page = (PAGES_PER_BLOCK - 1U);

        conf.operation = OP_READ_PAGE_CACHE_LAST;

        completion = test_read_cache_step(block_index, plane_bitmap, last_page, &conf);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, last_page, plane_bitmap);
    }

    /* 证据摘要（--log-visible info）：START_SEQ + (N-2) 次 SEQ + LAST 共 N 条命令。 */
    test_trace_summary("cache read block=%hu bm=0x%02hhX: START_SEQ + %u SEQ + LAST = %d commands, %d pages PAA-verified", block_index, plane_bitmap, (PAGES_PER_BLOCK - 2U), PAGES_PER_BLOCK, PAGES_PER_BLOCK);

    if (data != NULL)
    {
        *data = g_source_data; /* 最后一页（PAGES_PER_BLOCK-1）的明文 */
    }
}

void test_read_block_cache_multi_plane(WORD block_index, const fc_cmd_cfg_s *config, BYTE **data)
{
    fc_cmd_cfg_s    conf;
    fc_cmd_status_s completion;

    ASSERT(config != NULL);
    ASSERT(block_index < BLOCKS_PER_PLANE);

    /* ① START_SEQ_MP：输出 page0 pair，预取 page1 pair */
    completion = test_cache_start_seq(block_index, TEST_PLANE_BM_ALL, 0U, OP_READ_PAGE_CACHE_START_SEQ_MP, config);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    test_verify_page_paa(g_source_data, block_index, 0U, TEST_PLANE_BM_ALL);

    /* ② SEQ_MP：输出 page1..page62（每步两 plane 都校验） */
    conf           = *config;
    conf.operation = OP_READ_PAGE_CACHE_SEQ_MP;

    for (WORD page = 1U; page < (PAGES_PER_BLOCK - 1U); page++)
    {
        completion = test_read_cache_step(block_index, TEST_PLANE_BM_ALL, page, &conf);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, page, TEST_PLANE_BM_ALL);
    }

    /* ③ LAST_MP：输出 page63 pair */
    {
        const WORD last_page = (PAGES_PER_BLOCK - 1U);

        conf.operation = OP_READ_PAGE_CACHE_LAST_MP;

        completion = test_read_cache_step(block_index, TEST_PLANE_BM_ALL, last_page, &conf);
        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, last_page, TEST_PLANE_BM_ALL);
    }

    test_trace_summary("multi-plane cache read block=%hu: START_SEQ_MP + %u SEQ_MP + LAST_MP = %d commands, both planes PAA-verified", block_index, (PAGES_PER_BLOCK - 2U), PAGES_PER_BLOCK);

    if (data != NULL)
    {
        *data = g_source_data;
    }
}

/* ── 两 plane 各在一个 block 的 pair 原语（Policy B 的真实形态） ──
 *
 * test_cmd_build 只能表达「两 plane 同 block」：行地址来自一个 nand_addr_s。
 * 生产 FTL 的 pair 两 plane 分属不同 gblock（ftl_cmd_setup_ppa_pair_cw）⇒
 * 需要一条能分别给两个 plane 块的超集原语。
 *
 * meta 走**整对共用的 LBA 口径**（低 3 位由 desc 的 plane/cw 覆盖），不能用 PBA：
 * 一个命令只有一个 command->meta，而两 plane 的 PBA 已经不同。 */
fc_cmd_s *test_cmd_build_pair_cross_block(const WORD blocks[PLANE_NUM], WORD page_index, DWORD lba_meta, const fc_cmd_cfg_s *config)
{
    fc_cmd_s *cmd;
    DWORD     pba[PLANE_NUM];

    ASSERT(blocks != NULL);
    ASSERT(config != NULL);
    ASSERT(page_index < PAGES_PER_BLOCK);
    ASSERT(test_is_multi_op((BYTE)config->operation) != 0U);

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        nand_addr_s addr;

        ASSERT(blocks[plane] < BLOCKS_PER_PLANE);

        memset(&addr, 0, sizeof(addr));
        addr.block = blocks[plane];
        addr.plane = plane;
        addr.page  = page_index;

        pba[plane] = pba_from_addr(&addr);
    }

    cmd = fc_command_alloc();
    munit_assert_not_null(cmd);

    cmd->config             = *config;
    cmd->ch                 = 0U;
    cmd->ce                 = 0U;
    cmd->lun                = 0U;
    cmd->config.multi_plane = PLANE_NUM;
    cmd->meta               = lba_meta;

    if (ftl_cmd_setup_ppa_pair_cw(cmd, pba, test_plane_cw_bitmap(TEST_PLANE_BM_ALL)) != FTL_OK)
    {
        fc_command_release(cmd->cmd_id);
        munit_errorf("pair chain build failed (page=%hu)", page_index);
        return NULL;
    }

    return cmd;
}

/** 跨块 pair 的双 plane cache 编程：前 N-1 页 15h，末页 10h（与同块版本同序列）。 */
void test_prog_block_cache_multi_plane_pair(const WORD blocks[PLANE_NUM], const fc_cmd_cfg_s *config, BYTE **data)
{
    ASSERT(config != NULL);
    ASSERT(blocks != NULL);

    for (WORD page = 0U; page < (PAGES_PER_BLOCK - 1U); page++)
    {
        fc_cmd_cfg_s    conf = *config;
        fc_cmd_status_s completion;

        conf.operation = (page == 0U) ? OP_TWO_PLANE_PROGRAM_CACHE_FIRST : OP_TWO_PLANE_PROGRAM_CACHE;

        fc_cmd_s *cmd = test_cmd_build_pair_cross_block(blocks, page, (DWORD)page << 3, &conf);

        test_fill_user_paa(cmd);
        completion = test_submit_wait(cmd);
        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        fc_command_release(cmd->cmd_id);
    }

    {
        fc_cmd_cfg_s    conf = *config;
        fc_cmd_status_s completion;

        conf.operation = OP_TWO_PLANE_PROGRAM_CACHE_END;

        fc_cmd_s *cmd = test_cmd_build_pair_cross_block(blocks, (PAGES_PER_BLOCK - 1U), (PAGES_PER_BLOCK - 1U) << 3, &conf);

        test_fill_user_paa(cmd);
        completion = test_submit_wait(cmd);
        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        fc_command_release(cmd->cmd_id);
    }

    test_trace_summary("cross-block cache prog: plane0 blk=%hu / plane1 blk=%hu, %u x 15h + 1 x 10h = %d pairs", blocks[0], blocks[1], (PAGES_PER_BLOCK - 1U), PAGES_PER_BLOCK);

    if (data != NULL)
    {
        *data = g_source_data;
    }
}

/** 跨块 pair 的双 plane cache 读：START_SEQ_MP → SEQ_MP ×(N-2) → LAST_MP，逐页逐 plane 校验。 */
void test_read_block_cache_multi_plane_pair(const WORD blocks[PLANE_NUM], const fc_cmd_cfg_s *config)
{
    ASSERT(config != NULL);
    ASSERT(blocks != NULL);

    for (WORD page = 0U; page < PAGES_PER_BLOCK; page++)
    {
        fc_cmd_cfg_s    conf = *config;
        fc_cmd_status_s completion;

        if (page == 0U)
        {
            conf.operation = OP_READ_PAGE_CACHE_START_SEQ_MP;
        }
        else if (page == (PAGES_PER_BLOCK - 1U))
        {
            conf.operation = OP_READ_PAGE_CACHE_LAST_MP;
        }
        else
        {
            conf.operation = OP_READ_PAGE_CACHE_SEQ_MP;
        }

        fc_cmd_s *cmd = test_cmd_build_pair_cross_block(blocks, page, (DWORD)page << 3, &conf);

        test_memset_read_buffer(cmd);
        completion = test_submit_wait(cmd);
        test_capture_cmd(cmd);

        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_pair_paa(g_source_data, blocks, page, TEST_PLANE_BM_ALL);

        fc_command_release(cmd->cmd_id);
    }

    test_trace_summary("cross-block cache read: plane0 blk=%hu / plane1 blk=%hu, START_SEQ_MP + %u SEQ_MP + LAST_MP, both planes PAA-verified", blocks[0], blocks[1], (PAGES_PER_BLOCK - 2U));
}

void test_read_cache_random(WORD block_index, BYTE plane_bitmap, WORD prev_page, WORD addr_page, const fc_cmd_cfg_s *config)
{
    fc_cmd_cfg_s    conf;
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    DWORD           meta;
    const WORD      prefetched = (WORD)(prev_page + 1U);

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    /* 单 plane cache 读：段必须恰好一个 plane。 */
    ASSERT(test_plane_count(plane_bitmap) == 1U);
    ASSERT(block_index < BLOCKS_PER_PLANE);
    ASSERT(prev_page < PAGES_PER_BLOCK);
    ASSERT(addr_page < PAGES_PER_BLOCK);
    ASSERT(prefetched < PAGES_PER_BLOCK);
    ASSERT((addr_page + 1U) < PAGES_PER_BLOCK);
    /* START_SEQ 已预取 prev+1；RANDOM 的地址页若要和新输出区分，不能等于 prev+1。 */
    ASSERT(addr_page != prefetched);

    /* 语义（按 MT29F4G08 datasheet）：
     *   START_SEQ(prev) -> 输出 prev，data=prev+1
     *   RANDOM(addr)    -> 输出 prev+1（上一轮 data register 的页），data=addr
     *   SEQ(31h)        -> 输出 addr，data=addr+1
     *   LAST(3Fh)       -> 输出 addr+1
     * 地址页只进 data register，输出始终落后一页。 */

    /* ① START_SEQ：输出 prev，预取 prev+1 */
    completion = test_cache_start_seq(block_index, plane_bitmap, prev_page, OP_READ_PAGE_CACHE_START_SEQ, config);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    test_verify_page_paa(g_source_data, block_index, prev_page, plane_bitmap);

    /* ② RANDOM(addr)：输出 prev+1，预取 addr */
    memset(&addr, 0, sizeof(addr));
    addr.block     = block_index;
    addr.plane     = test_first_plane(plane_bitmap);
    addr.page      = addr_page;
    conf           = *config;
    conf.operation = OP_READ_PAGE_CACHE_RANDOM;
    meta           = test_page_pba(block_index, plane_bitmap, prefetched);

    cmd = test_cmd_build(&addr, test_plane_cw_bitmap(plane_bitmap), meta, &conf);

    test_memset_read_buffer(cmd);
    completion = test_submit_wait(cmd);
    test_capture_cmd(cmd);

    test_trace_detail("cache random addr=p%02hu -> out=p%02hu declared_meta=0x%08lX bm=0x%02hhX cw=%hhu status=0x%hhX", addr_page, prefetched, meta, plane_bitmap, test_cw_count(cmd), completion.status);

    {
        const fc_desc_s *desc     = test_cw_desc(cmd, 0U);
        const DWORD      expected = test_page_pba(block_index, plane_bitmap, prefetched);

        munit_assert_not_null(desc);
        munit_assert_uint32(desc->meta, ==, expected);
    }
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    test_verify_page_paa(g_source_data, block_index, prefetched, plane_bitmap);
    fc_command_release(cmd->cmd_id);

    /* ③ SEQ(31h)：输出 RANDOM 预取的 addr，预取 addr+1 */
    conf           = *config;
    conf.operation = OP_READ_PAGE_CACHE_SEQ;

    completion = test_read_cache_step(block_index, plane_bitmap, addr_page, &conf);
    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    test_verify_page_paa(g_source_data, block_index, addr_page, plane_bitmap);

    /* ④ LAST(3Fh)：输出 addr+1 */
    {
        const WORD next_page = (addr_page + 1U);

        conf.operation = OP_READ_PAGE_CACHE_LAST;

        completion = test_read_cache_step(block_index, plane_bitmap, next_page, &conf);
        munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
        test_verify_page_paa(g_source_data, block_index, next_page, plane_bitmap);
    }

    test_trace_summary("cache random block=%hu bm=0x%02hhX: START_SEQ(p%02hu) out=p%02hu -> RANDOM(addr=p%02hu) out=p%02hu -> 31h out=p%02hu -> 3Fh out=p%02u, 4 pages PAA-verified", block_index, plane_bitmap, prev_page, prev_page, addr_page, prefetched, addr_page, (addr_page + 1U));
}

/*************************************************************************
 *  加载 / 取数两段式
 **************************************************************************/

/**
 * @brief 纯加载：把目标页搬进器件页缓冲，不输出到总线。
 * @param block_index block 内块号。
 * @param plane_bitmap 单 plane op 必须只有一位；multi-plane op 必须 == TEST_PLANE_BM_ALL。
 * @param page_index 页号。
 * @param config operation 必须是 OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT 或
 *        OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT。
 * @note 不带 desc 链（cw_bitmap = 0，op 表 has_data = 0）；行地址由 FC 从 command->paa
 *       填进 plane_paa[]。数据随后由 test_random_data_read 取回。
 */
void test_load_page(WORD block_index, BYTE plane_bitmap, WORD page_index, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    BYTE            is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    ASSERT(block_index < BLOCKS_PER_PLANE);
    ASSERT(page_index < PAGES_PER_BLOCK);
    ASSERT((config->operation == OP_NORMAL_READ_PAGE_WITHOUT_DATA_OUT) || (config->operation == OP_READ_PAGE_MULTI_PLANE_WITHOUT_DATA_OUT));

    is_multi = test_is_multi_op((BYTE)config->operation);
    if (is_multi != 0U)
    {
        /* 多 plane 加载按定义搬两个 plane（fc_build_dispatch_request 的 op 29 分支无条件填
         * plane_paa[0]/[1]），半掩码表达不出别的意思 ⇒ 自洽性检查。 */
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    /* **不带 desc 链**：op 33/29 在 op 表里 has_data = 0（host 侧无数据搬运）⇒ 传 cw_bitmap = 0，
     * ftl_cmd_needs_desc_chain 短路，desc_id 保持 INVALID。行地址由 FC 从 command->paa 填进
     * plane_paa[]（fc_build_dispatch_request 的「无 desc 加载」分支）。meta 不参与（无数据路径）。
     * 提交后器件把目标页搬进页缓冲、**不输出到总线**——数据随后由 test_random_data_read 取回。 */
    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);
    addr.page  = page_index;

    cmd = test_cmd_build(&addr, 0U, 0U, config);

    completion = test_submit_wait(cmd);

    test_trace_detail("load op=%hhu b=%hu p=%hu plane=0x%02hhX status=0x%hhX (无 desc，仅搬进器件页缓冲)", config->operation, block_index, page_index, plane_bitmap, completion.status);

    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    fc_command_release(cmd->cmd_id);
}

/**
 * @brief 取数：把页缓冲里的整段 CW 抽到 USER 区，逐 CW PAA 校验。
 * @param block_index block 内块号。
 * @param plane_bitmap 单 plane op 必须只有一位；multi-plane op 必须 == TEST_PLANE_BM_ALL。
 * @param page_index 页号。
 * @param config operation 必须是 OP_RANDOM_DATA_READ 或 OP_RANDOM_DATA_READ_MULTI_PLANE。
 * @note 带整段掩码 desc 链；单条命令由 NAND CW 链自动遍历掩码内所有 CW。
 */
void test_random_data_read(WORD block_index, BYTE plane_bitmap, WORD page_index, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    BYTE            is_multi;

    ASSERT(config != NULL);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);
    ASSERT(block_index < BLOCKS_PER_PLANE);
    ASSERT(page_index < PAGES_PER_BLOCK);
    ASSERT((config->operation == OP_RANDOM_DATA_READ) || (config->operation == OP_RANDOM_DATA_READ_MULTI_PLANE));

    is_multi = test_is_multi_op((BYTE)config->operation);
    if (is_multi != 0U)
    {
        /* op 41（06h）按行地址 plane 位选页缓冲 ⇒ 段必须两个 plane 都覆盖。 */
        ASSERT(plane_bitmap == TEST_PLANE_BM_ALL);
    }
    else
    {
        /* op 8（05h）只有列地址周期、无 plane 位 ⇒ 单 plane。 */
        ASSERT(test_plane_count(plane_bitmap) == 1U);
    }

    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = test_first_plane(plane_bitmap);
    addr.page  = page_index;

    /* 整段掩码 ⇒ 单条命令由 CW 链自动遍历掩码内所有 CW（nand_dispatch.c 的 fetch 推进：
     * 每完成一个取数 op 就推进到下一个置位）。op 8/41 都在 fc_is_page_read 里 ⇒ FC 跑
     * process_read，USER 区回来即明文，逐 CW PAA 校验。 */
    cmd = test_cmd_build(&addr, test_plane_cw_bitmap(plane_bitmap), pba_from_addr(&addr), config);

    test_memset_read_buffer(cmd);

    completion = test_submit_wait(cmd);

    test_capture_cmd(cmd);

    test_trace_detail("random_data op=%hhu b=%hu p=%hu bm=0x%02hhX cw=%hhu status=0x%hhX", config->operation, block_index, page_index, plane_bitmap, test_cw_count(cmd), completion.status);

    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);
    test_verify_page_paa(g_source_data, block_index, page_index, plane_bitmap);

    fc_command_release(cmd->cmd_id);
}

/** META 只读回读（OP_READ_META）：无 desc 链，回读指定页、指定 CW 的前 4 字节 META，
 * 从 CQ 的 param.value 取回。cw 由 addr 携带（test_cmd_build 对本 op 放行 addr->cw）。 */
void test_read_meta(WORD block_index, BYTE plane, WORD page_index, BYTE cw, BYTE out[4])
{
    fc_cmd_cfg_s    config;
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;

    ASSERT(out != NULL);
    ASSERT(plane < PLANE_NUM);
    ASSERT(block_index < BLOCKS_PER_PLANE);
    ASSERT(page_index < PAGES_PER_BLOCK);
    ASSERT(cw < CW_NUM);

    /* 无 desc 链：cw_bitmap = 0 走 ftl_cmd_needs_desc_chain 短路（has_data = 0），
     * desc_id 保持 INVALID；cw 由 pba 携带 → FC 转成列地址。 */
    memset(&config, 0, sizeof(config));
    config.operation = OP_READ_META;

    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = plane;
    addr.page  = page_index;
    addr.cw    = cw;

    cmd = test_cmd_build(&addr, 0U, 0U, &config);

    completion = test_submit_wait(cmd);

    memcpy(out, &completion.param.value, 4U);

    test_trace_detail("read_meta b=%hu plane=%hhu p=%hu cw=%hhu status=0x%hhX meta=%02hhX%02hhX%02hhX%02hhX", block_index, plane, page_index, cw, completion.status, out[0], out[1], out[2], out[3]);

    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    fc_command_release(cmd->cmd_id);
}

/* 块锁族：op 21/22/23 与 op 25 的 op 表 has_data 全为 0 —— 不搬 host 数据、无 desc 链。
 * 行地址取自 cmd->paa（FC 的块锁族分支），故 block/plane 必须给对，否则命令静默打到 block 0。 */

/**
 * @brief 下发一条块锁命令（UNLOCK_LOW / UNLOCK_HIGH / LOCK）并断言无错误位。
 * @param block_index block 内块号。
 * @param plane plane 号。
 * @param config operation 必须是三种块锁 op 之一。
 */
void test_block_lock_cmd(WORD block_index, BYTE plane, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;

    ASSERT(config != NULL);
    ASSERT((config->operation == OP_BLOCK_UNLOCK_LOW) || (config->operation == OP_BLOCK_UNLOCK_HIGH) || (config->operation == OP_BLOCK_LOCK));
    ASSERT(plane < PLANE_NUM);
    ASSERT(block_index < BLOCKS_PER_PLANE);

    /* cw_bitmap = 0 → ftl_cmd_needs_desc_chain 短路，desc_id 保持 INVALID。 */
    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = plane;

    cmd = test_cmd_build(&addr, 0U, 0U, config);

    completion = test_submit_wait(cmd);

    test_trace_detail("block_lock op=%hhu b=%hu plane=%hhu status=0x%hhX", config->operation, block_index, plane, completion.status);

    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    fc_command_release(cmd->cmd_id);
}

/**
 * @brief 读块锁状态字节（op 25）。
 * @param block_index block 内块号。
 * @param plane plane 号。
 * @param config operation 必须是 OP_BLOCK_LOCK_STATUS。
 * @return 状态字节：bit0 = lock、bit1 = lock tight、bit2 = lock#。
 * @note 状态不经数据缓冲，直接从 CQ param.parameter0 取——必须在 fc_command_release 之前取。
 */
BYTE test_block_lock_status(WORD block_index, BYTE plane, const fc_cmd_cfg_s *config)
{
    nand_addr_s     addr;
    fc_cmd_s       *cmd;
    fc_cmd_status_s completion;
    BYTE            raw;

    ASSERT(config != NULL);
    ASSERT(config->operation == OP_BLOCK_LOCK_STATUS);
    ASSERT(plane < PLANE_NUM);
    ASSERT(block_index < BLOCKS_PER_PLANE);

    memset(&addr, 0, sizeof(addr));
    addr.block = block_index;
    addr.plane = plane;

    cmd = test_cmd_build(&addr, 0U, 0U, config);

    completion = test_submit_wait(cmd);

    /* 状态字节不经数据缓冲：引擎直读 DQ 总线 → nand_status → FC 拷进 CQ param.parameter0。
     * ⚠ 必须在 fc_command_release **之前**取。 */
    raw = completion.param.parameter0;

    test_trace_detail("lock_status b=%hu plane=%hhu raw=0x%02hhX lock#=%d lt#=%d lt=%d status=0x%hhX", block_index, plane, raw, bit_mask_any(raw, 0x04U), bit_mask_any(raw, 0x02U), bit_mask_any(raw, 0x01U), completion.status);

    munit_assert_uint32(completion.status & FC_CMD_STATUS_ERROR_MASK, ==, 0U);

    fc_command_release(cmd->cmd_id);

    return raw;
}

/**************************************************************************
 *  数据搬运 / 校验
 **************************************************************************/

void test_fill_paa(BYTE *slot, DWORD paa)
{
    ASSERT(slot != NULL);

    /* 用 memcpy 而非 DWORD* 强转：避开对齐与严格别名问题（槽在 malloc 块内，
     * 但没必要依赖这一点）。 */
    for (DWORD i = 0U; i < (FC_BUFFER_USER_SIZE / 4U); i++)
    {
        memcpy(&slot[4U * i], &paa, sizeof(paa));
    }
}

BYTE test_check_paa(const BYTE *slot, DWORD paa)
{
    DWORD word;

    ASSERT(slot != NULL);

    for (DWORD i = 0U; i < (FC_BUFFER_USER_SIZE / 4U); i++)
    {
        memcpy(&word, &slot[4U * i], sizeof(word));

        if (word != paa)
        {
            return 0U;
        }
    }
    return 1U;
}

void test_fill_user_paa(fc_cmd_s *cmd)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    BYTE         idx     = 0U;

    while ((desc_id != FC_DESC_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        BYTE *slot = test_source_slot(paa_to_plane(desc->paa), paa_to_cw(desc->paa));
        BYTE *user = fc_buffer_get_user_data(desc->buffer_id);
        munit_assert_not_null(user);

        test_fill_paa(slot, desc->paa);
        memcpy(user, slot, FC_BUFFER_USER_SIZE);

        desc_id = (fc_desc_id_t)desc->next_id;
        idx++;
    }
}

void test_memset_read_buffer(fc_cmd_s *cmd)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    BYTE         idx     = 0U;

    while ((desc_id != FC_DESC_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        BYTE *user = fc_buffer_get_user_data(desc->buffer_id);
        munit_assert_not_null(user);

        memset(user, 0xCC, FC_BUFFER_USER_SIZE);

        desc_id = (fc_desc_id_t)desc->next_id;
        idx++;
    }
}

void test_verify_page_paa(const BYTE *data, WORD block_index, WORD page_index, BYTE plane_bitmap)
{
    munit_assert_not_null(data);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(plane)) == 0U)
        {
            continue;
        }

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            nand_addr_s addr;
            const BYTE *slot = &data[((DWORD)plane * CW_NUM + (DWORD)cw) * FC_BUFFER_USER_SIZE];
            DWORD       expected;

            memset(&addr, 0, sizeof(addr));
            addr.block = block_index;
            addr.plane = plane;
            addr.page  = page_index;
            addr.cw    = cw;
            expected   = paa_from_addr(&addr);

            if (test_check_paa(slot, expected) == 0U)
            {
                munit_errorf("PAA mismatch: block=%hu page=%hu plane=%hhu cw=%hhu (expected paa=0x%08lX)", block_index, page_index, plane, cw, expected);
                return;
            }
        }
    }
}

static void test_verify_page_pair_paa(const BYTE *data, const WORD blocks[PLANE_NUM], WORD page_index, BYTE plane_bitmap)
{
    munit_assert_not_null(data);
    munit_assert_not_null(blocks);
    ASSERT(plane_bitmap != 0U);
    ASSERT((plane_bitmap & (BYTE)~TEST_PLANE_BM_ALL) == 0U);

    for (BYTE plane = 0U; plane < PLANE_NUM; plane++)
    {
        if ((plane_bitmap & TEST_PLANE_BM(plane)) == 0U)
        {
            continue;
        }

        for (BYTE cw = 0U; cw < CW_NUM; cw++)
        {
            nand_addr_s addr;
            const BYTE *slot = &data[((DWORD)plane * CW_NUM + (DWORD)cw) * FC_BUFFER_USER_SIZE];
            DWORD       expected;

            memset(&addr, 0, sizeof(addr));
            addr.block = blocks[plane];
            addr.plane = plane;
            addr.page  = page_index;
            addr.cw    = cw;
            expected   = paa_from_addr(&addr);

            if (test_check_paa(slot, expected) == 0U)
            {
                munit_errorf("cross-block PAA mismatch: plane0 blk=%hu plane1 blk=%hu page=%hu plane=%hhu cw=%hhu (expected paa=0x%08lX)", blocks[0], blocks[1], page_index, plane, cw, expected);
                return;
            }
        }
    }
}

void test_verify_erased(const fc_cmd_s *cmd)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    BYTE         idx     = 0U;

    while ((desc_id != FC_DESC_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        BYTE *buf = fc_buffer_get_user_data(desc->buffer_id);
        munit_assert_not_null(buf);

        for (WORD i = 0U; i < FC_BUFFER_USER_SIZE; i++)
        {
            if (buf[i] != 0xFFU)
            {
                munit_errorf("chain[%hhu] plane=%hhu cw=%hhu offset=%hu: got 0x%02hhX expected 0xFF (erased)", idx, paa_to_plane(desc->paa), paa_to_cw(desc->paa), i, buf[i]);
                return;
            }
        }

        desc_id = (fc_desc_id_t)desc->next_id;
        idx++;
    }
}

BYTE test_verify_cw_status_all_success(const fc_cmd_s *cmd)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    BYTE         idx     = 0U;
    BYTE         all_ok  = 1U;

    while ((desc_id != FC_DESC_ID_INVALID) && (idx < CW_TOTAL_MAX))
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);
        munit_assert_not_null(desc);

        if (fc_cw_status_is_success(desc->status) == 0U)
        {
            char stbuf[32];

            (void)fc_cw_status_str(desc->status, stbuf, sizeof(stbuf));
            munit_errorf("chain[%hhu] plane=%hhu cw=%hhu status=%s expected FC_CW_STATUS_UNUSED", idx, paa_to_plane(desc->paa), paa_to_cw(desc->paa), stbuf);
            all_ok = 0U;
        }

        desc_id = (fc_desc_id_t)desc->next_id;
        idx++;
    }

    return all_ok;
}

/*************************************************************************
 *  desc 链小工具
 **************************************************************************/

/** 链遍历一律带 `CW_TOTAL_MAX` 上界：链是池内 id 串起来的，正常不超过 PLANE_NUM×CW_NUM，
 * 但**成环**（next_id 指回）时哨兵永远不出现——上界把挂死换成安静的截断。 */
fc_desc_s *test_cw_desc(const fc_cmd_s *cmd, BYTE idx)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;

    for (BYTE i = 0U; (desc_id != FC_DESC_ID_INVALID) && (i < CW_TOTAL_MAX); i++)
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);
        if (desc == NULL)
        {
            return NULL;
        }
        if (i == idx)
        {
            return desc;
        }
        desc_id = (fc_desc_id_t)desc->next_id;
    }
    return NULL;
}

BYTE *test_cw_user(const fc_cmd_s *cmd, BYTE idx)
{
    fc_desc_s *desc = test_cw_desc(cmd, idx);

    munit_assert_not_null(desc);
    return fc_buffer_get_user_data(desc->buffer_id);
}

BYTE *test_cw_base(const fc_cmd_s *cmd, BYTE idx)
{
    fc_desc_s *desc = test_cw_desc(cmd, idx);

    munit_assert_not_null(desc);
    return fc_buffer_get_base(desc->buffer_id);
}

BYTE test_cw_count(const fc_cmd_s *cmd)
{
    BYTE count = 0U;

    while (test_cw_desc(cmd, count) != NULL)
    {
        count++;
    }
    return count;
}

BYTE test_collect_chain(const fc_cmd_s *cmd, BYTE *plane_out, BYTE *cw_out)
{
    fc_desc_id_t desc_id = (fc_desc_id_t)cmd->desc_id;
    BYTE         count   = 0U;

    while ((desc_id != FC_DESC_ID_INVALID) && (count < CW_TOTAL_MAX))
    {
        fc_desc_s *desc = fc_descriptor_id_to_descriptor(desc_id);

        munit_assert_not_null(desc);

        if (plane_out != NULL)
        {
            plane_out[count] = paa_to_plane(desc->paa);
        }
        if (cw_out != NULL)
        {
            cw_out[count] = paa_to_cw(desc->paa);
        }

        desc_id = (fc_desc_id_t)desc->next_id;
        count++;
    }

    return count;
}

DWORD test_cw_seed(const fc_cmd_s *cmd, BYTE idx)
{
    fc_desc_s *desc = test_cw_desc(cmd, idx);

    munit_assert_not_null(desc);
    return fc_random_seed_from_meta(desc->meta);
}

BYTE test_check_pattern(const BYTE *buffer, WORD length, BYTE pattern)
{
    for (WORD i = 0U; i < length; i++)
    {
        if (buffer[i] != pattern)
        {
            return 0U;
        }
    }
    return 1U;
}
