#ifndef PLATFORM_STM32H743_SYS_PORT_H
#define PLATFORM_STM32H743_SYS_PORT_H

/*
 * sys_port.h — stm32h743 系统服务 port 伞头。
 *
 * 只提供厂商设备头与 SYS_SUPPORT_OS；公共 sys / sys_port 声明在抽象 sys.h。
 * 需要 HAL 类型的 h743 侧文件以裸名 include 本头。
 */

#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"
#include "sys.h"

#define SYS_SUPPORT_OS 0U

#endif /* PLATFORM_STM32H743_SYS_PORT_H */
