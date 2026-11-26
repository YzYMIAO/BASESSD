#ifndef SSD_CONTROLLER_H
#define SSD_CONTROLLER_H

#include "ftl_capacity.h"
#include "ftl_interface.h"
#include "host_queue.h"
#include "ssd_request.h"

#include <stdbool.h>

/* Controller 生命周期（最小状态机；FAILED 为终态）。 */
typedef enum SSD_CTRL_STATE_E
{
    SSD_CONTROLLER_RESET = 0,    /* 上电初值 */
    SSD_CONTROLLER_INITIALIZING, /* 槽位/拓扑校验中 */
    SSD_CONTROLLER_READY,        /* 可接受 host 请求 */
    SSD_CONTROLLER_FAILED        /* 终态：初始化失败或运行时致命错误 */
} ssd_ctrl_state_e;

typedef struct SSD_CONTROLLER_S
{
    ssd_ctrl_state_e state; /* 当前生命周期状态 */

    /* FTL 实例路由表：按紧凑 die 号索引，指向该 die 的 FTL 实例。
     * 数组容量用静态上界 FTL_DIE_MAX；实际有效项 = DIE_MAX（扫描结果）。 */
    ftl_ctx_s *ftl[FTL_DIE_MAX];

    /* 在飞的 FLUSH 请求（NULL = 无）。FLUSH 是异步命令：submit 只登记并启动
     * 各实例的 flush job，完成由 ssd_controller_service 每轮推进后置
     * complete_flag。调用者的 ssd_req_s 必须活到完成（USB/CLI 都是静态对象）。 */
    ssd_req_s *flush_req;

    /* 在飞的 TRIM 请求（NULL = 无）。TRIM 现在是异步命令：submit 只登记范围，
     * ssd_controller_service 每轮按 run 推进 ftl_trim_step；完成时置 complete_flag。
     * trim_lba 是当前未处理的全局 LBA；trim_started = 当前 run 已调用 begin。 */
    ssd_req_s *trim_req;
    QWORD      trim_lba;
    BOOL       trim_started;

    /* 主机可见 LBA 上限（HIL 容量口径）。默认 = FTL_LBA_MAX；开机时按实际
     * 坏块扣减（本仓自拟的容量折中：主流是固定容量 + OP 吸收坏块，不动态缩容）。
     * ⚠ 这是**运行时**上限，L2P 表仍按编译期 FTL_LBA_MAX 定尺寸。 */
    DWORD host_lba_max;

    /* HIL 主机请求队列（tag 表）：USB MSC / UART CLI 的 ssd_req_s 都从这里取。
     * 见 host_queue.h；BOT 下没有 SQ/CQ/doorbell，这里只做归属与生命周期。 */
    ssd_host_queue_s host_queue;
} ssd_ctl_s;

/**
 * @brief 初始化 Controller，按 compact die 号绑定 FTL 实例表。
 *
 * 只检查 present die：对每个 die < DIE_MAX，ftl_by_die[die] 必须非空，且
 * 实例 topo 必须与 die 表分解出的 (ch,ce,lun) 一致（保证 LBA 路由与芯片
 * 定位语义一致）。稀疏拓扑的空洞不访问。
 *
 * @param controller Controller 上下文
 * @param ftl_by_die die → 实例指针表（长度 FTL_DIE_MAX，前 DIE_MAX 项有效）
 * @return SSD_STATUS_OK               成功
 *         SSD_STATUS_INVALID_ARGUMENT 入参非法 / 槽位缺失 / 拓扑不符
 */
ssd_status_e ssd_controller_init(ssd_ctl_s *controller, ftl_ctx_s *const *ftl_by_die);

/**
 * @brief 提交一条 host 请求（READ/WRITE/FLUSH/TRIM）。
 *
 * READ/WRITE 按 FTL 实例拆 run 后异步提交；FLUSH/TRIM 只登记并启动异步 job。
 * 请求对象必须活到 complete_flag 置 1；若发生过超时（timed_out），还必须
 * 等 ssd_controller_request_reclaimable() 为真才能释放（FTL 可能仍在引用）。
 *
 * @param controller Controller 上下文（必须 READY）
 * @param request    主机请求
 * @return SSD_STATUS_OK               已受理（含部分 run 提交失败：错误在 completion_status 中异步回执）
 *         SSD_STATUS_INVALID_ARGUMENT 状态/范围/缓冲非法
 *         SSD_STATUS_BUSY             FLUSH/TRIM 已有在飞请求
 *         SSD_STATUS_NOT_SUPPORTED    未知 opcode
 */
ssd_status_e ssd_controller_submit(ssd_ctl_s *controller, ssd_req_s *request);

/**
 * @brief 查询请求是否完成。
 *
 * @param request 请求（NULL 视为已完成）
 * @return 1 = 完成；0 = 未完成
 */
BYTE ssd_controller_is_complete(const ssd_req_s *request);

