/*
 * qspi.c — 跨 target 共享的 QSPI 服务层
 *
 * 只放与硬件无关的部分：接收/发送的空参检查（buf 非空、长度非 0）；QSPI 句柄、
 * 引脚/时钟、命令模式解码、HAL 收发全在 src/platform/<soc>/qspi_port.c。
 */

#include "qspi.h"

#include "common.h"

func_status_e qspi_wait_flag(DWORD flag, BYTE sta, DWORD wtime)
{
    return qspi_port_wait_flag(flag, sta, wtime);
}

func_status_e qspi_init(void)
{
    return qspi_port_init();
}

func_status_e qspi_send_cmd(BYTE cmd, DWORD addr, BYTE mode, BYTE dmcycle)
{
    return qspi_port_send_cmd(cmd, addr, mode, dmcycle);
}

func_status_e qspi_receive(BYTE *buf, DWORD datalen)
{
    if ((buf == NULL) || (datalen == 0U))
    {
        return FUNC_STATUS_ERROR;
    }
    return qspi_port_receive(buf, datalen);
}

func_status_e qspi_transmit(BYTE *buf, DWORD datalen)
{
    if ((buf == NULL) || (datalen == 0U))
    {
        return FUNC_STATUS_ERROR;
    }
    return qspi_port_transmit(buf, datalen);
}
