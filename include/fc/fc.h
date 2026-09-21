#ifndef FC_H
#define FC_H

#include "fc_command.h"

#define FC_COMMAND_POOL_DEPTH    (64U)
#define FC_DESCRIPTOR_POOL_DEPTH (256U)
#define FC_BUFFER_POOL_DEPTH     (128U)
#define FC_BUFFER_POOL_CW_SIZE   (PAGE_SIZE / 4U)
#define FC_SQ_DEPTH              (64U)
#define FC_HSQ_DEPTH             (16U)
#define FC_CQ_DEPTH              (64U)

typedef struct
{
    uint16_t sq_used;
    uint16_t hsq_used;
    uint16_t cq_used;
    uint16_t command_used;
    uint16_t descriptor_used;
    uint16_t buffer_used;
} fc_resource_stats_t;

/* 初始化 FC 内部固定 Pool、SQ、HSQ、CQ 和调度器。 */
fc_status_t        fc_init(void);
fc_command_t      *fc_command_alloc(void);
fc_command_t      *fc_command_id_to_command(uint16_t command_id);
fc_descriptor_t   *fc_descriptor_id_to_descriptor(uint16_t descriptor_id);
fc_status_t        fc_command_configure_descriptors(fc_command_t *command);
fc_descriptor_id_t fc_descriptor_alloc(fc_command_t *command);
fc_buffer_id_t     fc_buffer_alloc(void);
fc_status_t        fc_buffer_release(fc_buffer_id_t buffer_id);
uint8_t           *fc_buffer_id_to_real_buffer(fc_buffer_id_t buffer_id);
fc_buffer_id_t     fc_real_buffer_to_buffer_id(const uint8_t *buffer);

/* 便利函数：将外部数据按 CW 大小依次拷贝到命令的所有 desc buffer 中。
 * 数据不够时剩余部分用 0xFF dummy 填充。 */
fc_status_t fc_write_cmd_data(fc_command_t *command, const void *data, uint32_t length);

/* 便利函数：获取命令中指定 CW 索引的 buffer 指针（读操作后使用）。 */
uint8_t *fc_get_cmd_data_ptr(fc_command_t *command, uint8_t cw_index);

/* high_priority=0 提交 SQ；high_priority=1 提交 HSQ。 */
fc_status_t fc_submit(fc_command_t *command);

/* 每次最多处理 budget 条已提交命令；budget=0 不执行任何命令。 */
fc_status_t fc_service(void);

/* FTL 从 CQ 取完成项后，可根据状态决定何时回收 Command/Descriptor/Buffer。 */
fc_status_t fc_poll_completion(fc_command_status_t *completion);
fc_status_t fc_command_release(fc_command_t *command);
fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats);

#endif /* FC_H */
