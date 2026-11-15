/*
 * fault_inject.c — 测试镜像专属故障注入（分层负例框架）
 *
 * 接口契约与注入点语义见 include/common/fault_inject.h。
 * 开关 = `FC_NAND_TEST_ENABLE`（只有 `make tests` 定义）：产品固件下本 TU 不产生代码。
 *
 * 每个故障一个槽：剩余次数、匹配 key、故障参数。`fi_take*` 命中即自减；
 * 未武装（left==0）恒不命中；`FI_KEY_ANY` 表示不限定 key。
 * 调用者只有测试侧 wrap 库与既有静态缝 hook，生产构建不引用本 TU。
 */

#include "fault_inject.h"

#if defined(FC_NAND_TEST_ENABLE)

#    include "assertion.h"

typedef struct FI_SLOT_S
{
    DWORD left;    /* 剩余命中次数（0 = 未武装） */
    DWORD key;     /* 匹配 key（FI_KEY_ANY = 任意） */
    DWORD key_alt; /* 第二匹配 key（FI_KEY_NONE = 不启用） */
    DWORD arg;     /* 故障参数（类型相关） */
    DWORD hits;    /* 自 fi_reset() 以来的累计命中（证据用） */
} fi_slot_s;

static fi_slot_s s_fi[FI_FAULT_NUM];

void fi_reset(void)
{
    for (BYTE i = 0U; i < (BYTE)FI_FAULT_NUM; i++)
    {
        s_fi[i].left    = 0U;
        s_fi[i].key     = FI_KEY_ANY;
        s_fi[i].key_alt = FI_KEY_NONE;
        s_fi[i].arg     = 0U;
        s_fi[i].hits    = 0U;
    }
}

void fi_arm(fi_fault_e f, DWORD count)
{
    fi_arm_ex(f, FI_KEY_ANY, count, 0U);
}

void fi_arm_at(fi_fault_e f, DWORD key, DWORD count)
{
    fi_arm_ex(f, key, count, 0U);
}

void fi_arm_ex(fi_fault_e f, DWORD key, DWORD count, DWORD arg)
{
    fi_arm_ex2(f, key, FI_KEY_NONE, count, arg);
}

void fi_arm_ex2(fi_fault_e f, DWORD key, DWORD key_alt, DWORD count, DWORD arg)
{
    // 枚举由调用者自己持有（测试代码的编译期常量）⇒ 越界属编程错误（C5 / H34）。
    ASSERT((DWORD)f < (DWORD)FI_FAULT_NUM);

    if ((DWORD)f >= (DWORD)FI_FAULT_NUM)
    {
        return; // release UT：测试侧内部 API，越界直接忽略（不静默改介质）
    }

    s_fi[f].left    = count;
    s_fi[f].key     = key;
    s_fi[f].key_alt = key_alt;
    s_fi[f].arg     = arg;
}

static BOOL fi_take_key(fi_fault_e f, DWORD key, DWORD *out_arg)
{
    if ((DWORD)f >= (DWORD)FI_FAULT_NUM)
    {
        return false;
    }

    fi_slot_s *slot = &s_fi[f];

    if (slot->left == 0U)
    {
        return false;
    }
    if ((slot->key != FI_KEY_ANY) && (slot->key != key) && (slot->key_alt != key))
    {
        return false;
    }

    slot->left--;
    slot->hits++;

    if (out_arg != NULL)
    {
        *out_arg = slot->arg;
    }

    return true;
}

BOOL fi_take(fi_fault_e f)
{
    return fi_take_key(f, FI_KEY_ANY, NULL);
}

BOOL fi_take_at(fi_fault_e f, DWORD key)
{
    return fi_take_key(f, key, NULL);
}

BOOL fi_take_ex(fi_fault_e f, DWORD key, DWORD *out_arg)
{
    return fi_take_key(f, key, out_arg);
}

DWORD fi_hits(fi_fault_e f)
{
    if ((DWORD)f >= (DWORD)FI_FAULT_NUM)
    {
        return 0U;
    }

    return s_fi[f].hits;
}

/* ── 兼容旧 API：cache program 完成 / Journal 页写 ── */

void fi_cache_fail_arm(DWORD page_lba)
{
    fi_arm_at(FI_CACHE_FAIL, FI_KEY_LBA(page_lba), 1U);
}

BOOL fi_cache_fail_take(DWORD page_lba)
{
    return fi_take_at(FI_CACHE_FAIL, FI_KEY_LBA(page_lba));
}

void fi_journal_fail_arm(DWORD pages)
{
    fi_arm(FI_JOURNAL_WRITE_FAIL, pages);
}

BOOL fi_journal_fail_take(void)
{
    return fi_take(FI_JOURNAL_WRITE_FAIL);
}

#else  /* 产品固件：没有任何注入点；本 TU 允许为空。 */
#endif /* FC_NAND_TEST_ENABLE */
