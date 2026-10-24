/*
 * test_fc_level1.c — Level 1 纯逻辑测试：fc_command + fc_queue + fc_scheduler
 *
 * 共 12 个测试函数，全部是纯 RAM 逻辑：不碰 NAND、不需要 fc_init()、
 * 不依赖任何池，每个用例自带全部夹具（栈上队列 / 栈上命令）。
 *
 * 覆盖范围：
 *   Part 1  fc_command_validate  —— 合法输入放行（冒烟性质，见下）
 *   Part 2  fc_submission_queue  —— SQ 环形 FIFO：顺序 / 回绕 / 满 / peek
 *   Part 2b fc_completion_queue  —— CQ 环形 FIFO：初始化 / 顺序 / 满
 *   Part 3  fc_scheduler         —— init 绑定 SQ/HSQ
 *   Part 4  ONFI 参数页解码       —— 合成页喂纯函数，逐字段断言（手册 Table 11）
 *
 * 链接依赖：g_nand_operation_map（.rodata, onfi_1_0.o）——经 fc_command_validate()
 * 的 nand_operation_has_data() 宏引用；以及 fc_test_common.o 的证据打印原语
 * test_trace_*（`--log-visible info|debug` 才输出，默认零字节）。
 *
 * 历史注（v8 / v13 / v15 / v20 / 2026-10-08）：
 *   - 调度器的选择逻辑（HSQ 优先 / 按 LUN 保序 / 链打包 / per-LUN FIFO 挂起）
 *     已搬进 fc.c 的 static fc_poll_fill，只能经 fc_service() 触达（会推真 NAND），
 *     不再属 Level 1 纯逻辑范畴——相关用例移至 Level 2/3。
 *   - fc_command_validate 的负向守卫全部转 ASSERT（v20）；param.cw_bitmap 的读者
 *     是 FTL，FC 侧不再校验（v13）。负向用例随之删除，只留正向冒烟。
 *   - fc_command_t 不再携带 command_id（v15），ID 由指针派生。
 *   - 删除 5 条用例（2026-10-08），判据统一为「被测代码还有没有生产调用者」：
 *       status_init_pool_cmd / status_init_stack_cmd / status_set_cw_valid
 *         —— fc_command_status_init / _set_cw 自 2026-09-20 起零生产调用者
 *            （CQ 完成项改由 fc_complete_from_nand() 内联组装），函数连同用例一并删除；
 *       pop_n —— fc_submission_queue_pop_n 零生产调用者（调度器走逐条 pop）；
 *       empty_full —— 断言集是 peek_empty ∪ overflow 的子集，零增量覆盖。
 *     随之删除 setup_fc_suite（其唯一用途是给 status_init 两条用例初始化命令池）。
 */

#include "fc_command.h"
#include "fc_queue.h"
#include "fc_scheduler.h"
#include "fc_test_common.h" /* 只为证据打印原语 test_trace_*（本文件不碰 NAND / 池） */
#include "munit.h"
#include "onfi_1_0.h"

/**************************************************************************
 *  Part 1: fc_command 测试
 *
 *  只覆盖 fc_command_validate 的**正向**面。v20 起该函数三条守卫
 *  （command==NULL / operation>=OP_COUNT / 数据操作无 desc 链）全转 ASSERT，
 *  函数体只剩 `return FC_STATUS_OK`，因此这里的断言是**恒真**的——用例的性质
 *  从「验证返回值」转为「冒烟」：用合法输入把内层断言跑一遍，拦住以后有人
 *  加了过严的断言。这一层是否值得保留，取决于内层断言是否还会变。
 *
 *  注：v13 起 param.cw_bitmap 的读者是 FTL，FC 侧不再校验它。
 **************************************************************************/

/* --------------------------------------------------------------------
 * test_command_validate_op_range
 *   功能：确认 fc_command_validate 对全部合法 operation 放行。
 *   测什么：op 遍历 0 .. OP_COUNT-1，每次都断言返回 FC_STATUS_OK。
 *   设计思路：返回值这一维已不可能失败（见 Part 1 头注），真实价值是冒烟——
 *         遍历**全部** op 而非抽一个，是因为「数据操作必须有 desc 链」那条断言
 *         是按 operation 分类的，抽一个样本覆盖不到分类边界。
 *         desc_id 故意填 0（合法描述符号）而不是 INVALID：填 INVALID 会让
 *         「有数据的操作必须有链」那条断言对数据类 op 立刻响，用例就变成
 *         在测断言本身而不是测放行。
 * -------------------------------------------------------------------- */