/**
 * @brief 超时后请求是否已可安全释放
 *
 * `timed_out` 只解开 host 回执，不解除 FTL / controller 对请求的引用。
 * 前端必须等本函数为真后才能归还 host entry / USB 槽。
 *
 * @param controller Controller 上下文
 * @param request    请求（非空）
 * @return true  READ/WRITE：所有 run 已完成；FLUSH/TRIM：controller 已摘指针
 *         false 仍有在飞引用，提前释放会让完成回调写已释放内存
 */
BOOL ssd_controller_request_reclaimable(const ssd_ctl_s *controller, const ssd_req_s *request);

/**
 * @brief 推进 Controller 自己的异步命令（FLUSH / TRIM）。
 *
 * 主循环每轮调用一次（在 FTL 服务之后、USB 任务之前）：本轮把所有实例的
 * flush job 推进一步；全部完成时给在飞的 FLUSH 请求置 complete_flag。
 * TRIM 请求按 run 逐段调用 ftl_trim_begin/step，全部完成时置 complete_flag。
 * 无在飞命令时立即返回。
 *
 * 为什么需要它：FLUSH 不能在 host 回调里同步自旋，但又必须由
 * HIL 的完成通道回执；FTL 侧拆成 ftl_flush_begin/step 后，这个「每轮推进 +
 * 完成回执」的职责归 Controller（HIL）。
 *
 * @param controller Controller 上下文
 */
void ssd_controller_service(ssd_ctl_s *controller);

/**
 * @brief 放弃在飞的 FLUSH 回执（host 复位 / 拔出 / 前端换命令）。
 *
 * 摘掉 `flush_req` 回执指针，并清各实例的 flush job 标志（`ftl_flush_abort`），
 * 避免旧 job 在请求对象被复用后才完成、把回执写到新请求上。在飞的数据操作有各自
 * 的异步路径，不需要回滚。
 *
 * @param controller Controller 上下文
 * @note USB 的在飞 `ssd_req_s` 位于 host queue entry 中，调用者不得在请求
 *       完成前复用/清零它。
 */
void ssd_controller_cancel_flush(ssd_ctl_s *controller);

/**
 * @brief host LBA 区间是否落在设备容量内。
 *
 * 唯一实现：CLI 与 USB MSC 两个 host 前端在提交前共用（C2/R9）。
 * 区间为 [lba, lba + count)，count > 0。
 *
 * @param controller Controller 上下文
 * @param lba        起始 LBA（host 全局坐标）
 * @param count      连续 LBA 数
 * @return true  区间完整落在 [0, host_lba_max)
 *         false count == 0 / 越界
 */
BOOL ssd_controller_lba_range_ok(const ssd_ctl_s *controller, QWORD lba, DWORD count);

/**
 * @brief host 写启动许可（按 LBA 路由到 FTL 实例）
 *
 * USB/CLI 写前端在提交 WRITE 前调用；false 时保留请求、稍后重试，读不经过
 * 本判据。
 *
 * @param controller Controller 上下文
 * @param lba        写起始 LBA
 * @return true 允许启动；false 暂缓
 */
BOOL ssd_controller_host_write_admit(const ssd_ctl_s *controller, QWORD lba);

/**
 * @brief 主机可见 LBA 上限（0 = 未初始化 ⇒ 编译期上限）。
 *
 * 只有一个读者口径：USB READ CAPACITY、CLI 范围检查与 boot 日志都必须用它，
 * 不得再直接引用 FTL_LBA_MAX（那是 L2P 表的映射空间上限）。
 *
 * @param controller Controller 上下文（可 NULL）
 * @return 运行时 host LBA 上限；未初始化时返回 FTL_LBA_MAX
 */
static inline DWORD ssd_controller_host_lba_max(const ssd_ctl_s *controller)
{
    return ((controller != NULL) && (controller->host_lba_max != 0U)) ? controller->host_lba_max : (DWORD)FTL_LBA_MAX;
}

/**
 * @brief boot 后确保空白介质带默认 MBR
 *
 * 读 LBA0：已有 55AA 引导签名则保持原样；LBA0 全 0x00（format 后未映射）
 * 则写默认 MBR。LBA0 有数据但无引导签名时不动，避免覆盖用户自建布局。
 * 调用点必须在 ftl_boot_recover 回填 free/op/spare 池之后。
 *
 * @param controller Controller 上下文（READY；非空）
 * @return SSD_STATUS_OK 已有 MBR / 已写入 / LBA0 有数据保持原样
 *         SSD_STATUS_INVALID_ARGUMENT 状态或实例非法
 *         SSD_STATUS_IO_ERROR LBA0 读失败或 MBR 写失败
 */
ssd_status_e ssd_controller_ensure_default_mbr(ssd_ctl_s *controller);

/**
 * @brief Controller 最小状态机（C1）。
 *
 * 合法转换表：RESET→INITIALIZING（boot）、INITIALIZING→READY/FAILED
 * （槽位校验结果）、READY→FAILED（运行时致命错误，预留）。
 * 非法转换直接忽略（保持现态）；FAILED 为终态。
 *
 * @param controller Controller 上下文（NULL 时安全返回）
 * @param next_state 目标状态
 */
void ssd_controller_state_transition(ssd_ctl_s *controller, ssd_ctrl_state_e next_state);

#endif /* SSD_CONTROLLER_H */
