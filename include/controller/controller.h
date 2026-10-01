#ifndef SSD_CONTROLLER_H
#define SSD_CONTROLLER_H

#include "controller/ssd_request.h"
#include "ftl/ftl.h"

typedef enum
{
    SSD_CONTROLLER_RESET = 0,
    SSD_CONTROLLER_INITIALIZING,
    SSD_CONTROLLER_READY,
    SSD_CONTROLLER_FAILED
} ssd_controller_state_t;

typedef struct
{
    ssd_controller_state_t state;

    /* FTL 实例三维路由表：ftl[ch][ce][lun] 指向绑定该 (ch,ce,lun) 的 FTL 实例。
     * CH_NUM/CE_NUM/LUN_NUM 来自 nand_config.h（当前全 1 = 单实例折叠，仅 4B）。 */
    ftl_context_t *ftl[CH_NUM][CE_NUM][LUN_NUM];
} ssd_controller_t;

/*
 * ssd_controller_init - 初始化 Controller，绑定 FTL 实例三维表
 *
 * 形参按 C 数组退化规则实为指向 [CE_NUM][LUN_NUM] 子数组的指针；
 * 每个槽位必须非空，且槽位索引须与实例 topo.ch/ce/lun 一致
 *（保证 LBA 路由与芯片定位语义一致）。
 *
 * @param controller Controller 上下文
 * @param ftl        FTL 实例三维数组 ftl[CH_NUM][CE_NUM][LUN_NUM]
 * @return SSD_STATUS_OK 成功，SSD_STATUS_INVALID_ARGUMENT 入参非法/槽位缺失/拓扑不符
 */
int      ssd_controller_init(ssd_controller_t *controller,
                             ftl_context_t *ftl[CH_NUM][CE_NUM][LUN_NUM]);
int      ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request);
uint8_t  ssd_controller_is_complete(const ssd_request_t *request);

#endif /* SSD_CONTROLLER_H */
