#ifndef SSD_CONTROLLER_H
#define SSD_CONTROLLER_H

#include "ftl.h"
#include "ssd_request.h"

#include <stdbool.h>

typedef enum
{
    SSD_CONTROLLER_RESET = 0,
    SSD_CONTROLLER_INITIALIZING,
    SSD_CONTROLLER_READY,
    SSD_CONTROLLER_FAILED
} ssd_ctrl_state_e;

typedef struct
{
    ssd_ctrl_state_e state;

    /* FTL 实例路由表：按紧凑 die 号索引，指向该 die 的 FTL 实例。
     * 数组容量用静态上界 FTL_DIE_MAX；实际有效项 = DIE_MAX（扫描结果）。 */
    ftl_ctx_t *ftl[FTL_DIE_MAX];

    /* 在飞的 FLUSH 请求（NULL = 无）。FLUSH 是异步命令：submit 只登记并启动
     * 各实例的 flush job，完成由 ssd_controller_service 每轮推进后置
     * complete_flag。调用者的 ssd_request_t 必须活到完成（USB/CLI 都是静态对象）。 */
    ssd_request_t *flush_req;

    /* 在飞的 TRIM 请求（NULL = 无）。TRIM 现在是异步命令：submit 只登记范围，
     * ssd_controller_service 每轮按 run 推进 ftl_trim_step；完成时置 complete_flag。
     * trim_lba 是当前未处理的全局 LBA；trim_started = 当前 run 已调用 begin。 */
    ssd_request_t *trim_req;
    QWORD          trim_lba;
    bool           trim_started;

    /* 主机可见 LBA 上限（HIL 容量口径）。默认 = FTL_LBA_MAX；开机时按实际
     * 坏块扣减（本仓自拟的容量折中：主流是固定容量 + OP 吸收坏块，不动态缩容）。
     * ⚠ 这是**运行时**上限，L2P 表仍按编译期 FTL_LBA_MAX 定尺寸。 */
    DWORD host_lba_max;
} ssd_controller_t;

/*
 * ssd_controller_init - 初始化 Controller，按 compact die 号绑定 FTL 实例表
 *
 * 只检查 present die：对每个 die < DIE_MAX，ftl_by_die[die] 必须非空，且
 * 实例 topo 必须与 die 表分解出的 (ch,ce,lun) 一致（保证 LBA 路由与芯片
 * 定位语义一致）。稀疏拓扑的空洞不访问。
 *
 * @param controller Controller 上下文
 * @param ftl_by_die die → 实例指针表（长度 FTL_DIE_MAX，前 DIE_MAX 项有效）
 * @return SSD_STATUS_OK 成功，SSD_STATUS_INVALID_ARGUMENT 入参非法/槽位缺失/拓扑不符
 */
ssd_status_e ssd_controller_init(ssd_controller_t *controller, ftl_ctx_t *const *ftl_by_die);
ssd_status_e ssd_controller_submit(ssd_controller_t *controller, ssd_request_t *request);
BYTE         ssd_controller_is_complete(const ssd_request_t *request);

/*
 * ssd_controller_service - 推进 Controller 自己的异步命令（FLUSH / TRIM）
 *
 * 主循环每轮调用一次（在 FTL 服务之后、USB 任务之前）：本轮把所有实例的
 * flush job 推进一步；全部完成时给在飞的 FLUSH 请求置 complete_flag。
 * TRIM 请求按 run 逐段调用 ftl_trim_begin/step，全部完成时置 complete_flag。
 * 无在飞命令时立即返回。
 *
 * 为什么需要它：FLUSH 不能在 host 回调里同步自旋（RISK-USB-06），但又必须由
 * HIL 的完成通道回执；FTL 侧拆成 ftl_flush_begin/step 后，这个「每轮推进 +
 * 完成回执」的职责归 Controller（HIL）。
 */
void ssd_controller_service(ssd_controller_t *controller);

/*
 * ssd_controller_cancel_flush - 放弃在飞的 FLUSH 回执（host 复位 / 拔出 / 前端换命令）
 *
 * 只清 `flush_req` 指针，**不**中止 FTL 侧的 flush job：job 会按自己的完成判据或
 * 1 s 超时收敛。这样避免「请求对象已被前端复用，controller 迟到的完成回执照样写回
 * 新请求」的竞态（USB 的在飞 `ssd_request_t` 位于 usb_msc.c 的槽对象里，
 * 调用者不得在请求完成前复用/清零它）。
 */
void ssd_controller_cancel_flush(ssd_controller_t *controller);

/*
 * ssd_controller_lba_range_ok - host LBA 区间是否落在设备容量内
 *
 * 唯一实现：CLI 与 USB MSC 两个 host 前端在提交前共用（C2/R9）。
 * 区间为 [lba, lba + count)，count > 0。
 *
 * @param lba    起始 LBA（host 全局坐标）
 * @param count  连续 LBA 数
 * @return true  区间完整落在 [0, FTL_LBA_MAX)
 */
bool ssd_controller_lba_range_ok(const ssd_controller_t *controller, QWORD lba, DWORD count);

/*
 * ssd_controller_host_lba_max - 主机可见 LBA 上限（0 = 未初始化 ⇒ 编译期上限）
 *
 * 只有一个读者口径：USB READ CAPACITY、CLI 范围检查与 boot 日志都必须用它，
 * 不得再直接引用 FTL_LBA_MAX（那是 L2P 表的映射空间上限）。
 */
static inline DWORD ssd_controller_host_lba_max(const ssd_controller_t *controller)
{
    return ((controller != NULL) && (controller->host_lba_max != 0U)) ? controller->host_lba_max : (DWORD)FTL_LBA_MAX;
}

/*
 * ssd_controller_state_transition - Controller 最小状态机（C1，state_machine.c）
 *
 * 合法转换表：RESET→INITIALIZING（boot）、INITIALIZING→READY/FAILED
 * （槽位校验结果）、READY→FAILED（运行时致命错误，预留）。
 * 非法转换直接忽略（保持现态）；FAILED 为终态。
 *
 * @param controller Controller 上下文
 * @param next_state 目标状态
 */
void ssd_controller_state_transition(ssd_controller_t *controller, ssd_ctrl_state_e next_state);

/*
 * ssd_controller_schedule - Controller 优先级调度钩子（C1b，scheduler.c）
 *
 * host IO 到来（ssd_controller_submit 每次提交）时调用本函数，
 * 返回 0 = 后台让路（host IO 优先）；空闲时返回 1 = 全力后台。
 * 实际后台预算由 ftl_service 的 B3 空闲度判定承担，本钩子只做观测点。
 *
 * @param controller     Controller 上下文
 * @param host_io_active 1=有 host IO 提交，0=空闲
 * @return 后台预算指示：0=最小后台，1=全力后台
 */
int ssd_controller_schedule(ssd_controller_t *controller, int host_io_active);

#endif /* SSD_CONTROLLER_H */