static MunitResult test_command_validate_op_range(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t cmd = {0};

    test_trace_begin();

    /* 合法 OP：0 .. OP_COUNT-1 恒 OK（非法 OP 现由 ASSERT 拦截，不再是返回码） */
    for (uint8_t op = 0U; op < OP_COUNT; op++)
    {
        cmd.config.operation = op;
        cmd.desc_id          = 0U;
        munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);
    }

    /* 证据（--log-visible info）：遍历范围本身就是判据——按 operation 分类的那条断言
     * 只有走遍全部 op 才覆盖得到分类边界。 */
    test_trace_summary("validate: op 0..%u 全部放行（desc_id=0 合法）", (unsigned)(OP_COUNT - 1U));

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_command_validate_set_get_no_desc
 *   功能：确认 SET/GET_FEATURES 在**没有** desc 链时仍被放行。
 *   测什么：desc_id = INVALID 时，两个 FEATURES 操作都返回 FC_STATUS_OK。
 *   设计思路：这两条操作的数据走 SQ/CQ 的 param.bytes 内联段，不经 NAND 数据
 *         路径，所以「有数据的操作必须有 desc 链」那条断言必须对它们短路。
 *         用例就是钉住这个短路分支——它一旦被改坏（例如断言的布尔条件顺序
 *         调整、或 nand_operation_has_data 的分类变了），FEATURES 会在提交时
 *         直接停机，而这是 FTL 读写 BBT / 元数据要走的路径。
 *         desc_id 设成 INVALID 是本用例与上一条的唯一区别，两条例合用例
 *         共同把那条断言的**两个方向**都钉住。
 * -------------------------------------------------------------------- */
static MunitResult test_command_validate_set_get_no_desc(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    fc_command_t cmd = {0};

    test_trace_begin();

    cmd.desc_id = FC_DESCRIPTOR_ID_INVALID;

    cmd.config.operation = OP_SET_FEATURES;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);

    cmd.config.operation = OP_GET_FEATURES;
    munit_assert_int(fc_command_validate(&cmd), ==, FC_STATUS_OK);

    /* 证据（--log-visible info）：desc_id=INVALID 是这条用例的关键输入。 */
    test_trace_summary("validate: SET/GET_FEATURES 在 desc_id=INVALID（0x%X）时仍放行——数据走 param 内联段", (unsigned)FC_DESCRIPTOR_ID_INVALID);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 2: fc_submission_queue 测试
 *
 *  测试环形 FIFO 的核心操作：push / pop / peek，以及边界条件：空、满、环绕。
 *
 *  深度用 TEST_Q_DEPTH=8 而不是生产深度 64：夹具小到能一眼看出「入队-出队」
 *  的对应关系，且让每个用例只盯一件事（顺序 / 回绕 / 满 / 空各自独立，
 *  不互相掩盖）。
 **************************************************************************/

#define TEST_Q_DEPTH (8U)

/* --------------------------------------------------------------------
 * test_queue_push_pop
 *   功能：验证 SQ 的 FIFO 顺序（先进先出）。
 *   测什么：连 push 3 条后逐条 pop，断言出队顺序与入队一致；再 pop 一次
 *         断言 QUEUE_EMPTY。
 *   设计思路：顺序是队列最核心的契约——错了命令会以错误次序下发到 NAND，
 *         而且不会崩、只会错数据，属于最难查的一类。用固定期望值（0x01/0x02/0x03）
 *         而非「再 pop 一遍对比」：后者会被同一个 bug 掩盖。
 *         本用例刻意不触发环绕（3 < 8），把回绕留给下一条单独测。
 * -------------------------------------------------------------------- */
