#ifndef FC_INTERFACE_H
#define FC_INTERFACE_H

/*
 * fc_interface.h — FC 对外接口声明（FTL / 主循环 / 测试 使用）
 *
 * 与 fc.h 的分工：
 *   fc.h             ：类型、宏、资源池配置、功能开关、资源统计结构
 *   fc_interface.h   ：对外函数声明（本文件）
 *   fc_interface.c   ：对外函数实现（单例 g_fc_controller；建链机制体在此）
 *   fc_internal.h    ：fc.c ↔ fc_interface.c 共享的内部签名（FTL 不得 include）
 *
 * 对外接口分组：
 *   ① 初始化            fc_init
 *   ② 命令生命周期       fc_command_alloc / configure_descriptor / fc_submit /
 *                        fc_poll_completion / fc_command_release
 *   ③ buffer 访问       fc_buffer_alloc / release / get_user_data /
 *                        fc_buffer_get_base / fc_buffer_get_meta
 *   ④ 数据查询          fc_get_cmd_data_ptr / fc_get_cmd_ecc_max
 *   ⑤ 查询工具          fc_command_id_to_command / fc_command_to_id /
 *                        fc_descriptor_id_to_descriptor / fc_get_resource_stats
 *   ⑥ 服务入口          fc_service
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数 → 逐条 fc_command_configure_descriptor() 组装 desc 链
 *   （FTL 侧「按掩码一次性建链」见 ftl_cmd_configure_descriptors()）
 *   → fc_submit() → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release() 回收。
 *
 * 边界：FC = 硬件 FCC 的软件替身。**不含策略**——「访问哪些 CW」由 FTL 决定，
 *   「数据搬运」由 FTL 做（fc_command_configure_descriptor + 自己的拷贝），FC 只提供槽位/描述符/队列/数据路径。
 */

#include "fc.h"
#include "nand_config.h" /* DWORD（对外函数的行地址形参） */

