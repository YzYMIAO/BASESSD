#include "fc_crc_engine.h"

#include "assertion.h"
#if defined(FC_NAND_TEST_ENABLE)
#    include "fault_inject.h"
#endif
#include "crc.h"

#include <stddef.h>

/* ══════════════════════════════════════════════════════════
 * CRC 引擎
 * ══════════════════════════════════════════════════════════ */

/**
 * @brief 初始化硬件 CRC 外设
 *
 * 时钟使能、HAL 配置与外设应答自检都在平台层 crc_init() 内；自检不过
 * 一律 ASSERT，没有「失败静默继续」的降级路径。
 */
void fc_crc_init(void)
{
    crc_init();
}

/**
 * @brief 计算 CW 的 512B USER 数据 CRC-32
 *
 * 平台层每次先 RESET 引擎回 INIT 值，保证独立计算；user_data 须 4 字节对齐
 * （HAL 的 WORDS 分支直接按 DWORD* 解引用）。
 *
 * @param user_data USER 区首地址
 * @return CRC-32
 */
DWORD fc_crc_compute(const BYTE *user_data)
{
    // user_data 前置条件由调用者保证
    ASSERT(user_data != NULL);

#if defined(FC_NAND_TEST_ENABLE)
    if (fi_take(FI_CRC_COMPUTE_FAIL) == true)
    {
        return 0U;
    }
#endif

    return crc_compute(user_data, FC_CRC_USER_SIZE);
}

/**
 * @brief 校验 USER 数据 CRC 是否等于存储值
 *
 * @param user_data  USER 区首地址
 * @param stored_crc 读回的 CRC 字段
 * @return FC_CRC_RESULT_OK / FC_CRC_RESULT_ERROR
 */
BYTE fc_crc_verify(const BYTE *user_data, DWORD stored_crc)
{
    // user_data 前置条件由调用者保证
    ASSERT(user_data != NULL);

    return (fc_crc_compute(user_data) == stored_crc) ? FC_CRC_RESULT_OK : FC_CRC_RESULT_ERROR;
}
