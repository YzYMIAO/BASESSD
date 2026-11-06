#ifndef DS_PQUEUE_H_
#define DS_PQUEUE_H_

#include "config.h"

/**
 * @brief 优先队列结构体（不透明指针）
 *
 * 使用二叉堆（最大堆）实现的优先队列。
 * 支持 O(log n) 的插入和删除操作，O(1) 的查看堆顶操作。
 */
typedef struct DS_PQUEUE_S ds_pqueue_s;

/**
 * @brief 优先队列配置结构体
 */
typedef struct DS_PQUEUE_CONFIG_S
{
    size_t capacity;   /**< 初始容量 */
    float  exp_factor; /**< 扩容因子 */

    int (*cmp)(void const *a, void const *b);        /**< 比较函数，返回正值表示 a 优先级更高 */
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_pqueue_cfg_s;

/* 生命周期管理 */
ds_status_e ds_pqueue_new(ds_pqueue_s **pq, int (*cmp)(void const *, void const *));
void        ds_pqueue_conf_init(ds_pqueue_cfg_s *conf, int (*cmp)(void const *, void const *));
ds_status_e ds_pqueue_new_conf(ds_pqueue_cfg_s *conf, ds_pqueue_s **out);
void        ds_pqueue_destory(ds_pqueue_s *pq);
void        ds_pqueue_destory_cb(ds_pqueue_s *pq, void (*cb)(void *));

/* 核心操作 */
ds_status_e ds_pqueue_push(ds_pqueue_s *pq, void *element);
ds_status_e ds_pqueue_top(ds_pqueue_s *pq, void **out);
ds_status_e ds_pqueue_pop(ds_pqueue_s *pq, void **out);
void        ds_pqueue_heapify(ds_pqueue_s *pq, size_t index);

/* 工具 */
size_t ds_pqueue_struct_size();

#endif /* DS_PQUEUE_H_ */
