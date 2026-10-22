/*
 * state_machine.c — Controller 最小状态机（C1）
 *
 * 管理 RESET → INITIALIZING → READY / FAILED 转换。
 * ssd_controller_init 的槽位校验结果驱动转换：全部通过 → READY，
 * 任一槽位缺失/拓扑不符 → FAILED（终态）。
 *
 * SHUTDOWN 不在枚举内（学习主控无热移除/优雅掉电退出路径），注释保留；
 * 若后续加枚举值，只扩展本文件的转换表，不动调用方。
 */
#include "controller.h"
#include "log.h"

void ssd_controller_state_transition(ssd_controller_t *controller,
                                     ssd_ctrl_state_e  next_state)
{
    ssd_ctrl_state_e prev_state;

    if (controller == NULL)
    {
        return;
    }

    prev_state = controller->state;

    switch (controller->state)
    {
        case SSD_CONTROLLER_RESET:
            /* boot 起点：RESET → INITIALIZING（ssd_controller_init 入口） */
            if (next_state == SSD_CONTROLLER_INITIALIZING)
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_INITIALIZING:
            /* 槽位校验结束：全部通过 → READY；任一失败 → FAILED */
            if ((next_state == SSD_CONTROLLER_READY) ||
                (next_state == SSD_CONTROLLER_FAILED))
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_READY:
            /* 运行时致命错误 → FAILED（当前无调用点，保留语义） */
            if (next_state == SSD_CONTROLLER_FAILED)
            {
                controller->state = next_state;
            }
            break;

        case SSD_CONTROLLER_FAILED:
        default:
            /* FAILED 为终态，不响应任何转换 */
            break;
    }

    /* FAILED 是终态，进入即宣告 controller 不可用；在此单点留痕 */
    LOGE_IF((prev_state != SSD_CONTROLLER_FAILED) && (controller->state == SSD_CONTROLLER_FAILED), LOG_TAG_CTRL "state -> FAILED\r\n");
}
