#ifndef FC_H
#define FC_H

#include "fc_command.h"

/*
 * fc.h — FC 层对外接口（命令池 / 缓冲池 / 调度 / 服务入口）
 *
 * 核心资源：
 *   - Command Pool（FC_COMMAND_POOL_DEPTH）：命令槽位
 *   - Descriptor Pool（FC_DESCRIPTOR_POOL_DEPTH）：CW 描述符
 *   - Buffer Pool（FC_BUFFER_POOL_DEPTH）：CW 数据缓冲
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc(&cmd_id) → 填 config/参数 → 逐条
 *   fc_command_configure_descriptor(cmd_id, ...) 组装 desc 链
 *   （FTL 侧「按掩码一次性建链」见 ftl_cmd_configure_descriptors()）
 *   → fc_submit(cmd_id) → fc_service()（后台推进）→ fc_poll_completion() 取完成
 *   → fc_command_release(cmd_id) 回收。
 *
 * buffer 布局（528B CW）：
 *   [META:4][CRC:4][USER:512][PARITY:8]
 *   对外只暴露 USER 区（fc_buffer_get_user_data 返回 offset=8 处）。
 *
 * 文件分工：
 *   fc.h（本文件）        ：公共 umbrella，只 include 公共头；不定义配置/函数
 *   fc_interface.h        ：对外 API + 公共资源池/队列/布局配置
 *   fc.c / fc_pool.c      ：实现（调度/完成/服务 + 三池/生命周期）
 *
 * 接口分组（全部声明在 fc_interface.h）：
 *   ① 初始化      fc_init
 *   ② 命令生命周期 fc_command_alloc / configure_descriptor[_with_buffer] /
 *                  descriptor_release_head / descriptor_chain_release_all /
 *                  submit / submit_batch / poll_completion[_s] / command_release
 *   ③ buffer 访问 fc_buffer_alloc / release / get_user_data / get_base / get_meta
 *   ④ 数据查询    fc_get_cmd_data_ptr / fc_get_cmd_cw_bitmap /
 *                  fc_get_cmd_ecc_max / fc_get_cmd_all_cw_empty
 *   ⑤ 查询工具    fc_command_id_to_command / fc_command_to_id /
 *                  fc_descriptor_id_to_descriptor / fc_get_resource_stats /
 *                  fc_debug_state_get
 *   ⑥ 服务入口    fc_service
 */

#include "fc_command.h"
#include "fc_interface.h"

#endif /* FC_H */
