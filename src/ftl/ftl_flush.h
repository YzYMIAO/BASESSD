#ifndef FTL_FLUSH_H
#define FTL_FLUSH_H

#include "checkpoint.h"
#include "ftl_config.h"

/* 异步 FLUSH job 的耗时告警阈值：从 `ftl_flush_begin` 到完成超过该值即打一行 WRN。
 * 用于观察主机看到的 flush 延迟；job 本身不阻塞主循环。 */
#define FTL_FLUSH_SLOW_LOG_MS (10U)

/* 异步 FLUSH job 的**无进展**上限：wbuf / L2P / journal / 写确认仍在推进时按进展续期；
 * 连续 1 s 无任何推进才判超时。超时按 best-effort 完成并打 WRN；若有未确认写页，
 * 同时置 deferred_error（不能对 host 宣称成功，H14）。 */
#define FTL_FLUSH_TIMEOUT_MS (1000U)

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
 * @note 后台预算器（ftl_sched）与显式 CP（ftl_flush_kick_cp）共用——同一组字段
 *       只在单处装配，`cp_trigger_s` 加字段时不会漏改某一处。
 */
cp_trigger_s ftl_cp_trigger_build(const ftl_ctx_s *ftl, cp_reason_e reason, BOOL force);

/** @brief 复位 flush / trim 模块的文件级状态（NOLOAD 段不清零）。 */
void ftl_flush_trim_init(void);

#endif /* FTL_FLUSH_H */
