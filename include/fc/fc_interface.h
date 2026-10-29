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
 *   ③ buffer 访问       fc_buffer_alloc / release / id_to_real_buffer /
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

    /* fc_resource_stats_t 定义在 fc.h（本头 include 之） */

    /* ═══════ ① 初始化 ═══════ */
    /** @brief 初始化所有资源池和队列（幂等，可重复调用以重置） */
    fc_status_e fc_init(void);

    /* ═══════ ② 命令生命周期 ═══════ */
    /** @brief 申请命令，失败返回 NULL */
    fc_command_t *fc_command_alloc(void);

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
     * @return OK / RESOURCE_EMPTY（desc 或 buffer 池空）——只有「申请不到」才走返回码（模仿硬件行为）。**失败时链上可能已有 desc（含本条）**：本层不回滚，「已分配的 desc 全部在链上」是不变量，调用方沿链释放即完整回收（见下两个函数）。
     */
    fc_status_e fc_command_configure_descriptor(fc_command_t *command, BYTE plane, BYTE cw, DWORD row_paa);

    /**
     * @brief 建链入口（**上层申请 buffer**）——与上一个的唯一差别是 buffer 从哪来。
     * 仅当 command->config.buf_own 的申请位 = 上层申请（FC_BUF_OWN_ALLOC_BY_UPPER，即 10/11）
     * 时合法：调用者须先 `fc_buffer_alloc()` 拿到 buffer_id 再传进来，FC 建链时**不再申请**。
     * buf_own = FC 申请（00/01）时必须传 FC_BUFFER_ID_INVALID（否则是配置错误 → ASSERT）。
     * 其余前置、返回码、失败语义与 fc_command_configure_descriptor 完全一致。
     */
    fc_status_e fc_command_configure_descriptor_with_buffer(fc_command_t *command, BYTE plane, BYTE cw, DWORD row_paa, fc_buffer_id_t buffer_id);

    /**
     * @brief 释放链头那一个 desc（含其 buffer），返回新链头；空链（head 越界）原样返回。
     * 传链头 ID 而非命令：command->desc_id 是位域取不了地址，故按值传入、按值取回。
     * 用法：command->desc_id = fc_descriptor_chain_release_head(command->desc_id);
     */
    fc_descriptor_id_t fc_descriptor_chain_release_head(fc_descriptor_id_t head);

    /**
     * @brief 沿链释放**这条命令**的全部 desc + buffer，返回 FC_DESCRIPTOR_ID_INVALID（恒为空链头）。
     * "all" 指**本命令的 desc 链**（= 该命令已申请的全部资源），**不是**清空 desc/buffer 池，
     * 也不触及别的命令——链本身就是这条命令的资源集合（唯一真值）。
     * 建链中途失败的回滚入口——释放后命令只剩命令槽位，由 fc_command_release 收尾。
     * 用法：command->desc_id = fc_descriptor_chain_release_all(command->desc_id);
     */
    fc_descriptor_id_t fc_descriptor_chain_release_all(fc_descriptor_id_t head);

    /**
     * @brief 提交到 SQ 或 HSQ（根据 high_priority），入队即返回。
     * 前置（违反一律 ASSERT，不返回错误码）：指针在命令池内；状态 ALLOCATED；
     * validate 通过；desc 链所有权完整。配置错误绝不发到硬件。
     *
     * @return OK / QUEUE_FULL（队列满 = 资源，模仿硬件行为）。
     */
    fc_status_e fc_submit(fc_command_t *command);

    /**
     * @brief 批量提交：n 条命令要么全进 SQ/HSQ，要么一条都不进。
     * 供 two-plane cache program 批次使用——不允许“前几条已提交、末条失败”留下
     * 未终结的 cache sequence。前置与 fc_submit 相同；返回 OK / QUEUE_FULL。
     */
    fc_status_e fc_submit_batch(fc_command_t *const cmds[], DWORD n);

    /** @brief 调度执行（上层 while(1) 轮询调用） */
    fc_status_e fc_service(void);

    /** @brief 从 CQ 取完成项 */
    fc_status_e fc_poll_completion(fc_command_status_t *completion);

    /**
     * @brief 从 CQ 批量取完成项：最多取 max_count 条，实际条数写入 *count。
     * **上限由调用方给**（FC 不写死任何批量常量）——常见取值是 nand_dispatch_poll
     * 的推进上限 CH_NUM×CE_NUM×LUN_NUM，与单轮调度对齐；也可是任意 K。
     * 前置（违反一律 ASSERT）：completions 与 count 均非空。max_count 给多大都不会越界——
     * 循环被 CQ 占用数（≤ FC_CQ_DEPTH）天然封顶。
     *
     * @return 取到 ≥1 条 = OK；一条没取到 = QUEUE_EMPTY（含 max_count==0）。
     */
    fc_status_e fc_poll_completions(fc_command_status_t *completions, WORD max_count, WORD *count);

    /**
     * @brief 释放命令及关联 Descriptor 链 + 各 CW buffer（须已 COMPLETED 或 ALLOCATED 且不在 CQ 中）。
     * 前置（违反一律 ASSERT）：指针落在命令池内。**幂等**——已 FREE 时直接返回 OK。
     *
     * @return 恒 FC_STATUS_OK。
     */
    fc_status_e fc_command_release(fc_command_t *command);

    /* ═══════ ③ buffer 访问 ═══════ */
    /**
     * @brief 申请 Buffer（返回 ID），失败返回 FC_BUFFER_ID_INVALID。
     * ⚠️ 手动分配接口：常规路径由 fc_command_configure_descriptor() 为每个 CW 自动分配 desc + buffer。
     */
    fc_buffer_id_t fc_buffer_alloc(void);

    /** @brief 释放 Buffer。前置：buffer_id 须已占用（未占用 = 配置错误 → ASSERT）。返回：恒 OK。 */
    fc_status_e fc_buffer_release(fc_buffer_id_t buffer_id);

    /**
     * @brief ID→指针，返回 USER 数据区首地址（跳过 META+CRC 共 8B）。
     * 前置：buffer_id 须已占用（未占用 = 配置错误 → ASSERT，不再返回 NULL）。
     */
    BYTE *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id);

    /**
     * @brief FC 内部使用：返回 CW 基址（META 开头，528B 完整 CW）。
     * 数据路径（加扰/ECC/RAID）使用此接口。
     * 前置：buffer_id 须已占用（未占用 = 配置错误 → ASSERT，不再返回 NULL）。
     */
    BYTE *fc_buffer_get_base(fc_buffer_id_t buffer_id);

    /**
     * @brief 返回 CW 中 META 字段指针（4 字节，位于 CW 基址偏移 0）。
     * META 包含 FAA/PAA 信息，由 FC 在 program 前自动填写。
     * FTL/Test 在 read 完成后可通过此接口验证物理地址一致性。
     * cw_index 是 **CW 号**（按 paa_to_cw(desc->paa) 匹配），不是链走位。
     */
    BYTE *fc_buffer_get_meta(fc_command_t *command, BYTE cw_index);

    /* ═══════ ④ 数据查询 ═══════ */
    /**
     * @brief 获取命令中 **CW 号 == cw_index** 的 CW 的 buffer 指针（读操作后使用）。
     * 不在链上返回 NULL。
     */
    BYTE *fc_get_cmd_data_ptr(fc_command_t *command, BYTE cw_index);

    /**
     * @brief 从 desc 链重建命令的 flat CW 掩码（bit n = plane*CW_NUM + cw；无 desc 时 0）。
     * **链是 CW 集合的唯一真值**——FC 内没有掩码输入，需要"掩码形状"的
     * 判定（如 RAID 的 plane 对称性）一律走这里按链重建。
     */
    DWORD fc_get_cmd_cw_bitmap(const fc_command_t *command);

    /**
     * @brief 命令级聚合 ECC：desc 链上所有 CW 的 ecc_bits 最大值。
     * 须在 fc_command_release() 之前调用（desc 释放后失效）。
     */
    BYTE fc_get_cmd_ecc_max(const fc_command_t *command);

    /* ═══════ ⑤ 查询工具 ═══════ */
    /** @brief ID→指针转换。前置条件：command_id 为 fc_command_alloc() 分配出的池内槽位号。 */
    fc_command_t *fc_command_id_to_command(WORD command_id);

    /**
     * @brief 指针→ID 转换（fc_command_id_to_command 的逆）。返回 0 = command 不在命令池内
     * （例如栈上的 fc_command_t），*command_id 未定义。
     * ⚠ 命令 ID 由「指针 − command_pool 基址」导出，**不是 fc_command_t 的字段**
     * （见 fc_command.h）——命令释放、结构体整体清零之后，本函数依然能算出 ID。
     */
    BYTE fc_command_to_id(const fc_command_t *command, fc_command_id_t *command_id);

    /** @brief ID→指针转换。前置条件：descriptor_id 为命令 desc 链中的池内 ID。 */
    fc_descriptor_t *fc_descriptor_id_to_descriptor(WORD descriptor_id);

    /** @brief 资源使用统计（测试/监控用） */
    fc_status_e fc_get_resource_stats(fc_resource_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERFACE_H */
