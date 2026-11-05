#ifndef SSD_HOST_QUEUE_H
#define SSD_HOST_QUEUE_H

#include "ssd_request.h"
#include "ssd_types.h"

#include <stdbool.h>

/*
 * HIL 主机请求队列 / tag 表（本仓自拟的“最小 host queue”）。
 *
 * 定位：BOT 没有 NVMe SQ/CQ/doorbell，但 HIL 仍需要一个**唯一的请求归属与 tag 分配点**，
 * 让 USB MSC / UART CLI 共用同一张表，避免各自把 ssd_request_t 散落在私有结构里。
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

typedef enum
{
    SSD_HOST_OWNER_NONE = 0, /* 无前端占用 */
    SSD_HOST_OWNER_USB,      /* USB MSC 前端 */
    SSD_HOST_OWNER_CLI,      /* UART CLI 前端 */
    SSD_HOST_OWNER_COUNT     /* 枚举上界（不是合法 owner） */
} ssd_host_owner_e;

typedef struct
{
    bool          in_use;  /* true = 该 entry 已分配 */
    BYTE          tag;     /* == 数组下标（唯一） */
    BYTE          owner;   /* ssd_host_owner_e */
    ssd_request_t request; /* 已解析主机请求（由分配者按 H47 填充） */
} ssd_host_entry_t;

typedef struct
{
    ssd_host_entry_t entries[SSD_HOST_QUEUE_DEPTH]; /* 固定 tag 表：下标即 tag */
    BYTE             used;                          /* 当前占用数 */
} ssd_host_queue_t;

/**
 * @brief 清零整张 host queue 表。
 *
 * @param queue 队列（由 ssd_controller_t 持有）
 */
void ssd_host_queue_init(ssd_host_queue_t *queue);

/**
 * @brief 取一个空闲 entry（tag = 数组下标）。
 *
 * request 字段不清零——调用者按 H47 先 memset / 逐字段赋值。
 *
 * @param queue 队列
 * @param owner 占用前端（USB / CLI；NONE 非法）
 * @return 空闲 entry；表满返回 NULL
 */
ssd_host_entry_t *ssd_host_queue_alloc(ssd_host_queue_t *queue, ssd_host_owner_e owner);

/**
 * @brief 归还一个已分配的 entry，并清零其 request。
 *
 * @param queue 队列
 * @param entry 必须是 in_use 的 entry
 */
void ssd_host_queue_free(ssd_host_queue_t *queue, ssd_host_entry_t *entry);

/**
 * @brief 当前占用数。
 *
 * @param queue 队列
 * @return 0..SSD_HOST_QUEUE_DEPTH
 */
BYTE ssd_host_queue_used(const ssd_host_queue_t *queue);

/**
 * @brief 某前端是否没有占用任何 entry。
 *
 * @param queue 队列
 * @param owner 前端（USB / CLI）
 * @return true = 该 owner 的 entry 数为 0
 */
bool ssd_host_queue_owner_idle(const ssd_host_queue_t *queue, ssd_host_owner_e owner);

#endif /* SSD_HOST_QUEUE_H */
