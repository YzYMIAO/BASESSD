#ifndef DS_PQUEUE_H_
#define DS_PQUEUE_H_

/**
 * @brief 优先队列结构体（不透明指针）
 *
 * 使用二叉堆（最大堆）实现的优先队列。
 * 支持 O(log n) 的插入和删除操作，O(1) 的查看堆顶操作。
 */
typedef struct ds_pqueue_s ds_Pqueue;

/**
 * @brief 优先队列配置结构体
 */
typedef struct ds_pqueue_config_s
{
    size_t capacity;   /**< 初始容量 */
    float  exp_factor; /**< 扩容因子 */

    int (*cmp)(void const *a, void const *b);        /**< 比较函数，返回正值表示 a 优先级更高 */
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_PqueueConfig;

/* 生命周期管理 */
ds_Stat ds_pqueue_new(ds_Pqueue **pq, int (*cmp)(void const *, void const *));
void    ds_pqueue_conf_init(ds_PqueueConfig *conf, int (*cmp)(void const *, void const *));
ds_Stat ds_pqueue_new_conf(ds_PqueueConfig *conf, ds_Pqueue **out);
void    ds_pqueue_destory(ds_Pqueue *pq);
void    ds_pqueue_destory_cb(ds_Pqueue *pq, void (*cb)(void *));

/* 核心操作 */
ds_Stat ds_pqueue_push(ds_Pqueue *pq, void *element);
ds_Stat ds_pqueue_top(ds_Pqueue *pq, void **out);
ds_Stat ds_pqueue_pop(ds_Pqueue *pq, void **out);
void    ds_pqueue_heapify(ds_Pqueue *pq, size_t index);

/* 工具 */
size_t ds_pqueue_struct_size();

#endif /* DS_PQUEUE_H_ */
