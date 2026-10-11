#ifndef PLATFORM_STM32H743_SYS_PORT_H
#define PLATFORM_STM32H743_SYS_PORT_H

/*
 * sys_port.h — stm32h743 系统服务 port 伞头。
 *
 * 由原 include/platform/stm32h743/sys.h 拆出：这里只留厂商设备头与 SYS_SUPPORT_OS，
 * 以及抽象 sys.h 的转发（sys_* / sys_port_* 声明都在 include/platform/sys.h）。
 * 需要 HAL 类型的 h743 侧文件改为裸名 include 本头。
 */

#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"
#include "sys.h"

#define SYS_SUPPORT_OS 0U

#endif /* PLATFORM_STM32H743_SYS_PORT_H */
