#ifndef SSD_FIRMWARE_BOOT_H
#define SSD_FIRMWARE_BOOT_H

#include "common/ssd_types.h"

#include <stdint.h>

/* 启动阶段用于日志、故障定位以及限制主机接口过早进入 Ready。 */
typedef enum
{
    FIRMWARE_BOOT_STAGE_RESET = 0,
    FIRMWARE_BOOT_STAGE_PLATFORM,
    FIRMWARE_BOOT_STAGE_MEMORY,
    FIRMWARE_BOOT_STAGE_IMAGE,
    FIRMWARE_BOOT_STAGE_STORAGE,
    FIRMWARE_BOOT_STAGE_HOST,
    /* 所有真实硬件能力均完成后才能使用 READY。 */
    FIRMWARE_BOOT_STAGE_READY,
    /* 参考平台流程已走通，但仍存在由 placeholder_mask 指示的占位能力。 */
    FIRMWARE_BOOT_STAGE_PLACEHOLDER_READY,
    FIRMWARE_BOOT_STAGE_FAILED
} firmware_boot_stage_t;

/*
 * 这些标志表示流程已经保留，但参考 SoC 尚无真实硬件实现。
 * 返回成功仅允许软件架构继续运行，不能解释为对应硬件已经验证。
 */
typedef enum
{
    FIRMWARE_PLACEHOLDER_RESET_REASON = (1UL << 0),
    FIRMWARE_PLACEHOLDER_MPU_CACHE    = (1UL << 1),
    FIRMWARE_PLACEHOLDER_DDR          = (1UL << 2),
    FIRMWARE_PLACEHOLDER_IMAGE_VERIFY = (1UL << 3),
    FIRMWARE_PLACEHOLDER_NAND         = (1UL << 4),
    FIRMWARE_PLACEHOLDER_FTL_RECOVERY = (1UL << 5),
    FIRMWARE_PLACEHOLDER_PCIE_NVME    = (1UL << 6)
} firmware_placeholder_t;

typedef struct
{
    firmware_boot_stage_t stage;
    uint32_t              placeholder_mask;
    int                   last_status;
} firmware_boot_context_t;

/* 初始化启动上下文，不访问任何尚未初始化的外设。 */
void firmware_boot_context_init(firmware_boot_context_t *context);

/* 按平台、内存、镜像、存储、主机接口的顺序执行完整启动流程。 */
int firmware_boot_execute(firmware_boot_context_t *context);

#endif /* SSD_FIRMWARE_BOOT_H */
