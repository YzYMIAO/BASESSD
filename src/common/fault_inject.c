/*
 * fault_inject.c — 测试镜像专属故障注入点（ISS-FTL-16）
 *
 * 接口契约与注入点语义见 include/common/fault_inject.h。
 * 开关 = `FC_NAND_TEST_ENABLE`（只有 `make tests` 定义）：产品固件下本 TU 不产生代码，
 * 头文件把同名 API 退化为 static inline 空实现。用测试开关而不是 `NDEBUG` 的原因：
 * release 档同样要能跑 UT（-O2 下看最终速度），注入不能随 NDEBUG 一起消失。
 */

#include "fault_inject.h"

#if defined(FC_NAND_TEST_ENABLE)

/* 「未武装」哨兵。LBA 值域远小于 UINT32_MAX（FTL_LBA_MAX ≈ 89 万），不会撞上真实地址。 */
#    define FI_ARM_NONE (UINT32_MAX)

/* 初值必须是「未武装」：静态零初始化会让 LBA 0 的页在没人 arm 时命中。 */
static DWORD s_cache_fail_lba    = FI_ARM_NONE;
static DWORD s_journal_fail_left = 0U;

void fi_reset(void)
{
    s_cache_fail_lba    = FI_ARM_NONE;
    s_journal_fail_left = 0U;
}

void fi_cache_fail_arm(DWORD page_lba)
{
    s_cache_fail_lba = page_lba;
}

BOOL fi_cache_fail_take(DWORD page_lba)
{
    if (page_lba != s_cache_fail_lba)
    {
        return false;
    }

    s_cache_fail_lba = FI_ARM_NONE; /* 一次性：失败后不再命中，恢复路径才看得见 */
    return true;
}

void fi_journal_fail_arm(DWORD pages)
{
    s_journal_fail_left = pages;
}

BOOL fi_journal_fail_take(void)
{
    if (s_journal_fail_left == 0U)
    {
        return false;
    }

    s_journal_fail_left--;
    return true;
}

#else  /* 产品固件：没有任何注入点；本 TU 允许为空（无 -pedantic 要求）。 */
#endif /* FC_NAND_TEST_ENABLE */