static MunitResult test_queue_push_pop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    fc_command_id_t       val;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    munit_assert_int(fc_submission_queue_push(&q, 0x01U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x02U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x03U), ==, FC_STATUS_OK);

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x01U);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x02U);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x03U);

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_QUEUE_EMPTY);

    /* 证据（--log-visible info）：顺序是队列最核心的契约——错了不崩、只错数据。 */
    test_trace_summary("SQ push 0x01/0x02/0x03 -> pop 顺序一致 -> 第 4 次 QUEUE_EMPTY (depth=%u)", (unsigned)TEST_Q_DEPTH);

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_queue_wrap_around
 *   功能：验证环形缓冲 head / tail 的回绕。
 *   测什么：填满 8 条 → pop 1 条（head 前进到 1）→ 再 push 1 条（tail 回绕到 0）
 *         → 断言剩余顺序恰为 0x11..0x17, 0xAA，最后 QUEUE_EMPTY。
 *   设计思路：回绕是环形队列唯一容易写错的算术（两处 `% depth`）。用例刻意
 *         把 head / tail 摆到环的两侧（head=1, tail=0），只要有一处回绕写错，
 *         这个断言序列就会错位。期望值写死成字面量而不是「先 pop 完再比对」，
 *         同样是为了让错误立刻可读。
 *         末条 0xAA 是**越过回绕点**写入的元素，专门验证 tail 回绕后写入的位置
 *         没有覆盖还在队列里的旧元素。
 * -------------------------------------------------------------------- */
static MunitResult test_queue_wrap_around(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    fc_command_id_t       val;
    uint16_t              i;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    for (i = 0U; i < TEST_Q_DEPTH; i++)
    {
        munit_assert_int(fc_submission_queue_push(&q, (fc_command_id_t)(0x10U + i)), ==, FC_STATUS_OK);
    }

    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0x10U);

    munit_assert_int(fc_submission_queue_push(&q, 0xAAU), ==, FC_STATUS_OK);

    /* head=1, tail=0 → 顺序：0x11..0x17, 0xAA */
    for (i = 0x11U; i <= 0x17U; i++)
    {
        munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
        munit_assert_uint32(val, ==, (fc_command_id_t)i);
    }
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_uint32(val, ==, 0xAAU);
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_QUEUE_EMPTY);

    /* 证据（--log-visible info）：head=1 / tail=0 把两处回绕算术同时摆在环的两侧。 */
    test_trace_summary("SQ 回绕: head=1 tail=0 -> 0x11..0x17, 0xAA -> EMPTY (depth=%u)", (unsigned)TEST_Q_DEPTH);

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_queue_overflow
 *   功能：验证满队列的背压语义。
 *   测什么：填满 8 条 → 再 push 断言 QUEUE_FULL → pop 一条 → push 断言恢复 OK。
 *   设计思路：QUEUE_FULL 是 FC 对 FTL 的背压信号（fc_submit 的**唯一**合法失败），
 *         必须真的被拒绝，而不是静默覆盖队头。
 *         后半段「pop 后恢复可用」才是这条用例的重点：只测前半段的话，
 *         count 没正确回退、或满判定写成 `head == tail` 的实现都能蒙混过关。
 * -------------------------------------------------------------------- */
static MunitResult test_queue_overflow(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;
    uint16_t              i;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    for (i = 0U; i < TEST_Q_DEPTH; i++)
    {
        munit_assert_int(fc_submission_queue_push(&q, (fc_command_id_t)i), ==, FC_STATUS_OK);
    }

    munit_assert_int(fc_submission_queue_push(&q, 0xFFU), ==, FC_STATUS_QUEUE_FULL);

    /* pop 后恢复可用 */
    fc_command_id_t val;
    munit_assert_int(fc_submission_queue_pop(&q, &val), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0xFFU), ==, FC_STATUS_OK);

    /* 证据（--log-visible info）：后半段「pop 后恢复可用」才是重点——只测前半段的话，
     * count 没回退、或满判定写成 head == tail 的实现都能蒙混过关。 */
    test_trace_summary("SQ 满 %u 条 -> push QUEUE_FULL -> pop 1 条 -> push 恢复 OK", (unsigned)TEST_Q_DEPTH);

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_queue_peek
 *   功能：验证 peek 按偏移随机访问、且**不消耗**队列。
 *   测什么：push 3 条后 peek(0) 连查两次、再查 peek(1) / peek(2)，
 *         断言取到对应的队内元素。
 *   设计思路：peek 是生产路径上唯一被调用的只读原语——fc.c 的链打包用它
 *         预判「队头起第 need-1 条是否同链」（fc_poll_fill_queue），判错了
 *         会把不该打包的命令塞进同一次下发。
 *         重点因此有两个：偏移算对（0/1/2 各自命中），以及**连查两次结果相同**
 *         ——后者证明 peek 没有推进 head，否则调用方每探一次就吃掉一条命令。
 * -------------------------------------------------------------------- */
