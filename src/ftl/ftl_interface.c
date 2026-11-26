/*
 * ftl_interface.c — FTL 公共 API（include/ftl/ftl_interface.h）的实现层
 *
 * 职责：
 *   · 对外薄转发 / 类型适配 / 跨实例编排（boot 三件套、路由、请求中止、admission）；
 *   · 介质布局版本计算（拓扑指纹）。
 *
 * 关键机制：
 *   · 公共头声明的函数一律 `ftl_` 前缀；真实逻辑留在各模块 .c，本文件不引入第二套坐标；
 *   · 跨实例编排（abort_request 遍历全部 die）只在这里发生；
 *   · 薄转发只做参数 / 类型适配，不复制模块内策略。
 *
 * 与其他模块：
 *   · 上游：main.c（boot）、HIL controller（路由 / abort / admission）；
 *   · 下游：ftl_op（路由）、pfr（boot）、pending（组中止）、ftl_sched（admission）。
 */

#include "ftl_interface.h"

#include "allocator.h" /* alc_s / alc_pool_counts_sum */
#include "assertion.h"
#include "crc.h"       /* crc_compute_iso_hdlc（介质布局指纹） */
#include "ftl.h"       /* ftl_ctx_s 完整定义 */
#include "ftl_op.h"    /* ftl_op_route_* */
#include "ftl_sched.h" /* ftl_sched_host_write_is_admitted */
#include "log.h"
#include "pending.h"                /* pend_group_abort_ctx */
#include "power_failure_recovery.h" /* pfr_* */

/* ── host ── */

/**
 * @brief host 写启动许可
 *
 * @param ftl FTL 实例（非 NULL）
 * @return true 允许；false 暂缓
 */
BOOL ftl_host_write_is_admitted(const ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);
    return ftl_sched_host_write_is_admitted(ftl);
}

/* ── media ── */

/**
 * @brief 计算当前介质布局版本
 *
 * 单 die = BASE；多 die 把 die 数与 topology map 编入版本，拓扑变化 ⇒ 版本不符
 * ⇒ 格式化。用 crc_compute_iso_hdlc 哈希字段字节（不哈希 padding）。
 *
 * @return 介质布局版本
 */
DWORD ftl_media_layout_version(void)
{
    if (g_die_count <= 1U)
    {
        return FTL_MEDIA_LAYOUT_VERSION_BASE;
    }

    /* [count][ch0][ce0][lun0][ch1]... —— 只含已分配 die。 */
    BYTE   topo[1U + (FTL_DIE_MAX * 3U)];
    size_t used = 1U;

    topo[0] = g_die_count;

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        topo[used++] = g_die_table[die].ch;
        topo[used++] = g_die_table[die].ce;
        topo[used++] = g_die_table[die].lun;
    }

    const DWORD fingerprint = crc_compute_iso_hdlc(topo, used);

    /* bit31 固定为 1：多 die 版本不可能与单 die 的 BASE 小版本号相撞。
     * 有效哈希 31 位，碰撞概率 2^-31。 */
    return 0x80000000UL | (fingerprint & 0x7FFFFFFFUL);
}

/* ── route ── */

/**
 * @brief 校验实例路由与紧凑 die 表的往返一致性
 *
 * @return true = 一致；false = 路由/拓扑公式漂移
 */
BOOL ftl_route_verify(void)
{
    return ftl_op_route_verify();
}

/**
 * @brief 撤销实例路由（boot 失败回退 fc_service 时用）
 */
void ftl_route_clear(void)
{
    ftl_op_route_set(NULL);
}

/* ── abort ── */

/**
 * @brief 中止属于同一 host 请求的所有 FTL 组
 *
 * @param ctx HIL 请求指针（非空）
 */
void ftl_abort_request(void *ctx)
{
    ASSERT(ctx != NULL);

    for (BYTE die = 0U; die < DIE_MAX; die++)
    {
        ftl_ctx_s *ftl = ftl_instance_get(die);

        pend_group_abort_ctx(&ftl->pending, ctx);
    }
}

/* ── boot ── */

/**
 * @brief 判定介质是否需要格式化。
 * @param ftl FTL 上下文
 * @param need_format 输出：true = 需要格式化
 * @return FTL_OK 判定完成；FTL_ERROR CP 介质读不通（调用者不得格式化）
 */
ftl_status_e ftl_boot_check_format(ftl_ctx_s *ftl, BOOL *need_format)
{
    return pfr_media_check_format(ftl, need_format);
}

/**
 * @brief 全量格式化当前实例的介质。
 * @param ftl FTL 上下文
 * @param verify true = 逐块擦除体检
 * @return FTL_OK 成功；FTL_ERROR 失败
 */
ftl_status_e ftl_boot_format(ftl_ctx_s *ftl, BOOL verify)
{
    return pfr_media_format(ftl, verify);
}

/**
 * @brief 执行启动恢复并输出报告。
 * @param ftl FTL 上下文
 * @param report 启动类型/Checkpoint/Journal 统计；可为 NULL
 * @return FTL_OK 成功；FTL_ERROR 恢复失败
 */
ftl_status_e ftl_boot_recover(ftl_ctx_s *ftl, ftl_boot_report_s *report)
{
    pfr_boot_result_s result;
    ftl_status_e      status = pfr_boot_recovery(ftl, &result);

    if ((status == FTL_OK) && (report != NULL))
    {
        report->boot_type       = (ftl_boot_type_e)result.boot_type;
        report->cp_sequence     = result.cp_sequence;
        report->journal_entries = result.journal_entries;
        report->blocks_restored = result.blocks_restored;
        report->scoped          = result.scoped;
    }
    return status;
}

/**
 * @brief 打印本实例的池占用（boot 基线）
 *
 * @param ftl 已初始化的实例（只读）
 */
void ftl_boot_print_usage(const ftl_ctx_s *ftl)
{
    ASSERT(ftl != NULL);

    const alc_s *alloc = &ftl->alloc;

    LOGI("[FTL ] instance ch=%hhu ce=%hhu lun=%hhu free=%hu op=%hu spare=%hu\r\n", ftl->topo.ch, ftl->topo.ce, ftl->topo.lun, alloc->free_count, alc_pool_counts_sum(alloc->op_count, PLANE_NUM), alc_pool_counts_sum(alloc->spare_count, PLANE_NUM));
}

/**
 * @brief 打印固定分区与数据区的坏块分布（开机基线）。
 * @param ftl FTL 上下文（只读）
 */
void ftl_boot_print_bad_areas(const ftl_ctx_s *ftl)
{
    pfr_boot_print_bad_areas(ftl);
}
