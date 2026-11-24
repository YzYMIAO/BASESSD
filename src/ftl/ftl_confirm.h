#ifndef FTL_CONFIRM_H
#define FTL_CONFIRM_H

#include "fc_interface.h"
#include "ftl_internal.h"
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
 * @param ftl         实例
 * @param d           页描述
 * @param l2p_blocked 出参：true = L2P 需要先加载，调用者下轮重试
 */
ftl_status_e ftl_confirm_write(ftl_ctx_s *ftl, const ftl_page_desc_s *d, BOOL *l2p_blocked);

/**
 * @brief 检查并登记一次写失败造成的悬空映射。
 *
 * @param ftl     实例
 * @param l       全局 host LBA
 * @param old_pba 失败页原先映射的 PBA
 * @param new_g   失败页所属序列代号
 */
void ftl_dangling_check(ftl_ctx_s *ftl, DWORD l, DWORD old_pba, WORD new_g);

/**
 * @brief 透明重试一次失败的写命令。
 *
 * @param ftl     实例
 * @param pending 失败请求
 * @param old_cmd 已完成的旧命令
 */
ftl_status_e ftl_write_retry(ftl_ctx_s *ftl, pend_req_s *pending, fc_cmd_s *old_cmd);

/* ── 生命周期 ── */

/** @brief 复位本模块文件级状态（NOLOAD 段不清零）。 */
void ftl_confirm_init(void);

/** @brief 每实例推进：待确认页 / 悬空项 / 失败组结算。 */
void ftl_confirm_service(ftl_ctx_s *ftl);

#endif /* FTL_CONFIRM_H */
