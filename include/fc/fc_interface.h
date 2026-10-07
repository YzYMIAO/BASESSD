#ifndef FC_INTERFACE_H
#define FC_INTERFACE_H

/*
 * fc_interface.h — FC 对外接口声明（FTL / 主循环 / 测试 使用）
 *
 * 与 fc.h 的分工：
 *   fc.h             ：类型、宏、资源池配置、功能开关、资源统计结构
 *   fc_interface.h   ：对外函数声明（本文件）
 *   fc_interface.c   ：对外函数实现（单例 g_fc_controller 的薄壳）
 *   fc_internal.h    ：fc.c ↔ fc_interface.c 共享的内部签名（FTL 不得 include）
 *
 * 对外接口分组：
 *   ① 初始化            fc_init
 *   ② 命令生命周期       fc_command_alloc / attach_cw / fc_submit /
 *                        fc_poll_completion / fc_command_release
 *   ③ buffer 访问       fc_buffer_alloc / release / id_to_real_buffer /
 *                        fc_buffer_get_base / fc_buffer_get_meta
 *   ④ 数据查询          fc_get_cmd_data_ptr / fc_get_cmd_ecc_max
 *   ⑤ 查询工具          fc_command_id_to_command / fc_command_to_id /
 *                        fc_descriptor_id_to_descriptor / fc_get_resource_stats
 *   ⑥ 服务入口          fc_service
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数 → 逐条 fc_command_attach_cw() 组装 desc 链
 *   （FTL 侧「按掩码一次性建链」见 ftl_cmd_configure_descriptors()）
 *   → fc_submit() → fc_service()（后台推进）
 *   → fc_poll_completion() 取完成 → fc_command_release() 回收。
 *
 * 边界（v13）：FC = 硬件 FCC 的软件替身。**不含策略**——「访问哪些 CW」由 FTL 决定，
 *   「数据搬运」由 FTL 做（fc_command_attach_cw + 自己的拷贝），FC 只提供槽位/描述符/队列/数据路径。
 */