static MunitResult test_queue_peek(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);

    munit_assert_int(fc_submission_queue_push(&q, 0x01U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x02U), ==, FC_STATUS_OK);
    munit_assert_int(fc_submission_queue_push(&q, 0x03U), ==, FC_STATUS_OK);

    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, 0x01U);
    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, 0x01U);
    munit_assert_uint32(fc_submission_queue_peek(&q, 1U), ==, 0x02U);
    munit_assert_uint32(fc_submission_queue_peek(&q, 2U), ==, 0x03U);

    /* 证据（--log-visible info）：peek(0) 连查两次结果相同 ⇒ 证明它**没有**推进 head
     * （否则调用方每探一次就吃掉一条命令）。 */
    test_trace_summary("SQ peek: 0->0x01（连查两次不变）, 1->0x02, 2->0x03；队列未被消耗");

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_queue_peek_empty
 *   功能：验证空队列 peek 的哨兵返回。
 *   测什么：刚 init 的队列 peek(0) 断言 FC_COMMAND_ID_INVALID。
 *   设计思路：peek 的调用方（fc.c 链打包）正是靠这个哨兵判链尾，返回 0 或
 *         越界读都会让打包循环走错方向。空队列踩的是 `offset >= count` 这条
 *         边界，是哨兵契约最容易漏的一格，故单独用例钉住。
 *         与 test_queue_overflow 里那条 peek 断言不同：这里队列**始终**是空的，
 *         不掺入任何 push/pop 干扰。
 * -------------------------------------------------------------------- */
