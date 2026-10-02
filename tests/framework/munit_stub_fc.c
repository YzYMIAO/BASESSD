/* munit host 测试的 FC 桩：真机/fc 构建不编译本文件（tests/Makefile 仅 f4 target 用）。
 *
 * 提供 FC 层符号，让 l2p_cache/checkpoint/metadata 的同步元数据读写路径
 * 在 host 上"虚拟成功"——不触碰真实 NAND，仅走通簿记逻辑（flush 成功 →
 * line CLEAN → region bit 清零）。
 *
 * 关键约定：
 *   fc_command_alloc       → 返回静态有效 cmd（非 NULL）
 *   fc_submit              → FC_STATUS_OK
 *   ftl_internal_sync_wait → 返回 0，completion->status = 0（无 NAND_FAIL）
 *   因此 sync_write/read_page 返回 FTL_OK，与真实 NAND 成功行为一致。
 */
#include "fc/fc.h"
#include "ftl/ftl_fc_interface.h"
#include "ftl/journal.h"
#include "nand/nand_config.h"
#include "platform/platform.h"

#include <string.h>

/* 静态 cmd 池：分配/释放都是它本身（host 无并发，单实例足够） */
static fc_command_t g_stub_cmd;

/* 失败注入开关：置 1 后下一次 fc_submit 返回 IO_ERROR（模拟 NAND 写失败） */
int fc_stub_fail_next = 0;

/* ── 内存 NAND：F10 需要 journal 页真实读回（回放读 NAND）。
 * 布局按 journal_page_addr 的地址映射：block(0/1) × page(0..63) × plane(0/1)
 * 交错，但 F10 测试只写整页连续 data（JOURNAL_PAGE_SIZE ≤ PAGE_SIZE），
 * 故每 (block,page) 只存一份。── */
#define STUB_NAND_BLOCKS (JOURNAL_BLOCK_COUNT)
#define STUB_NAND_PAGES  (PAGES_PER_BLOCK)
#define STUB_NAND_PLANES (PLANE_NUM)
#define STUB_NAND_SIZE   (STUB_NAND_BLOCKS * STUB_NAND_PAGES * STUB_NAND_PLANES * PAGE_SIZE)

static uint8_t g_stub_nand[STUB_NAND_SIZE];
static uint8_t g_stub_nand_init; /* 0xFF 惰性初始化标记 */

static uint8_t *stub_nand_page_addr(uint8_t block, uint8_t page, uint8_t plane)
{
    uint32_t idx = ((uint32_t)block * STUB_NAND_PAGES + page) *
                       STUB_NAND_PLANES * PAGE_SIZE +
                   (uint32_t)plane * PAGE_SIZE;
    return &g_stub_nand[idx];
}

/* journal 命令的 cmd->paa 是 paa_make(page,plane,block,lun,cw) 打包的 PAA：
 * 拆开得物理页。block 映射回 0/1（JOURNAL_BLOCK0/1）。 */
static void stub_nand_from_paa(DWORD paa, uint8_t *block, uint8_t *page,
                               uint8_t *plane)
{
    uint32_t blk = paa_to_block(paa);
    *block       = (blk == (uint32_t)JOURNAL_BLOCK0) ? 0U : 1U;
    *page        = (uint8_t)paa_to_page(paa);
    *plane       = (uint8_t)paa_to_plane(paa);
}

void munit_stub_nand_init(void)
{
    memset(g_stub_nand, 0xFF, sizeof(g_stub_nand));
    g_stub_nand_init = 1U;
}

void munit_stub_nand_erase_block(uint8_t block)
{
    uint8_t p, pl;
    for (p = 0U; p < STUB_NAND_PAGES; p++)
    {
        for (pl = 0U; pl < STUB_NAND_PLANES; pl++)
        {
            memset(stub_nand_page_addr(block, p, pl), 0xFF, PAGE_SIZE);
        }
    }
}

