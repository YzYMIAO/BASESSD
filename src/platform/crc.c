/*
 * crc.c — 跨 target 共享的 CRC-32 服务层
 *
 * 只放与硬件无关的部分：外设应答自检策略（IDR 写读回环）与 crc_compute 的
 * 对齐/长度契约；寄存器动作（HAL 配置、IDR 读写、Calculate）全在
 * src/platform/<soc>/crc_port.c。上层 src/fc/fc_crc_engine.c 只调本文件的
 * crc_init() / crc_compute()。
 *
 * 配置：IEEE 802.3 CRC-32 —— 默认多项式 0x04C11DB7、INIT 0xFFFFFFFF、
 *       输入/输出均不反相、按 32 位字喂入。
 */

#include "crc.h"

#include "common/assertion.h"

/*
 * crc_init
 *   初始化 CRC 外设，随后做一次 IDR 写读回环自检。
 *   自检不过一律 ASSERT —— 外设不应答属于配置错误，不允许静默降级。
 */
void crc_init(void)
{
    crc_port_init();

    /*
     * IDR 写读回环：唯一能证明外设真的应答的检查 —— 时钟没开时写被静默丢弃、
     * 读回恒 0，而 HAL 的 CRC 初始化自身不校验。此前 FC_CRC_BASE 写成了 ADC3 的地址，
     * 正是被这一环抓到、又被「失败静默继续」咽掉的。
     */
    crc_port_set_idr(0xA5U);
    ASSERT(crc_port_get_idr() == 0xA5U);
}

/*
 * crc_compute
 *   对 data 起 length_bytes 字节算 CRC-32，每次从 INIT 值重新起算。
 *   前置：data 非空且 4 字节对齐、length_bytes 为 4 的倍数。
 */
uint32_t crc_compute(const uint8_t *data, uint32_t length_bytes)
{
    ASSERT(data != NULL);
    ASSERT((length_bytes % 4U) == 0U); /* WORDS 格式：长度必须是 4 的倍数 */

    return crc_port_compute(data, length_bytes);
}
