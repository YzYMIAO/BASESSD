#ifndef FTL_MODULE_H
#define FTL_MODULE_H

/*
 * ftl_module.h — FTL 内部模块的统一 handle 契约（公共部分）
 *
 * 设计见 docs/design/ftl_module_contracts.md §3。
 *
 * 分工：
 *   · 共享数据面账本（L2P / BST / allocator / journal 水位 / metadata / pending /
 *     completion）保持共享 device context，模块**不**复制账本；
 *   · 模块只拥有**执行状态**（FSM / 单飞行 / 计时器 / 进度计数），对外只暴露
 *     typed `*_handle(handle, trigger/info, status)` 与纯查询；
 *   · 主流程不窥探模块内部 phase / active / waiting / stage。
 *
 * 本头只放**跨模块共用**的类型；各模块的 `*_status_t` 用嵌入 base 的方式扩展自己的
 * 上报字段（见 bbt.h / journal.h / ...）。
 */

#include "ftl_type.h" /* ftl_status_t（FTL_TIMEOUT） */

#include <stdbool.h>
#include <stdint.h>

/* ── 模块状态 ──
 *
 * 只描述「本轮调用之后模块处于什么状态」，不是模块内部的 phase 编号：
 *   IDLE     无事可做（wants_run = false）；
 *   RUNNING  有工作在推进（有命令在飞或纯计算续跑）；
 *   WAITING  等外部资源/完成（提交背压、等 L2P/Journal）；
 *   BLOCKED  需要上层动作（水位满 / 无 victim / 配置缺口）；
 *   FAILED   本次执行遇到不可自愈错误（error 给出码）。
 */
typedef enum
{
    FTL_MODULE_IDLE = 0,
    FTL_MODULE_RUNNING,
    FTL_MODULE_WAITING,
    FTL_MODULE_BLOCKED,
    FTL_MODULE_FAILED,
} ftl_module_state_t;

/*
 * ftl_module_status_t — 所有模块 status 的公共前缀
 *
 * 字段语义：
 *   state     本轮结束后的模块状态（见上）；
 *   critical  must-run liveness（正确性关键集；调度器据此决定是否受预算/档位门控）；
 *   wants_run 下一轮是否还需要被调（false = 可以跳过）；
 *   progress  模块自定的单调计数（只增不减，用于区分「在推进」与「卡死」）；
 *   next_ms   0 = 随时可调；否则为最早下次调用时间（ms 绝对时基）；
 *   error     FTL_* / FTL_TIMEOUT；仅在 state == FAILED 时有意义。
 */
typedef struct
{
    ftl_module_state_t state;
    bool               critical;
    bool               wants_run;
    DWORD              progress;
    DWORD              next_ms;
    int32_t            error;
} ftl_module_status_t;

/*
 * ftl_bg_info_t — 调度器每轮交给模块的只读动态信息
 *
 * 只传**每轮会变**的量；编译期常量由模块自己 include，只读策略表放模块内
 * `static const`，每实例不可变配置在 `*_init` 时绑定进 handle（H10）。
 *
 * free_blocks / pending 等由需要它的模块通过 allocator/pending 端口查询，或放进
 * 该模块自己的 trigger —— 不塞进本结构（否则每个模块都被迫接收无关量）。
 */
typedef struct
{
    DWORD now_ms;     /* 单调毫秒（平台时钟缺失时为 0） */
    BYTE  load_level; /* BG_LEVEL_IDLE / NORMAL / BUSY（bg_scheduler.h） */
} ftl_bg_info_t;

/* ── ms deadline（本仓取值，须上板标定；docs §6.3）──
 * 统一绝对 deadline，不用 rounds（rounds 随主循环速率漂移）。
 * 模块 deadline 必须 > NAND 超时 + 余量；NAND 已返回 TIMEOUT 时不再叠加。 */
#define FTL_WAIT_TIMEOUT_MS        (50U)  /* 等 L2P/Journal 等外部模块 */
#define FTL_SUBMIT_TIMEOUT_MS      (10U)  /* 提交背压（池空/QUEUE 满） */
#define FTL_GROUP_DRAIN_TIMEOUT_MS (100U) /* 写确认停泊 / 组收尾 */

#endif /* FTL_MODULE_H */