#ifdef __cplusplus
extern "C"
{
#endif

    /* fc_resource_stats_s 定义在 fc.h（本头 include 之） */

    /* ═══════ ① 初始化 ═══════ */
    /**
     * @brief 初始化所有资源池和队列（幂等，可重复调用以重置）。
     *
     * @return FC_STATUS_OK        初始化成功。
     *         FC_STATUS_IO_ERROR  NAND 派发层初始化失败。
     */
    fc_status_e fc_init(void);

    /* ═══════ ② 命令生命周期 ═══════ */
    /**
     * @brief 申请一个命令槽位。
     *
     * @return 命令指针；命令池空时返回 NULL。
     */
    fc_cmd_s *fc_command_alloc(void);

    /**
     * @brief 逐条追加一个 (plane, cw) 到命令的 desc 链尾（分配 desc + buffer、填 PAA/config/meta）。
     * **建链的唯一入口**：按掩码一次性建链的策略层在 FTL（见
     * ftl_cmd_configure_descriptors()），本函数即机制体。
     * 上层自行组装 CW 集合（例如只读离散 CW）：
     * 按自己算出的集合循环调用即可，
     * **链序即数据布局顺序**（FTL 侧拷贝按此顺序遍历链，见 ftl_cmd_read_data）。
     * row_paa = 该 plane 的行地址（**PAA**：page/block/lun，plane/cw 位由实参决定），
     * 其 plane 位必须等于 plane。本入口收 PAA——FC 层内没有 PBA，
     * 调用方（FTL）自行 `pba_to_paa()` 后传入。
     * 由此 multi-plane 命令的**两个 plane 可以指向不同 block**（Policy B：每 plane 独立分配块 +
     * 坏块替换）——每个 plane 传自己的 row_paa 即可；同一 plane 的所有 CW 必须传同一 row_paa。
     * plane 必须落在本命令的 plane 段内（单 plane = paa_to_plane(command->paa) 那一个；
     * multi-plane = [0, multi_plane)）。
     * 前置（违反一律 ASSERT，不返回错误码）：命令处于 ALLOCATED；plane 落在本命令 plane 段内；
     * CW 号 < CW_NUM；row_paa 的 plane 位 == plane。配置错误绝不发到硬件。
     *
     * @param command 目标命令；须已 ALLOCATED。
     * @param plane   CW 所在 plane；须落在本命令的 plane 段内。
     * @param cw      CW 号；须 < CW_NUM。
     * @param row_paa 该 plane 的行地址（PAA）；其 plane 位必须等于 plane。
     *
     * @return FC_STATUS_OK             建链成功。
     *         FC_STATUS_RESOURCE_EMPTY desc 或 buffer 池空——只有「申请不到」才走返回码（模仿硬件行为）。
     *                                  **失败时链上可能已有 desc（含本条）**：本层不回滚，「已分配的 desc 全部在链上」是不变量，调用方沿链释放即完整回收（见下两个函数）。
     */
    fc_status_e fc_command_configure_descriptor(fc_cmd_s *command, BYTE plane, BYTE cw, DWORD row_paa);

    /**
     * @brief 建链入口（**上层申请 buffer**）——与上一个的唯一差别是 buffer 从哪来。
     * 仅当 command->config.buf_own 的申请位 = 上层申请（FC_BUF_OWN_ALLOC_BY_UPPER，即 10/11）
     * 时合法：调用者须先 `fc_buffer_alloc()` 拿到 buffer_id 再传进来，FC 建链时**不申请**。
     * buf_own = FC 申请（00/01）时必须传 FC_BUF_ID_INVALID（否则是配置错误 → ASSERT）。
     * 其余前置与失败语义与 fc_command_configure_descriptor 一致。
     *
     * @param command   目标命令；须已 ALLOCATED。
     * @param plane     CW 所在 plane；须落在本命令的 plane 段内。
     * @param cw        CW 号；须 < CW_NUM。
     * @param row_paa   该 plane 的行地址（PAA）；其 plane 位必须等于 plane。
     * @param buffer_id 上层已申请的 buffer ID；buf_own 为 FC 申请时须传 FC_BUF_ID_INVALID。
     *
     * @return FC_STATUS_OK             建链成功。
     *         FC_STATUS_RESOURCE_EMPTY desc 池空，或 FC 申请 buffer 时 buffer 池空。
     */
    fc_status_e fc_command_configure_descriptor_with_buffer(fc_cmd_s *command, BYTE plane, BYTE cw, DWORD row_paa, fc_buf_id_t buffer_id);

    /**
     * @brief 释放链头那一个 desc（含其 buffer），返回新链头；空链（head 越界）原样返回。
     * 传链头 ID 而非命令：command->desc_id 是位域取不了地址，故按值传入、按值取回。
     * 用法：command->desc_id = fc_descriptor_chain_release_head(command->desc_id);
     *
     * @param head 当前链头 ID。
     *
     * @return 释放后的新链头；空链（head 越界）时原样返回 head。
     */
    fc_desc_id_t fc_descriptor_chain_release_head(fc_desc_id_t head);

    /**
     * @brief 沿链释放**这条命令**的全部 desc + buffer，返回 FC_DESC_ID_INVALID（恒为空链头）。
     * "all" 指**本命令的 desc 链**（= 该命令已申请的全部资源），**不是**清空 desc/buffer 池，
     * 也不触及别的命令——链本身就是这条命令的资源集合（唯一真值）。
     * 建链中途失败的回滚入口——释放后命令只剩命令槽位，由 fc_command_release 收尾。
     * 用法：command->desc_id = fc_descriptor_chain_release_all(command->desc_id);
     *
     * @param head 当前链头 ID。
     *
     * @return 恒为 FC_DESC_ID_INVALID（空链头）。
     */
    fc_desc_id_t fc_descriptor_chain_release_all(fc_desc_id_t head);

    /**
     * @brief 提交到 SQ 或 HSQ（根据 high_priority），入队即返回。
     * 前置（违反一律 ASSERT，不返回错误码）：指针在命令池内；状态 ALLOCATED；
     * validate 通过；desc 链所有权完整。
     *
     * @param command 待提交命令；须在命令池内且状态 ALLOCATED。
     *
     * @return FC_STATUS_OK         入队成功。
     *         FC_STATUS_QUEUE_FULL 目标队列满（资源，模仿硬件行为）。
     */
    fc_status_e fc_submit(fc_cmd_s *command);

    /**
     * @brief 批量提交：n 条命令要么全进 SQ/HSQ，要么一条都不进。
     * 供 two-plane cache program 批次使用——不允许“前几条已提交、末条失败”留下
     * 未终结的 cache sequence。前置与 fc_submit 相同。
     *
     * 链契约：批内只要含 `FC_CHAIN_MORE` 成员，全批必须同优先级（进同一条队列），
     * 否则链会被拆到 SQ/HSQ 两段（序列错乱）。末条**允许** `FC_CHAIN_MORE`——
     * 这是流式序列的中间窗口；FTL 的序列状态负责在 run_end 发 `FC_CHAIN_END`，
     * `chain_owner` 跨窗口保留。
     *
     * @param cmds 命令指针数组，长度为 n。
     * @param n    命令条数；须 > 0。
     *
     * @return FC_STATUS_OK               全部入队。
     *         FC_STATUS_QUEUE_FULL       SQ 或 HSQ 余量不足（一条都不入队）。
     *         FC_STATUS_INVALID_ARGUMENT 批内含 chain 成员但优先级不一致。
     */
    fc_status_e fc_submit_batch(fc_cmd_s *const cmds[], DWORD n);

    /**
     * @brief 调度执行（上层 while(1) 轮询调用）。
     *
     * @return FC_STATUS_OK          本轮有完成被消费，或有命令被下发。
     *         FC_STATUS_QUEUE_EMPTY 无完成且 SQ/HSQ 均为空。
     *         FC_STATUS_NOT_READY   无完成，队列非空但当前无 LUN 可下发（背压）。
     *         FC_STATUS_IO_ERROR    CQ 满或命令下发失败。
     */
    fc_status_e fc_service(void);

    /**
     * @brief 从 CQ 取一条完成项。
     *
     * @param completion 完成项出参；须非空。
     *
     * @return FC_STATUS_OK          取到一条完成项。
     *         FC_STATUS_QUEUE_EMPTY CQ 为空。
     */
    fc_status_e fc_poll_completion(fc_cmd_status_s *completion);

    /**
     * @brief 从 CQ 批量取完成项：最多取 max_count 条，实际条数写入 *count。
     * **上限由调用方给**（FC 不写死任何批量常量）——常见取值是 fc_nand_dispatch_poll
     * 的推进上限 CH_NUM×CE_NUM×LUN_NUM，与单轮调度对齐；也可是任意 K。
     * 前置（违反一律 ASSERT）：completions 与 count 均非空。max_count 给多大都不会越界——
     * 循环被 CQ 占用数（≤ FC_CQ_DEPTH）天然封顶。
     *
     * @param completions 完成项输出数组；须非空。
     * @param max_count   最多取多少条；0 合法（此时必返回 QUEUE_EMPTY）。
     * @param count       实际取到的条数（出参）；须非空。
     *
     * @return FC_STATUS_OK          取到 ≥1 条。
     *         FC_STATUS_QUEUE_EMPTY 一条没取到（含 max_count == 0）。
     */
    fc_status_e fc_poll_completions(fc_cmd_status_s *completions, WORD max_count, WORD *count);

    /**
     * @brief 释放命令及关联 Descriptor 链 + 各 CW buffer（FREE 幂等）。
     *
     * 指针身份是配置错误 ⇒ ASSERT；生命周期由调用链保证（未提交 / 完成项已取走）。
     *
     * @param command 待释放命令；指针须落在命令池内。
     */
    void fc_command_release(fc_cmd_s *command);

    /* ═══════ ③ buffer 访问 ═══════ */
    /**
     * @brief 申请 Buffer（返回 ID），失败返回 FC_BUF_ID_INVALID。
     * ⚠️ 手动分配接口：常规路径由 fc_command_configure_descriptor() 为每个 CW 自动分配 desc + buffer。
     *
     * @return buffer ID；buffer 池空时返回 FC_BUF_ID_INVALID。
     */
    fc_buf_id_t fc_buffer_alloc(void);

    /**
     * @brief 释放上层预申请的 Buffer（尚未挂到命令上）。
     *
     * 越界 / 未占用 / 仍归属某条命令是配置错误 ⇒ ASSERT；已挂链 buffer 必须随
     * 命令释放（fc_command_release），不能在这里释放。
     *
     * @param buffer_id 待释放的 buffer ID。
     */
    void fc_buffer_release(fc_buf_id_t buffer_id);

    /**
     * @brief ID→指针，返回 USER 数据区首地址（跳过 META+CRC 共 8B）。
     *
     * @param buffer_id 已占用的 buffer ID。
     *
     * @return USER 数据区首地址（CW 基址 + FC_BUFFER_USER_OFFSET，恒非空）。
     */
    BYTE *fc_buffer_get_user_data(fc_buf_id_t buffer_id);

    /**
     * @brief FC 内部使用：返回 CW 基址（META 开头，528B 完整 CW）。
     * 数据路径（加扰/ECC/RAID）使用此接口。
     *
     * @param buffer_id 已占用的 buffer ID。
     *
     * @return CW 基址指针（恒非空）。
     */
    BYTE *fc_buffer_get_base(fc_buf_id_t buffer_id);

    /**
     * @brief 返回 CW 中 META 字段指针（4 字节，位于 CW 基址偏移 0）。
     * META 包含 FAA/PAA 信息，由 FC 在 program 前自动填写。
     * FTL/Test 在 read 完成后可通过此接口验证物理地址一致性。
     * cw_index 是 **CW 号**（按 paa_to_cw(desc->paa) 匹配），不是链走位。
     *
     * @param command  命令；须非空。
     * @param cw_index CW 号。
     *
     * @return META 字段指针（4 字节）；链上无该 CW，或该 desc 没有 buffer 时返回 NULL。
     */
    BYTE *fc_buffer_get_meta(fc_cmd_s *command, BYTE cw_index);

    /* ═══════ ④ 数据查询 ═══════ */
    /**
     * @brief 获取命令中 **CW 号 == cw_index** 的 CW 的 buffer 指针（读操作后使用）。
     * 不在链上返回 NULL。
     *
     * @param command  命令；须非空且 desc 链非空。
     * @param cw_index CW 号。
     *
     * @return 该 CW 的 USER 数据指针；不在链上或该 desc 无 buffer 时返回 NULL。
     */
    BYTE *fc_get_cmd_data_ptr(fc_cmd_s *command, BYTE cw_index);

    /**
     * @brief 从 desc 链重建命令的 flat CW 掩码（bit n = plane*CW_NUM + cw；无 desc 时 0）。
     * **链是 CW 集合的唯一真值**——FC 内没有掩码输入，需要"掩码形状"的
     * 判定（如 RAID 的 plane 对称性）一律走这里按链重建。
     *
     * @param command 命令；须非空。
     *
     * @return flat CW 掩码；无 desc 时为 0。
     */
    DWORD fc_get_cmd_cw_bitmap(const fc_cmd_s *command);

    /**
     * @brief 命令级聚合 ECC：desc 链上所有 CW 的 ecc_bits 最大值。
     * 须在 fc_command_release() 之前调用（desc 释放后失效）。
     *
     * @param command 命令；须非空。
     *
     * @return desc 链上所有 CW 的 ecc_bits 最大值；无 desc 时为 0。
     */
    BYTE fc_get_cmd_ecc_max(const fc_cmd_s *command);

    /**
     * @brief 命令读到的所有 CW 是否都是**擦除页**（desc 链上每个 CW 都是 FC_CW_STATUS_EMPTY）。
     *
     * 这是「这一页编程过没有」的**唯一权威判据**：空/非空的依据（ECC 是否成码字、
     * parity 是否写成）只有 FC 看得见，FTL 只拿得到 USER 区。
     * 须在 fc_command_release() 之前调用（desc 释放后失效）。
     *
     * @param command 命令；须非空，且是读命令（写命令的 desc 状态不表示介质内容）。
     *
     * @return true  = 链上每个 CW 都判为擦除页
     *         false = 至少一个 CW 不是（含解码成功 / UNC / 读失败）
     */
    BOOL fc_get_cmd_all_cw_empty(const fc_cmd_s *command);

    /* ═══════ ⑤ 查询工具 ═══════ */
    /**
     * @brief ID→指针转换。前置条件：command_id 为 fc_command_alloc() 分配出的池内槽位号。
     *
     * @param command_id 池内命令槽位号。
     *
     * @return 命令指针（恒非空）。
     */
    fc_cmd_s *fc_command_id_to_command(WORD command_id);

    /**
     * @brief 指针→ID 转换（fc_command_id_to_command 的逆）。返回 0 = command 不在命令池内
     * （例如栈上的 fc_cmd_s），*command_id 未定义。
     * ⚠ 命令 ID 由「指针 − command_pool 基址」导出，**不是 fc_cmd_s 的字段**
     * （见 fc_command.h）——命令释放、结构体整体清零之后，本函数依然能算出 ID。
     *
     * @param command    命令指针。
     * @param command_id ID 出参；返回 1 时写入。
     *
     * @return true = command 在命令池内；false = 不在池内。
     */
    BOOL fc_command_to_id(const fc_cmd_s *command, fc_cmd_id_t *command_id);

    /**
     * @brief ID→指针转换。前置条件：descriptor_id 为命令 desc 链中的池内 ID。
     *
     * @param descriptor_id 池内描述符 ID。
     *
     * @return 描述符指针（恒非空）。
     */
    fc_desc_s *fc_descriptor_id_to_descriptor(WORD descriptor_id);

    /* ── 诊断：队列头 / 链锁快照（只读，不改状态）──
     * 排障用：用例/运维在「FTL 回不到 idle」时打印它，看是哪个队列的头卡住、
     * 以及该 LUN 的链锁归谁（chain_owner）。生产路径不调用。 */
    typedef struct FC_DEBUG_STATE_S
    {
        WORD  sq_used;
        WORD  hsq_used;
        BYTE  chain_owner; /* 0=NONE/1=SQ/2=HSQ（内部枚举见 fc_internal.h） */
        WORD  sq_head_id;
        WORD  sq_head_chain;
        BYTE  sq_head_op;
        WORD  hsq_head_id;
        WORD  hsq_head_chain;
        BYTE  hsq_head_op;
        BYTE  lun_used;       /* NAND per-LUN FIFO 已用槽 */
        BYTE  lun_phase;      /* NAND per-LUN FSM 相位（LUN_PHASE_*） */
        DWORD fill_calls;     /* FC 填充轮次累计调用次数 */
        DWORD fill_pops;      /* 填充轮次累计从队列 pop 的命令数 */
        BYTE  sq_head_state;  /* SQ 头命令的状态（fc_cmd_state_e：0=FREE 1=ALLOCATED 2=QUEUED 3=ACTIVE 4=COMPLETED） */
        WORD  sched_sq_count; /* 调度器实际扫描的 SQ 的 count（应与 sq_used 相同） */
        BYTE  sched_sq_bound; /* 1 = scheduler->sq/hsq 就绑在 controller->sq/hsq 上 */
    } fc_debug_state_s;

    /**
     * @brief 取队列头 / 链锁快照（只读）。
     *
     * @param out 出参；须非空。
     */
    void fc_debug_state_get(fc_debug_state_s *out);

    /**
     * @brief 资源使用统计（测试/监控用）。
     *
     * used 字段来自分配/释放闸门的 O(1) 计数；high 字段是历史最高占用（高水位）。
     * debug 档会额外扫描全池校验计数与真值一致，release 档只读计数器。
     *
     * @param stats 统计结果出参；须非空。
     */
    void fc_get_resource_stats(fc_resource_stats_s *stats);

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERFACE_H */