static MunitResult test_queue_peek_empty(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_id_t       storage[TEST_Q_DEPTH];
    fc_submission_queue_t q;

    munit_assert_int(fc_submission_queue_init(&q, storage, TEST_Q_DEPTH), ==, FC_STATUS_OK);
    munit_assert_uint32(fc_submission_queue_peek(&q, 0U), ==, FC_COMMAND_ID_INVALID);

    /* 证据（--log-visible info）：fc.c 链打包正是靠这个哨兵判链尾。 */
    test_trace_summary("SQ 空队列 peek(0) -> FC_COMMAND_ID_INVALID (0x%X)", (unsigned)FC_COMMAND_ID_INVALID);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 2b: fc_completion_queue 测试
 *
 *  CQ 存 fc_command_status_t（整项拷贝），FIFO 语义与 SQ 相同，但存储类型
 *  与深度不同，故单独一组。CQ 的生产路径是 fc_complete_from_nand()（push）
 *  与 fc_poll_completion_internal()（pop）。
 **************************************************************************/

#define TEST_CQ_DEPTH (8U)

/* --------------------------------------------------------------------
 * test_cqueue_init
 *   功能：验证 CQ 初始化的字段落位。
 *   测什么：init 后断言 depth 被记录、count == 0。
 *   设计思路：CQ 是完成项的搬运通道，init 漏清零会让「满/空」判定从一开始
 *         就错。只断言这两个字段，是因为 head / tail 的初值属实现细节——
 *         钉死它们会让以后改成绝对序号之类的重构无谓地失败，而 depth/count
 *         是调用方（fc.c）真正读的契约。
 * -------------------------------------------------------------------- */
static MunitResult test_cqueue_init(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_status_t   storage[TEST_CQ_DEPTH];
    fc_completion_queue_t q;

    munit_assert_int(fc_completion_queue_init(&q, storage, TEST_CQ_DEPTH), ==, FC_STATUS_OK);
    munit_assert_uint32(q.depth, ==, TEST_CQ_DEPTH);
    munit_assert_uint32(q.count, ==, 0U);

    /* 证据（--log-visible info）：init 漏清零会让「满/空」判定从一开始就错。 */
    test_trace_summary("CQ init: depth=%u count=%u", (unsigned)q.depth, (unsigned)q.count);

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_cqueue_push_pop
 *   功能：验证 CQ 的 FIFO 顺序与整项拷贝。
 *   测什么：push 两条（command_id = 0x01 / 0x02）后逐条 pop，断言 command_id
 *         顺序一致；再 pop 一次断言 QUEUE_EMPTY。
 *   设计思路：CQ 存的是 fc_command_status_t **整体**而不是句柄，所以顺序之外
 *         还隐含「拷贝而非引用」——用两个不同的 command_id 区分，顺序错了
 *         立刻可见。语义与 SQ 相同但存储类型不同（结构体 vs 16 位 ID），
 *         拷贝路径是另一段代码，不能靠 SQ 的用例代为覆盖。
 * -------------------------------------------------------------------- */
static MunitResult test_cqueue_push_pop(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_status_t   storage[TEST_CQ_DEPTH];
    fc_completion_queue_t q;
    fc_command_status_t   entry  = {0};
    fc_command_status_t   popped = {0};

    munit_assert_int(fc_completion_queue_init(&q, storage, TEST_CQ_DEPTH), ==, FC_STATUS_OK);

    entry.command_id = 0x01U;
    entry.status     = FC_CW_STATUS_READY;
    munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_OK);

    entry.command_id = 0x02U;
    munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_OK);

    munit_assert_int(fc_completion_queue_pop(&q, &popped), ==, FC_STATUS_OK);
    munit_assert_uint32(popped.command_id, ==, 0x01U);

    munit_assert_int(fc_completion_queue_pop(&q, &popped), ==, FC_STATUS_OK);
    munit_assert_uint32(popped.command_id, ==, 0x02U);

    munit_assert_int(fc_completion_queue_pop(&q, &popped), ==, FC_STATUS_QUEUE_EMPTY);

    /* 证据（--log-visible info）：CQ 存整项而非句柄，用两个不同 command_id 区分
     * ⇒ 顺序错或拷贝错立刻可见。 */
    test_trace_summary("CQ push 0x01/0x02（整项拷贝）-> pop 顺序一致 -> EMPTY (depth=%u)", (unsigned)TEST_CQ_DEPTH);

    return MUNIT_OK;
}

/* --------------------------------------------------------------------
 * test_cqueue_overflow
 *   功能：验证 CQ 满时的背压。
 *   测什么：填满 TEST_CQ_DEPTH 条 → 再 push 断言 QUEUE_FULL。
 *   设计思路：CQ 满在 fc_complete_from_nand() 里是 fatal（完成项一旦丢失，
 *         FTL 再也等不到这条命令），所以「满了必须拒绝而不是覆盖」是硬契约。
 *         与 SQ 的 overflow 不同，这里**不**测「pop 后恢复」——CQ 的消费方是
 *         FTL 的完成处理，本用例不模拟消费节奏，只钉住满判定本身。
 * -------------------------------------------------------------------- */
static MunitResult test_cqueue_overflow(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_status_t   storage[TEST_CQ_DEPTH];
    fc_completion_queue_t q;
    uint16_t              i;

    munit_assert_int(fc_completion_queue_init(&q, storage, TEST_CQ_DEPTH), ==, FC_STATUS_OK);

    fc_command_status_t entry = {0};
    entry.status              = FC_CW_STATUS_READY;

    for (i = 0U; i < TEST_CQ_DEPTH; i++)
    {
        entry.command_id = (fc_command_id_t)i;
        munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_OK);
    }

    entry.command_id = 0xFFU;
    munit_assert_int(fc_completion_queue_push(&q, &entry), ==, FC_STATUS_QUEUE_FULL);

    /* 证据（--log-visible info）：CQ 满在 fc_complete_from_nand() 里是 fatal
     * （完成项丢了 FTL 就再也等不到）⇒「满了必须拒绝而不是覆盖」是硬契约。 */
    test_trace_summary("CQ 满 %u 条 -> push QUEUE_FULL（拒绝而非覆盖队头）", (unsigned)TEST_CQ_DEPTH);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 3: fc_scheduler 测试
 *
 *  v8 后调度器只剩 fc_scheduler_init —— 绑定 SQ/HSQ 两个队列指针，纯 stack
 *  操作，零初始化依赖。
 *
 *  选择逻辑（HSQ 优先 / 按 LUN 保序 / 链打包 / per-LUN FIFO 挂起）已搬进
 *  fc.c 的 static fc_poll_fill，只能经 fc_service() 触达（会推真 NAND）——
 *  其覆盖属 Level 2/3，不在本文件。
 **************************************************************************/

