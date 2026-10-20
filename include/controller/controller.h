#ifndef SSD_CONTROLLER_H
#define SSD_CONTROLLER_H

#include "ftl.h"
#include "ssd_request.h"

#include <stdbool.h>

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
ssd_status_t ssd_controller_init(ssd_controller_t *controller,
                                 ftl_context_t    *ftl[CH_NUM][CE_NUM][LUN_NUM]);
ssd_status_t ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request);
BYTE         ssd_controller_is_complete(const ssd_request_t *request);

/*
 * ssd_controller_lba_range_ok - host LBA 区间是否落在设备容量内
 *
 * 唯一实现：CLI 与 USB MSC 两个 host 前端在提交前共用（C2/R9）。
 * 区间为 [lba, lba + count)，count > 0。
 *
 * @param lba    起始 LBA（host 全局坐标）
 * @param count  连续 LBA 数
 * @return true  区间完整落在 [0, FTL_LBA_MAX)
 */
bool ssd_controller_lba_range_ok(QWORD lba, DWORD count);

/*
 * ssd_controller_state_transition - Controller 最小状态机（C1，state_machine.c）
 *
 * 合法转换表：RESET→INITIALIZING（boot）、INITIALIZING→READY/FAILED
 * （槽位校验结果）、READY→FAILED（运行时致命错误，预留）。
 * 非法转换直接忽略（保持现态）；FAILED 为终态。
 *
 * @param controller Controller 上下文
 * @param next_state 目标状态
 */
void ssd_controller_state_transition(ssd_controller_t      *controller,
                                     ssd_controller_state_t next_state);

/*
 * ssd_controller_schedule - Controller 优先级调度钩子（C1b，scheduler.c）
 *
 * host IO 到来（ssd_controller_submit 每次提交）时调用本函数，
 * 返回 0 = 后台让路（host IO 优先）；空闲时返回 1 = 全力后台。
 * 实际后台预算由 ftl_service 的 B3 空闲度判定承担，本钩子只做观测点。
 *
 * @param controller     Controller 上下文
 * @param host_io_active 1=有 host IO 提交，0=空闲
 * @return 后台预算指示：0=最小后台，1=全力后台
 */
int ssd_controller_schedule(ssd_controller_t *controller, int host_io_active);

#endif /* SSD_CONTROLLER_H */
