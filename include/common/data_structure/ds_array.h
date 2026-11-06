#ifndef DS_ARRAY_H_
#define DS_ARRAY_H_

#include "config.h"

/**
 * @brief 动态数组结构体（不透明指针）
 *
 * 提供动态数组功能，支持自动扩容、随机访问和元素操作。
 */
typedef struct DS_ARRAY_S ds_array_s;

/**
 * @brief 数组配置结构体
 *
 * 用于初始化数组时指定容量、扩容因子和内存分配器。
 */
typedef struct DS_ARRAY_CONFIG_S
{
    size_t capacity;   /**< 初始容量 */
    float  exp_factor; /**< 扩容因子（必须大于 1） */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_array_cfg_s;

/**
 * @brief 数组迭代器结构体
 *
 * 用于遍历数组元素，支持在遍历过程中安全地增删改元素。
 */
typedef struct DS_ARRAY_ITER_S
{
    ds_array_s *ar;           /**< 被遍历的数组指针 */
    size_t      index;        /**< 下一个要返回的元素索引 */
    BOOL        last_removed; /**< 上一个元素是否已被删除 */
} ds_array_iter_s;

/* 生命周期管理 */
ds_status_e ds_array_new(ds_array_s **out);
ds_status_e ds_array_new_confing(ds_array_cfg_s *ac, ds_array_s **out);
void        ds_array_confing_init(ds_array_cfg_s *ac);
void        ds_array_destroy(ds_array_s *ar);
void        ds_array_destroy_cb(ds_array_s *ar, void (*cb)(void *));

/* 添加元素 */
ds_status_e ds_array_add(ds_array_s *ar, void *element);
ds_status_e ds_array_add_at(ds_array_s *ar, void *element, size_t index);

/* 修改元素 */
ds_status_e ds_array_replace_at(ds_array_s *ar, void *element, size_t index, void **out);
ds_status_e ds_array_swap_at(ds_array_s *ar, size_t index1, size_t index2);

/* 移除元素 */
ds_status_e ds_array_remove(ds_array_s *ar, void *element, void **out);
ds_status_e ds_array_remove_at(ds_array_s *ar, size_t index, void **out);
ds_status_e ds_array_remove_last(ds_array_s *ar, void **out);
void        ds_array_remove_all(ds_array_s *ar);
void        ds_array_remove_all_free(ds_array_s *ar);

/* 获取元素 */
ds_status_e ds_array_get_at(ds_array_s *ar, size_t index, void **out);
ds_status_e ds_array_get_last(ds_array_s *ar, void **out);

/* 复制和子数组 */
ds_status_e ds_array_subrarry(ds_array_s *ar, size_t b, size_t e, ds_array_s **out);
ds_status_e ds_array_copy_shallow(ds_array_s *ar, ds_array_s **out);
ds_status_e ds_array_copy_deep(ds_array_s *ar, void *(*cp)(void *), ds_array_s **out);

/* 过滤 */
ds_status_e ds_array_filter_mut(ds_array_s *ar, BOOL (*pred)(const void *));
ds_status_e ds_array_filter(ds_array_s *ar, BOOL (*pred)(const void *), ds_array_s **out);

/* 工具函数 */
void   ds_array_reverse(ds_array_s *ar);
size_t ds_array_contains(ds_array_s *ar, void *element);
size_t ds_array_contains_value(ds_array_s *ar, void *element, int (*comper)(const void *, const void *));
size_t ds_array_size(ds_array_s *ar);
size_t ds_array_capacity(ds_array_s *ar);

/* 排序和映射 */
void ds_array_sort(ds_array_s *ar, int (*comper)(const void *, const void *));
void ds_array_map(ds_array_s *ar, void (*fn_map)(const void *));
void ds_array_reduce(ds_array_s *ar, void (*fn)(void *, void *, void *), void *result);

#endif /* DS_ARRAY_H_ */