#define SCHED_Q_DEPTH (16U)

/* --------------------------------------------------------------------
 * test_scheduler_init
 *   功能：验证 fc_scheduler_init 绑定 SQ / HSQ 两个队列指针。
 *   测什么：用两个栈上队列 init 调度器，断言 scheduler.sq / .hsq 指向传入对象。
 *   设计思路：断言**指针相等**而不是断言内部状态，是因为「不复制队列、只持有
 *         引用」正是它的契约——队列存储由调用方提供（见 fc_queue.h 头注）。
 *         若实现改成按值拷贝，指针断言会立刻失败，而状态断言可能仍然通过。
 *         生产调用点是 fc.c 的 fc_init_internal（init 时一次）。
 * -------------------------------------------------------------------- */
static MunitResult test_scheduler_init(const MunitParameter params[], void *data)
{
    (void)params;
    (void)data;

    test_trace_begin();

    fc_command_id_t       sq_storage[SCHED_Q_DEPTH];
    fc_command_id_t       hsq_storage[SCHED_Q_DEPTH];
    fc_submission_queue_t sq, hsq;
    fc_scheduler_t        scheduler;

    fc_submission_queue_init(&sq, sq_storage, SCHED_Q_DEPTH);
    fc_submission_queue_init(&hsq, hsq_storage, SCHED_Q_DEPTH);

    munit_assert_int(fc_scheduler_init(&scheduler, &sq, &hsq), ==, FC_STATUS_OK);

    munit_assert_ptr_equal(scheduler.sq, &sq);
    munit_assert_ptr_equal(scheduler.hsq, &hsq);

    /* 证据（--log-visible info）：断言的是**指针相等**——契约是「不复制队列、只持引用」。 */
    test_trace_summary("scheduler init: sq=%p hsq=%p（持有引用，不拷贝；depth=%u）", (void *)scheduler.sq, (void *)scheduler.hsq, (unsigned)SCHED_Q_DEPTH);

    return MUNIT_OK;
}

/**************************************************************************
 *  Part 4: ONFI 参数页解码（手册 Table 11）
 *
 *  nand_onfi_parameter_page_decode() 是**纯函数**——零 HAL / 零日志 / 零硬件，
 *  故可在 Level 1 用栈上合成页直接测，不碰 NAND。断言取的是「手册 Table 11
 *  给死的值」（MT29F4G08ABADAWP 列），不是从代码反推的值。
 **************************************************************************/

