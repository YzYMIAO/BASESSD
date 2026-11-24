#ifndef FTL_FLUSH_H
#define FTL_FLUSH_H

#include "checkpoint.h"
#include "ftl_internal.h"

typedef struct FTL_FLUSH_JOB_S
{
    BOOL  active;
    DWORD start_ms;         /* 总耗时观测（FTL_FLUSH_SLOW_LOG_MS） */
    DWORD last_progress_ms; /* 最近一次观察到元数据推进的时刻（按进展续期） */
    DWORD progress;         /* 上一次的推进签名；变化即刷新 last_progress_ms */
} ftl_flush_job_s;

typedef struct FTL_TRIM_JOB_S
{
    BOOL             active;
    DWORD            lba;
    DWORD            end;
    DWORD            start_lba; /* 完成时 trim_lba 已到 end，不能反推 */
    timer_ms_t       start_ms;  /* 观测 elapsed */
    timer_deadline_s deadline;  /* 无进展等待 deadline */
} ftl_trim_job_s;

/**
 * @brief 按当前实例状态构造一次 Checkpoint 触发快照。
 *
 * @param ftl    实例（读 total_writes 与 journal 的 seq / 水位 / 紧急标志）
 * @param reason 触发原因（原样写入 trigger.reason）
 * @param force  强制请求标志（显式 CP 传 true；后台适配器传 false）
 *
 * @return 触发快照（now_ms 取调用时刻）
 *
 * @note 后台适配器（bg_policy）与显式 CP（ftl_flush_kick_cp）共用——同一组字段
 *       只在单处装配，`cp_trigger_s` 加字段时不会漏改某一处。
 */
cp_trigger_s ftl_cp_trigger_build(const ftl_ctx_s *ftl, cp_reason_e reason, BOOL force);

/** @brief 复位 flush / trim 模块的文件级状态（NOLOAD 段不清零）。 */
void ftl_flush_trim_init(void);

#endif /* FTL_FLUSH_H */
