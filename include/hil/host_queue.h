#ifndef SSD_HOST_QUEUE_H
#define SSD_HOST_QUEUE_H

#include "ssd_request.h"
#include "ssd_types.h"

#include <stdbool.h>

/*
 * HIL 主机请求队列 / tag 表（本仓自拟的“最小 host queue”）。
 *
 * 定位：BOT 没有 NVMe SQ/CQ/doorbell，但 HIL 仍需要一个**唯一的请求归属与 tag 分配点**，
 * 让 USB MSC / UART CLI 共用同一张表，避免各自把 ssd_req_s 散落在私有结构里。
 *
 * 映射（与主流 NVMe HIL 的概念对应，形态按 BOT 简化）：
 *   host entry (tag = 下标)  ≈ SQ slot / 命令 tag
 *   entry->request           ≈ 已解析的主机请求
 *   request.complete_flag    ≈ CQ 条目（完成轮询，usb_msc_task / cli poll）
 *   owner                    ≈ 提交队列的来源前端（USB / CLI）
 *
 * ⚠ 没有实现 SQ/CQ 环形与 doorbell：BOT 串行下没有第二个消费者；接 NVMe 时
 *   在本模块上扩展，不要把 usb_msc 的槽状态再复制一份。
 */

#define SSD_HOST_QUEUE_DEPTH (8U)

typedef enum SSD_HOST_OWNER_E
{
    SSD_HOST_OWNER_NONE = 0, /* 无前端占用 */
    SSD_HOST_OWNER_USB,      /* USB MSC 前端 */
    SSD_HOST_OWNER_CLI,      /* UART CLI 前端 */
    SSD_HOST_OWNER_COUNT     /* 枚举上界（不是合法 owner） */
} ssd_host_owner_e;

typedef struct SSD_HOST_ENTRY_S
{
    BOOL      in_use;  /* true = 该 entry 已分配 */
    BYTE      tag;     /* == 数组下标（唯一） */
    BYTE      owner;   /* ssd_host_owner_e */
    ssd_req_s request; /* 已解析主机请求（由分配者按 H47 填充） */
} ssd_host_entry_s;

typedef struct SSD_HOST_QUEUE_S
{
    ssd_host_entry_s entries[SSD_HOST_QUEUE_DEPTH]; /* 固定 tag 表：下标即 tag */
    BYTE             used;                          /* 当前占用数 */

    /* 跨前端在飞闸门（唯一来源）：NONE = 没有主机命令占用。
     * 同一前端可有多条在飞（USB 两个写 chunk / CLI wp 多组），用计数；
     * 另一前端必须等计数归零后才能占用。前端提交与释放只走
     * ssd_host_queue_gate_acquire / _release，各 handler 不再自查对方状态。 */
    ssd_host_owner_e gate_owner; /* 当前占用闸门的前端 */
    BYTE             gate_count; /* 该前端的在飞占用数 */
} ssd_host_queue_s;

/**
 * @brief 当前前端能否占用 host gate（NONE 或 owner 自己持有）。
 *
 * @param queue 队列
 * @param owner 前端（USB / CLI；NONE 非法）
 * @return true = 可占用；false = 另一前端持有
 */
BOOL ssd_host_queue_gate_allows(const ssd_host_queue_s *queue, ssd_host_owner_e owner);

/**
 * @brief 是否有任一前端占用了 host gate。
 *
 * @param queue 队列
 * @return true = 有主机命令在飞（含等待提交的 USB 槽）
 */
BOOL ssd_host_queue_gate_busy(const ssd_host_queue_s *queue);

/**
 * @brief 清零整张 host queue 表（含 gate）。
 *
 * @param queue 队列（由 ssd_ctl_s 持有）
 */
void ssd_host_queue_init(ssd_host_queue_s *queue);

/**
 * @brief 取一个空闲 entry（tag = 数组下标）。
 *
 * request 字段不清零——调用者按 H47 先 memset / 逐字段赋值。
 *
 * @param queue 队列
 * @param owner 占用前端（USB / CLI；NONE 非法）
 * @return 空闲 entry；表满返回 NULL
 */
ssd_host_entry_s *ssd_host_queue_alloc(ssd_host_queue_s *queue, ssd_host_owner_e owner);

/**
 * @brief 归还一个已分配的 entry，并清零其 request。
 *
 * @param queue 队列
 * @param entry 必须是 in_use 的 entry
 */
void ssd_host_queue_free(ssd_host_queue_s *queue, ssd_host_entry_s *entry);

/**
 * @brief 占用 host gate（同一前端可重复占用，计数递增）。
 *
 * 调用者必须先用 ssd_host_queue_gate_allows() 确认；抢占另一前端是逻辑错误。
 *
 * @param queue 队列
 * @param owner 前端（USB / CLI）
 * @return true = 已占用；false = 另一前端持有
 */
BOOL ssd_host_queue_gate_acquire(ssd_host_queue_s *queue, ssd_host_owner_e owner);

/**
 * @brief 释放 host gate（与 acquire 一一配对；计数归零后 owner 回到 NONE）。
 *
 * @param queue 队列
 * @param owner 必须与当前 gate_owner 一致
 */
void ssd_host_queue_gate_release(ssd_host_queue_s *queue, ssd_host_owner_e owner);

#endif /* SSD_HOST_QUEUE_H */
