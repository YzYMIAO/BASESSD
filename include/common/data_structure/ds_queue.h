#ifndef DS_QUEUE_H_
#define DS_QUEUE_H_

#include "config.h"

/**
 * @brief 队列结构体（不透明指针）
 *
 * 使用环形缓冲区实现的先进先出（FIFO）队列。
 */
typedef struct ds_queue_s ds_Queue;

/**
 * @brief 队列配置结构体
 */
typedef struct ds_queue_config
{
    size_t capacity; /**< 初始容量 */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_QueueConfig;

/* 生命周期管理 */
ds_Stat ds_queue_new(ds_Queue **queue);
void    ds_queue_conf_init(ds_QueueConfig *conf);
ds_Stat ds_queue_new_conf(ds_QueueConfig *conf, ds_Queue **out);
void    ds_queue_destory(ds_Queue *queue);
void    ds_queue_destory_cb(ds_Queue *queue, void (*cb)(void *));

/* 核心操作 */
ds_Stat ds_queue_push(ds_Queue *queue, void *element);
ds_Stat ds_queue_pop(ds_Queue *queue, void **out);
ds_Stat ds_queue_peek(ds_Queue *queue, void **out);

/* 遍历和查询 */
void   ds_queue_foreach(ds_Queue *queue, void (*fn)(void *));
size_t ds_queue_size(ds_Queue *queue);

#endif /* DS_QUEUE_H_ */
