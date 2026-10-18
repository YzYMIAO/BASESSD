#ifndef DS_DEQUE_H_
#define DS_DEQUE_H_

#include "config.h"

/**
 * @brief 双端队列结构体（不透明指针）
 *
 * 使用环形缓冲区实现的双端队列，支持在两端进行 O(1) 的插入和删除操作。
 */
typedef struct ds_deque_s ds_Deque;

/**
 * @brief 双端队列配置结构体
 */
typedef struct de_deque_config_s
{
    size_t capacity; /**< 初始容量 */

    void *(*mem_alloc)(size_t size);                /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t block, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                  /**< 内存释放函数指针 */
} ds_DequeConfig;

/* 生命周期管理 */
ds_Stat ds_deque_new(ds_Deque **out);
void    ds_deque_conf_init(ds_DequeConfig *conf);
ds_Stat ds_deque_new_conf(ds_DequeConfig *conf, ds_Deque **out);
void    ds_deque_destory(ds_Deque *deque);
void    ds_deque_destory_cb(ds_Deque *deque, void (*cb)(void *));

/* 添加元素 */
ds_Stat ds_deque_add(ds_Deque *deque, void *element);
ds_Stat ds_deque_add_first(ds_Deque *deque, void *element);
ds_Stat ds_deque_add_last(ds_Deque *deque, void *element);
ds_Stat ds_deque_add_at(ds_Deque *deque, void *element, size_t index);

/* 修改元素 */
ds_Stat ds_deque_replace_at(ds_Deque *deque, void *element, size_t index, void **out);

/* 移除元素 */
ds_Stat ds_deque_remove(ds_Deque *deque, void *element);
ds_Stat ds_deque_remove_first(ds_Deque *deque, void **out);
ds_Stat ds_deque_remove_last(ds_Deque *deque, void **out);
ds_Stat ds_deque_remove_at(ds_Deque *deque, size_t index, void **out);
void    ds_deque_remove_all(ds_Deque *deque);
void    ds_deque_remove_all_cb(ds_Deque *deque, void (*cb)(void *));

/* 获取元素 */
ds_Stat ds_deque_get_at(ds_Deque *deque, size_t index, void **out);
ds_Stat ds_deque_get_first(ds_Deque *deque, void **out);
ds_Stat ds_deque_get_last(ds_Deque *deque, void **out);

/* 复制和工具 */
ds_Stat ds_deque_copy_shallow(ds_Deque *deque, ds_Deque **out);
ds_Stat ds_deque_copy_deeep(ds_Deque *deque, void *(*cp)(void *), ds_Deque **out);
ds_Stat ds_deque_trim_capacity(ds_Deque *deque);
void    ds_deque_reverse(ds_Deque *deque);

/* 查询 */
size_t             ds_deque_contains(ds_Deque *deque, void *element);
ds_Stat            ds_deque_contains_value(ds_Deque *deque, void *element, int (*cmper)(void const *, void const *));
size_t             ds_deque_size(ds_Deque const *const deque);
size_t             ds_deque_capacity(ds_Deque const *const deque);
const void *const *ds_deque_get_buffer(ds_Deque const *const deque);

/* 过滤和遍历 */
ds_Stat ds_deque_fiter_mut(ds_Deque *deque, bool (*pred)(void const *));
ds_Stat ds_deque_filter(ds_Deque *deque, bool (*pred)(void const *), ds_Deque **out);
void    ds_deque_foreach(ds_Deque *deque, void (*fn)(void *));

#endif /* DS_DEQUE_H_ */
