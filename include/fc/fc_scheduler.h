#ifndef FC_SCHEDULER_H
#define FC_SCHEDULER_H

#include "fc_queue.h"
#include "nand_config.h"
#include "nand_dispatch.h"

/*
 * 调度器：HSQ 优先于普通 SQ。
 *
 * 本模块只承载调度器状态与"命令链"描述类型；**选择逻辑在 fc.c 的填充轮次**
 * （fc_poll_fill）——它需要把选中的命令立即交给 fc_poll_dispatch_single/chain，
 * 与 FC 提交路径同文件。
 *
 * 一个 LUN 能否接收命令，查 NAND 侧 per-LUN FIFO 的剩余槽
 * （nand_dispatch_lun_free）；填充时按此判断能否放下整条链。
 */

/* 最长命令链：与 nand_dispatch 的 per-LUN FIFO 深度一致 */
#define FC_SCHEDULER_CHAIN_MAX NAND_DISPATCH_CHAIN_DEPTH

typedef struct
{
    fc_command_id_t ids[FC_SCHEDULER_CHAIN_MAX];
    BYTE            count; /* 0=空, 1=单命令, 2+=链 */
} fc_scheduler_chain_t;

/* ── 链归属队列（per-LUN 链锁）──
 *
 * 问题：一条链可能长于 NAND per-LUN FIFO 深（FC_SCHEDULER_CHAIN_MAX = 4），会被
 * fc_poll_chain_length 截成多批、分多次填充轮次下发。批与批之间若没有**跨轮次** 的
 * 状态，另一条队列（HSQ 或 SQ）的命令就会插进链的「已下发部分」与「还没下发的部分」
 * 之间，把一条本该连续执行的链劈开。
 *
 * 锁的粒度是「哪条提交队列能喂这个 LUN」，**不是**「这个 LUN 不能用」：
 *   · 链所在的队列（SQ 或 HSQ）照常能喂它——链的续批正是从这条队列取的；
 *   · 另一条队列一律回推，直到链的 END 下发。
 * 效果对称：发 SQ 链时 HSQ 插不进来，发 HSQ 链时 SQ 插不进来。
 * 代价（刻意）：打到该 LUN 的另一条队列命令被延后到链结束——「链不可被插入」优先于
 * 「HSQ 抢占」。
 * 它与 fc_poll_fill 的共享 suspended（防 room 不足时优先级倒挂）**正交**：那条管本趟
 * 空间、每趟清零；本表管「链没完」、跨趟保留。准入只回推、不置 suspended。
 *
 * 前置契约：链成员必须在**同一条提交队列**里**连续入队**。这是「只让拥有者队列喂」
 * 就够的全部理由——续批因此恒是该 LUN 在队列里**最老**的那几条，填充轮次按序扫描
 * 必然先取到它们。现有 fc_submit 一次只提交一条，连续性**无人保证**，靠调用者自觉
 * （与 fc_poll_chain_length 依赖的假设是同一个）。
 *
 * 读写全在 fc_poll_fill_queue（fc.c）：准入判断 + 下发后按批尾 chain 位更新。 */
typedef enum
{
    FC_CHAIN_OWNER_NONE = 0U, /* 该 LUN 上没有未结束的链 */
    FC_CHAIN_OWNER_SQ,        /* 未结束的链在普通队列里 */
    FC_CHAIN_OWNER_HSQ        /* 未结束的链在高优先队列里 */
} fc_chain_owner_e;

typedef struct
{
    fc_submission_queue_t *sq;
    fc_submission_queue_t *hsq;
    /* 链锁：链一旦开始，该 LUN 的填充权归链所在的队列独占，直到链的 END 下发。
     * 取值 fc_chain_owner_e。**跨填充轮次持久**——fc_poll_fill 的 suspended[] 每趟
     * memset 清零，做不到跨趟保留，所以这份状态必须挂在控制器上。
     * 使用者：FTL 的 cache read / cache program run（整条序列标成一条 chain）。
     * 单命令（chain = END，链长 1）下发后恒写 NONE，即普通命令流不改变准入。 */
    BYTE chain_owner[CH_NUM][CE_NUM][LUN_NUM];
} fc_scheduler_t;

/**
 * @brief 初始化调度器（绑定 SQ/HSQ）。
 *
 * @param scheduler 调度器。
 * @param sq        提交队列（SQ）。
 * @param hsq       高优先级提交队列（HSQ）。
 *
 * @return FC_STATUS_OK。
 *
 * @note 实现要点：入参前置条件由调用者保证（fc.c 传入本池的调度器与两条队列）。
 */
fc_status_e fc_scheduler_init(fc_scheduler_t *scheduler, fc_submission_queue_t *sq, fc_submission_queue_t *hsq);

#endif /* FC_SCHEDULER_H */