#include "fc.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* fc_resource_stats_t 定义在 fc.h（本头 include 之） */

    /* ═══════ ① 初始化 ═══════ */
    /* 初始化所有资源池和队列（幂等，可重复调用以重置） */
    fc_status_t fc_init(void);

    /* ═══════ ② 命令生命周期 ═══════ */
    /* 申请命令，失败返回 NULL */
    fc_command_t *fc_command_alloc(void);

    /* 逐条追加一个 (plane, cw) 到命令的 desc 链尾（分配 desc + buffer、填 PAA/config/meta）。
     * **建链的唯一入口**（v13：按掩码一次性建链的策略层已上移 FTL，见
     * ftl_cmd_configure_descriptors()）。上层自行组装 CW 集合（例如只读离散 CW）：
     * 按自己算出的集合循环调用即可，
     * **链序即数据布局顺序**（FTL 侧拷贝按此顺序遍历链，见 ftl_cmd_read_data）。
     * row_paa = 该 plane 的行地址（page/block/lun，CW 位被忽略），其 plane 位必须等于 plane。
     * 由此 multi-plane 命令的**两个 plane 可以指向不同 block**（Policy B：每 plane 独立分配块 +
     * 坏块替换）——每个 plane 传自己的 row_paa 即可；同一 plane 的所有 CW 必须传同一 row_paa。
     * plane 必须落在本命令的 plane 段内（单 plane = paa_to_plane(command->paa) 那一个；
     * multi-plane = [0, multi_plane)）。
     * 前置：命令处于 ALLOCATED。
     * 返回：OK / RESOURCE_EMPTY（池空，本条半成品由 release 统一回收）/ INVALID_ARGUMENT（坐标越界）。 */
    fc_status_t fc_command_attach_cw(fc_command_t *command, uint8_t plane, uint8_t cw, nand_paa_t row_paa);

    /* 提交到 SQ 或 HSQ（根据 high_priority），入队即返回 */
    fc_status_t fc_submit(fc_command_t *command);

    /* 调度执行（上层 while(1) 轮询调用） */
    fc_status_t fc_service(void);

    /* 从 CQ 取完成项 */
    fc_status_t fc_poll_completion(fc_command_status_t *completion);

    /* 释放命令及关联 Descriptor 链（须已 COMPLETED 或 ALLOCATED 且不在 CQ 中） */
    fc_status_t fc_command_release(fc_command_t *command);

    /* ═══════ ③ buffer 访问 ═══════ */
    /* 申请 Buffer（返回 ID），失败返回 FC_BUFFER_ID_INVALID。
     * ⚠️ 手动分配接口：常规路径由 fc_command_attach_cw() 为每个 CW 自动分配 desc + buffer。 */
    fc_buffer_id_t fc_buffer_alloc(void);

    /* 释放 Buffer（须已占用） */
    fc_status_t fc_buffer_release(fc_buffer_id_t buffer_id);

    /* ID→指针，返回 USER 数据区首地址（跳过 META+CRC 共 8B）；无效 ID 返回 NULL */
    uint8_t *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id);

    /* FC 内部使用：返回 CW 基址（META 开头，528B 完整 CW）。
       数据路径（加扰/ECC/RAID）使用此接口。 */
    uint8_t *fc_buffer_get_base(fc_buffer_id_t buffer_id);

    /* 返回 CW 中 META 字段指针（4 字节，位于 CW 基址偏移 0）。
       META 包含 FAA/PAA 信息，由 FC 在 program 前自动填写。
       FTL/Test 在 read 完成后可通过此接口验证物理地址一致性。
       cw_index 是 **CW 号**（按 paa_to_cw(desc->paa) 匹配），不是链走位。 */
    uint8_t *fc_buffer_get_meta(fc_command_t *command, uint8_t cw_index);

    /* ═══════ ④ 数据查询 ═══════ */
    /* 获取命令中 **CW 号 == cw_index** 的 CW 的 buffer 指针（读操作后使用）。
     * 不在链上返回 NULL。 */
    uint8_t *fc_get_cmd_data_ptr(fc_command_t *command, uint8_t cw_index);

    /* 从 desc 链重建命令的 flat CW 掩码（bit n = plane*CW_NUM + cw；无 desc 时 0）。
     * **链是 CW 集合的唯一真值**——v13 起 FC 内不再有掩码输入，需要"掩码形状"的
     * 判定（如 RAID 的 plane 对称性）一律走这里按链重建，不回读 command->param.cw_mask。 */
    uint32_t fc_get_cmd_cw_mask(const fc_command_t *command);

    /* 命令级聚合 ECC：desc 链上所有 CW 的 ecc_bits 最大值（替代 CQ 已删除的
     * ecc_bit0..3）。须在 fc_command_release() 之前调用（desc 释放后失效）。 */
    uint8_t fc_get_cmd_ecc_max(const fc_command_t *command);

    /* ═══════ ⑤ 查询工具 ═══════ */
    /* ID→指针转换。前置条件：command_id 为 fc_command_alloc() 分配出的池内槽位号。 */
    fc_command_t *fc_command_id_to_command(uint16_t command_id);

    /* 指针→ID 转换（fc_command_id_to_command 的逆）。返回 0 = command 不在命令池内
     * （例如栈上的 fc_command_t），*command_id 未定义。
     * ⚠ 命令 ID 由「指针 − command_pool 基址」导出，**不是 fc_command_t 的字段**
     *   （见 fc_command.h）——命令释放、结构体整体清零之后，本函数依然能算出 ID。 */
    uint8_t fc_command_to_id(const fc_command_t *command, fc_command_id_t *command_id);

    /* ID→指针转换。前置条件：descriptor_id 为命令 desc 链中的池内 ID。 */
    fc_descriptor_t *fc_descriptor_id_to_descriptor(uint16_t descriptor_id);

    /* 资源使用统计（测试/监控用） */
    fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* FC_INTERFACE_H */