/* 小端写入（合成参数页用；解码侧的小端读取在 onfi_1_0.h）。 */
static void test_put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void test_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8U) & 0xFFU);
    p[2] = (uint8_t)((value >> 16U) & 0xFFU);
    p[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* --------------------------------------------------------------------
 * test_onfi_parameter_page_decode
 *   功能：纯函数 nand_onfi_parameter_page_decode() 的字段解码验证。
 *   测什么：合成一张按手册 Table 11（MT29F4G08ABADAWP 列）取值的 256 字节参数页，
 *         逐字段断言；再破坏签名，断言返回 0 且字段仍被填满（供诊断打印）。
 *   设计思路：解码函数纯 RAM ⇒ 无需真 NAND；偏移宏与解码函数共用一套
 *         NAND_ONFI_PARAM_OFF_*，本用例不另写魔数。
 * -------------------------------------------------------------------- */
static MunitResult test_onfi_parameter_page_decode(const MunitParameter params[], void *data)
{
    static const char      model[] = "MT29F4G08ABADAWP";
    uint8_t                page[NAND_ONFI_PARAM_PAGE_SIZE];
    nand_onfi_param_info_t info;
    uint16_t               i; /* 需装下 0..255 的整页计数（uint8_t 会让 i<PAGE_SIZE 恒真） */

    (void)params;
    (void)data;

    test_trace_begin();

    /* 合成参数页：全 0 → 逐字段按 Table 11 填值。 */
    for (i = 0U; i < NAND_ONFI_PARAM_PAGE_SIZE; i++)
    {
        page[i] = 0x00U;
    }

    page[NAND_ONFI_PARAM_OFF_SIGNATURE + 0U] = 0x4FU; /* 'O' */
    page[NAND_ONFI_PARAM_OFF_SIGNATURE + 1U] = 0x4EU; /* 'N' */
    page[NAND_ONFI_PARAM_OFF_SIGNATURE + 2U] = 0x46U; /* 'F' */
    page[NAND_ONFI_PARAM_OFF_SIGNATURE + 3U] = 0x49U; /* 'I' */
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_REVISION], 0x0002U);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_FEATURES], 0x0018U);

    /* 型号串：bytes 44–63 空格填充，前 16 字节是型号名。 */
    for (i = 0U; i < NAND_ONFI_PARAM_MODEL_LEN; i++)
    {
        page[NAND_ONFI_PARAM_OFF_MODEL + i] = 0x20U;
    }
    for (i = 0U; i < (uint8_t)(sizeof(model) - 1U); i++)
    {
        page[NAND_ONFI_PARAM_OFF_MODEL + i] = (uint8_t)model[i];
    }

    page[NAND_ONFI_PARAM_OFF_MANUFACTURER_ID] = 0x2CU;
    test_put_le32(&page[NAND_ONFI_PARAM_OFF_DATA_BYTES_PER_PAGE], 2048U);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_SPARE_BYTES_PER_PAGE], 64U);
    test_put_le32(&page[NAND_ONFI_PARAM_OFF_DATA_BYTES_PER_PARTIAL], 512U);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_SPARE_BYTES_PER_PARTIAL], 16U);
    test_put_le32(&page[NAND_ONFI_PARAM_OFF_PAGES_PER_BLOCK], 64U);
    test_put_le32(&page[NAND_ONFI_PARAM_OFF_BLOCKS_PER_UNIT], 4096U);
    page[NAND_ONFI_PARAM_OFF_LOGICAL_UNITS]     = 0x01U;
    page[NAND_ONFI_PARAM_OFF_ADDRESS_CYCLES]    = 0x23U;
    page[NAND_ONFI_PARAM_OFF_BITS_PER_CELL]     = 0x01U;
    page[NAND_ONFI_PARAM_OFF_PROGRAMS_PER_PAGE] = 0x04U;
    page[NAND_ONFI_PARAM_OFF_ECC_BITS]          = 0x04U;
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_TIMING_MODE_SUPPORT], 0x003FU);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_PROGRAM_CACHE_TIMING], 0x003FU);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_TPROG_MAX], 600U);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_TBERS_MAX], 3000U);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_TR_MAX], 25U);
    test_put_le16(&page[NAND_ONFI_PARAM_OFF_TCCS_MIN], 100U);

    /* 签名匹配 → 返回 1，逐字段断言。 */
    munit_assert_uint8(nand_onfi_parameter_page_decode(page, &info), ==, 1U);
    munit_assert_uint8(info.signature[0], ==, 0x4FU);
    munit_assert_uint8(info.signature[1], ==, 0x4EU);
    munit_assert_uint8(info.signature[2], ==, 0x46U);
    munit_assert_uint8(info.signature[3], ==, 0x49U);
    munit_assert_uint16(info.revision, ==, 0x0002U);
    munit_assert_uint16(info.features, ==, 0x0018U);
    munit_assert_string_equal(info.model, "MT29F4G08ABADAWP");
    munit_assert_uint8(info.manufacturer_id, ==, 0x2CU);
    munit_assert_uint32(info.data_bytes_per_page, ==, 2048U);
    munit_assert_uint16(info.spare_bytes_per_page, ==, 64U);
    munit_assert_uint32(info.data_bytes_per_partial_page, ==, 512U);
    munit_assert_uint16(info.spare_bytes_per_partial_page, ==, 16U);
    munit_assert_uint32(info.pages_per_block, ==, 64U);
    munit_assert_uint32(info.blocks_per_unit, ==, 4096U);
    munit_assert_uint8(info.logical_units, ==, 1U);
    munit_assert_uint8(info.address_cycles, ==, 0x23U);
    munit_assert_uint8(info.bits_per_cell, ==, 1U);
    munit_assert_uint8(info.programs_per_page, ==, 4U);
    munit_assert_uint8(info.ecc_bits, ==, 4U);
    munit_assert_uint16(info.timing_mode_support, ==, 0x003FU);
    munit_assert_uint16(info.program_cache_timing, ==, 0x003FU);
    munit_assert_uint16(info.tprog_max, ==, 600U);
    munit_assert_uint16(info.tbers_max, ==, 3000U);
    munit_assert_uint16(info.tr_max, ==, 25U);
    munit_assert_uint16(info.tccs_min, ==, 100U);

    /* 证据（--log-visible info）：把解出的关键字段打出来。 */
    test_trace_summary("ONFI param decode: model=%s page=%lu+%u block=%lup blocks=%lu ecc=%ubit timingmodes=0x%04X", info.model, (unsigned long)info.data_bytes_per_page, (unsigned int)info.spare_bytes_per_page, (unsigned long)info.pages_per_block, (unsigned long)info.blocks_per_unit, (unsigned int)info.ecc_bits, (unsigned int)info.timing_mode_support);

    /* 破坏签名 → 返回 0，但字段仍被填满（解码函数如实报「签名不符」）。 */
    page[NAND_ONFI_PARAM_OFF_SIGNATURE] = 0x00U;
    munit_assert_uint8(nand_onfi_parameter_page_decode(page, &info), ==, 0U);
    munit_assert_uint32(info.data_bytes_per_page, ==, 2048U);

    return MUNIT_OK;
}

