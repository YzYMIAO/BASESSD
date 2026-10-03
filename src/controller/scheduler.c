/*
 * scheduler.c — Controller 优先级调度钩子（C1b）
 *
 * 学习主控最小实现：host IO（UART CLI）优先于后台 GC/元数据。
 * ssd_controller_submit 每提交一条 host 请求调用本函数（host_io_active=1），
 * 返回 0 = 后台让路；空闲时（host_io_active=0）返回 1 = 全力后台。
 *
 * 真正的后台预算分配由 ftl_service 的 B3 空闲度判定承担（pending +
 * completion_table 占用 → IDLE/NORMAL/BUSY，ftl.c:546-558），本钩子不重复
 * 实现队列，只做"host IO 到来"的观测点，返回值预留后续挂时间窗/预算缩减。
 */
#include "controller/controller.h"

int ssd_controller_schedule(ssd_controller_t *controller, int host_io_active)
{
    (void)controller; /* 当前无状态字段；时间窗扩展时在此记录 last_host_io_ms */

    return (host_io_active != 0) ? 0 : 1;
}
