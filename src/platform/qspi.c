/*
 * qspi.c — 跨 target 共享的 QSPI 服务层
 *
 * 只放与硬件无关的部分：接收/发送的空参检查（buf 非空、长度非 0）；QSPI 句柄、
 * 引脚/时钟、命令模式解码、HAL 收发全在 src/platform/<soc>/qspi_port.c。
 */

#include "qspi.h"

#include "common/common.h"

uint8_t qspi_wait_flag(uint32_t flag, uint8_t sta, uint32_t wtime)
{
    return qspi_port_wait_flag(flag, sta, wtime);
}

uint8_t qspi_init(void)
{
    return qspi_port_init();
}

uint8_t qspi_send_cmd(uint8_t cmd, uint32_t addr, uint8_t mode, uint8_t dmcycle)
{
    return qspi_port_send_cmd(cmd, addr, mode, dmcycle);
}

uint8_t qspi_receive(uint8_t *buf, uint32_t datalen)
{
    if ((buf == NULL) || (datalen == 0U))
    {
        return FUNC_STATUS_ERROR;
    }
    return qspi_port_receive(buf, datalen);
}

uint8_t qspi_transmit(uint8_t *buf, uint32_t datalen)
{
    if ((buf == NULL) || (datalen == 0U))
    {
        return FUNC_STATUS_ERROR;
    }
    return qspi_port_transmit(buf, datalen);
}
