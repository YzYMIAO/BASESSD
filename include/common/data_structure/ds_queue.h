#ifndef DS_QUEUE_H_
#define DS_QUEUE_H_

#include "config.h"

/**
 * @brief 队列结构体（不透明指针）
 *
 * 使用环形缓冲区实现的先进先出（FIFO）队列。
 */
typedef struct DS_QUEUE_S ds_queue_s;

/**
 * @brief 队列配置结构体
 */
typedef struct DS_QUEUE_CONFIG_S
{
    size_t capacity; /**< 初始容量 */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_queue_cfg_s;

/* 生命周期管理 */
ds_status_e ds_queue_new(ds_queue_s **queue);
void        ds_queue_conf_init(ds_queue_cfg_s *conf);
ds_status_e ds_queue_new_conf(ds_queue_cfg_s *conf, ds_queue_s **out);
void        ds_queue_destory(ds_queue_s *queue);
void        ds_queue_destory_cb(ds_queue_s *queue, void (*cb)(void *));

/* 核心操作 */
ds_status_e ds_queue_push(ds_queue_s *queue, void *element);
ds_status_e ds_queue_pop(ds_queue_s *queue, void **out);
ds_status_e ds_queue_peek(ds_queue_s *queue, void **out);

/* 遍历和查询 */
void   ds_queue_foreach(ds_queue_s *queue, void (*fn)(void *));
size_t ds_queue_size(ds_queue_s *queue);

#endif /* DS_QUEUE_H_ */
