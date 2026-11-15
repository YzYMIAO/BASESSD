#ifndef SSD_RUNTIME_H
#define SSD_RUNTIME_H

/*
 * runtime.h — 主循环后台服务的一次推进。
 *
 * 把「FTL/FC 泵 + controller + USB + timer」收敛成唯一入口，主循环和需要
 * 同步等待完成的 CLI 命令（wr/rd/tr/fl/wp）共用同一套推进顺序，避免命令
 * handler 自己手抄一份服务顺序。
 *
 * ⚠ 本函数不解析 CLI、不读写命令缓冲；只能在普通上下文中调用。
 */

/**
 * @brief 推进一次后台服务。
 *
 * @note 顺序固定：FTL/FC → controller → USB → timer。必须在 USB 任务之前推进
 *       controller，USB 前端才能在同一轮看到 complete_flag。
 */
void ssd_runtime_pump(void);

#endif /* SSD_RUNTIME_H */
