#ifndef FC_H
#define FC_H

#include "fc_command.h"

/*
 * fc.h — FC 层对外接口（命令池 / 缓冲池 / 调度 / 服务入口）
 *
 * 核心资源：
 *   - Command Pool（FC_COMMAND_POOL_DEPTH=64）：命令槽位
 *   - Descriptor Pool（FC_DESCRIPTOR_POOL_DEPTH=256）：CW 描述符
 *   - Buffer Pool（FC_BUFFER_POOL_DEPTH=128）：CW 数据缓冲
 *
 * 生命周期（对 FTL）：
 *   fc_command_alloc() → 填 config/参数 → 逐条 fc_command_configure_descriptor() 组装 desc 链
 *   （FTL 侧「按掩码一次性建链」见 ftl_cmd_configure_descriptors()）
 *   → fc_submit() → fc_service()（后台推进）→ fc_poll_completion() 取完成
 *   → fc_command_release() 回收。
 *
 * buffer 布局（528B CW）：
 *   [META:4][CRC:4][USER:512][PARITY:8]
 *   对外只暴露 USER 区（fc_buffer_id_to_real_buffer 返回 offset=8 处）。
 *
 * 文件分工：
 *   fc.h（本文件）        ：类型、宏、资源池配置、功能开关、资源统计结构
 *   fc_interface.h        ：对外函数声明（本文件 include 之，二者合一对外）
 *   fc_interface.c        ：对外函数实现（单例 g_fc_controller；建链机制体在此）
 *   fc_internal.h         ：fc.c ↔ fc_interface.c 共享的内部签名（FTL 不得 include）
 *
 * 接口分组（全部声明在 fc_interface.h）：
 *   ① 初始化      fc_init
 *   ② 命令生命周期 fc_command_alloc / configure_descriptor / submit /
 *                  service / poll_completion / command_release
 *   ③ buffer 访问 fc_buffer_alloc / release / id_to_real_buffer / get_base / get_meta
 *   ④ 数据查询    fc_get_cmd_data_ptr / fc_get_cmd_ecc_max
 *   ⑤ 查询工具    fc_command_id_to_command / fc_command_to_id /
 *                  fc_descriptor_id_to_descriptor / fc_get_resource_stats
 */

#include "fc_interface.h"

/* ── 功能特性开关 ── */

/*
 * Stripe RAID 功能使能。
 *   0 = 禁用（默认）。RAID 相关代码不编译，fc_cmd_conf_t.config.raid 固定为 NORMAL。
 *   1 = 启用。跨 Plane RAID XOR parity generation + recovery。
 *
 * 当前未启用（功能代码已完成但未验证，待硬件就绪后开启）。
 */
#ifndef CONFIG_FC_RAID
#    define CONFIG_FC_RAID (0U)
#endif

/* 固定资源池深度 */
#define FC_COMMAND_POOL_DEPTH    (64U)
#define FC_DESCRIPTOR_POOL_DEPTH (256U)
#define FC_BUFFER_POOL_DEPTH     (128U)
#define FC_BUFFER_POOL_CW_SIZE   (CW_SIZE) /* 528B = 每 CW 缓冲大小（= NAND 侧 CW_SIZE） */

/*
 * CW 布局: [META:4][CRC:4][USER:512][PARITY:8] = 528B
 * 对外接口只暴露 USER 数据区，META/CRC/PARITY 由 FC 内部管理。
 */
#define FC_BUFFER_USER_OFFSET (8U)   /* USER 区域在 CW 中的偏移 */
#define FC_BUFFER_USER_SIZE   (512U) /* 暴露给 FTL 的用户数据大小 */
#define FC_BUFFER_DATA_SIZE   (520U) /* META+CRC+USER = 加扰/ECC 处理的数据区大小 */

/* 队列深度 */
#define FC_SQ_DEPTH  (64U)
#define FC_HSQ_DEPTH (16U)
#define FC_CQ_DEPTH  (64U)

/* 资源使用统计（测试/监控用）：定义见 fc_types.h */
#endif /* FC_H */
