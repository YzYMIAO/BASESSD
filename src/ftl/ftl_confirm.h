#ifndef FTL_CONFIRM_H
#define FTL_CONFIRM_H

#include "fc_interface.h"
#include "ftl_config.h"
#include "pending.h"

/* 一页的「确认描述」：wbuf 路径从 pend_req_s 填、cache 路径从已编程页表填——
 * 单一定义，避免两套口径（C9）。 */
typedef struct FTL_PAGE_DESCRIPTOR_S
{
    DWORD lba;      /* 页基址（pair 对齐） */
    DWORD new_pba;  /* plane0 页 PBA */
    DWORD new_pba1; /* plane1 页 PBA */
    BYTE  cw_bitmap;
    BOOL  seq_page; /* 顺序页（cache program）⇒ Journal 走 jrn_append_seq */
} ftl_page_desc_s;

/* ── 写确认 / 悬空检查 / 重试 ── */

/**
 * @brief 把一页写确认推进到 Journal→L2P→BST（可能因 L2P 阻塞而暂缓）。
 *
 * @note `*l2p_blocked == true` ⇒ L2P 需要先加载，调用者下轮重试。
 */
ftl_status_e ftl_confirm_write(ftl_ctx_s *ftl, const ftl_page_desc_s *d, BOOL *l2p_blocked);

/** @brief 检查并登记一次写失败造成的悬空映射。 */
void ftl_dangling_check(ftl_ctx_s *ftl, DWORD l, DWORD old_pba, WORD new_g);

/** @brief 透明重试一次失败的写命令。 */
ftl_status_e ftl_write_retry(ftl_ctx_s *ftl, pend_req_s *pending, fc_cmd_s *old_cmd);

/* ── host I/O 完成分支（定义在本文件，入口在 ftl_host_io.c）── */

/** @brief 写成功（非 cache 中间页）：逐 CW 确认 Journal + L2P + 有效计数；true = 因 region 未就绪已待入账。 */
BOOL ftl_host_io_write_ok(ftl_ctx_s *ftl, pend_req_s *pending);

/** @brief 读成功：按链序把数据拷给 caller，并做 ECC / read-disturb 记账。 */
ssd_status_e ftl_host_io_read_ok(ftl_ctx_s *ftl, fc_cmd_s *cmd, pend_req_s *pending, WORD cmd_id);

/** @brief 读失败：把子操作托管给 Read Recovery job（组不结算）。 */
void ftl_host_io_read_fail(ftl_ctx_s *ftl, pend_req_s *pending);

/** @brief 写失败：标坏失败 plane 并关闭当前 Open Super Block，L2P 保持旧映射。 */
void ftl_host_io_write_fail(ftl_ctx_s *ftl, pend_req_s *pending, const fc_cmd_status_s *completion);

/* ── 生命周期 ── */

/** @brief 复位本模块文件级状态（NOLOAD 段不清零）。 */
void ftl_confirm_init(void);

/** @brief 每实例推进：待确认页 / 悬空项 / 失败组结算。 */
void ftl_confirm_service(ftl_ctx_s *ftl);

#endif /* FTL_CONFIRM_H */