/**************************************************************************
 *  套件定义
 *
 *  单个子套件 logic，挂在 /fc-nand/ 根套件下，与 Level 2（data_path）/
 *  Level 3（lifecycle）一致——**一个文件一个套件**。
 *  原先按模块切成的 command / queue / scheduler 三个子套件已合并：合并后
 *  command 只剩 2 条、scheduler 只剩 1 条，过滤粒度已细到等于「单跑一条用例」，
 *  而 Level 1 全部 12 条都是纯 RAM 微秒级，按模块过滤省不出任何东西。
 *  真正需要过滤的边界是**依赖面**（零依赖 / 需池 / 需真 NAND），那由根套件的
 *  Level 1/2/3 三层保住。
 *
 *  套件不需要 setup/teardown——Level 1 全部用例自带夹具，零全局状态。
 **************************************************************************/

MunitTest g_fc_logic_tests[] = {
  /* Part 1: fc_command */
    {       "validate_op_range",        test_command_validate_op_range, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {"validate_set_get_no_desc", test_command_validate_set_get_no_desc, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 2: fc_submission_queue */
    {                "push_pop",                   test_queue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {             "wrap_around",                test_queue_wrap_around, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                "overflow",                   test_queue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {                    "peek",                       test_queue_peek, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {              "peek_empty",                 test_queue_peek_empty, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 2b: fc_completion_queue */
    {             "cqueue_init",                      test_cqueue_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "cqueue_push_pop",                  test_cqueue_push_pop, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},
    {         "cqueue_overflow",                  test_cqueue_overflow, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 3: fc_scheduler */
    {          "scheduler_init",                   test_scheduler_init, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

 /* Part 4: ONFI 参数页解码 */
    {  "onfi_param_page_decode",       test_onfi_parameter_page_decode, NULL, NULL, MUNIT_TEST_OPTION_SINGLE_ITERATION, NULL},

    {                      NULL,                                  NULL, NULL, NULL,             MUNIT_TEST_OPTION_NONE, NULL},
};