fc_command_t *fc_command_alloc(void)
{
    memset(&g_stub_cmd, 0, sizeof(g_stub_cmd));
    return &g_stub_cmd;
}

fc_status_t fc_command_release(fc_command_t *cmd)
{
    (void)cmd;
    return FC_STATUS_OK;
}

fc_status_t fc_submit(fc_command_t *cmd)
{
    (void)cmd;
    if (fc_stub_fail_next != 0)
    {
        fc_stub_fail_next = 0;
        return FC_STATUS_IO_ERROR;
    }
    return FC_STATUS_OK;
}

ftl_status_t ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length)
{
    uint8_t  block, page, plane;
    uint8_t *dst;

    if ((cmd == NULL) || (data == NULL))
    {
        return FTL_ERROR;
    }

    if (!g_stub_nand_init)
    {
        munit_stub_nand_init();
    }

    stub_nand_from_paa(cmd->paa, &block, &page, &plane);
    dst = stub_nand_page_addr(block, page, plane);
    if (length > PAGE_SIZE)
    {
        length = PAGE_SIZE;
    }
    memcpy(dst, data, length);
    return FTL_OK;
}

ftl_status_t ftl_cmd_read_data(fc_command_t *cmd, void *data, uint32_t length)
{
    uint8_t  block, page, plane;
    uint8_t *src;

    if ((cmd == NULL) || (data == NULL))
    {
        return FTL_ERROR;
    }

    if (!g_stub_nand_init)
    {
        munit_stub_nand_init();
    }

    stub_nand_from_paa(cmd->paa, &block, &page, &plane);
    src = stub_nand_page_addr(block, page, plane);
    if (length > PAGE_SIZE)
    {
        length = PAGE_SIZE;
    }
    memcpy(data, src, length);
    return FTL_OK;
}

int ftl_internal_sync_wait(uint16_t command_id, fc_command_status_t *completion,
                           uint32_t timeout_ms)
{
    (void)command_id; (void)timeout_ms;
    if (completion != NULL)
    {
        memset(completion, 0, sizeof(*completion)); /* status=0 → 无 NAND_FAIL */
    }
    return 0;
}

fc_status_t fc_get_resource_stats(fc_resource_stats_t *stats)
{
    (void)stats;
    return FC_STATUS_NOT_SUPPORTED; /* host 环境无 FC 资源：munit 不打印 RES 行 */
}

/* fc_poll_completion 桩：恒 QUEUE_EMPTY → completion_table_drain 的
 * while 循环立即终止（返回 OK=0 会死循环）。F4/F6 测试不真正 drain。 */
fc_status_t fc_poll_completion(fc_command_status_t *completion)
{
    (void)completion;
    return FC_STATUS_QUEUE_EMPTY;
}

/* fc_command_id_to_command 桩：F6 的 with_release handler 引用；返回 NULL
 * 即可（release 分支跳过，无泄漏风险）。 */
fc_command_t *fc_command_id_to_command(uint16_t command_id)
{
    (void)command_id;
    return NULL;
}

/* 平台 time_us：checkpoint_now_ms 引用（host 桩返回 0，timestamp 退化 0） */
const ssd_platform_ops_t *ssd_platform_get(void)
{
    return NULL;
}

/* journal_truncate 桩：F4/F6 测试不含 journal.c（checkpoint_create 引用）；
 * 用弱符号保证 F10 target（链真实 journal.c 强符号）自动覆盖。
 * （GCC：__attribute__((weak)) 在 -Werror 下不触发告警。） */
void __attribute__((weak)) journal_truncate(l2p_journal_t *journal,
                                            uint32_t up_to_sequence)
{
    (void)journal; (void)up_to_sequence;
}

/* 本桩仅供 host 独立构建（tests/Makefile f4/f6/f10 target）；上板固件不编译本文件，
 * 同名符号由 src/fc/fc.c（真实 FC 层）+ tests/unit/f4_stub_fail.c 提供。 */
