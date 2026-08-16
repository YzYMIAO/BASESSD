#ifndef DS_ARRAY_H_
#define DS_ARRAY_H_

/**
 * @brief 动态数组结构体（不透明指针）
 *
 * 提供动态数组功能，支持自动扩容、随机访问和元素操作。
 */
typedef struct ds_Array_s ds_Array;


/**
 * @brief 数组配置结构体
 *
 * 用于初始化数组时指定容量、扩容因子和内存分配器。
 */
typedef struct ds_array_config_s
{
    size_t      capacity;    /**< 初始容量 */
    float       exp_factor;  /**< 扩容因子（必须大于 1） */

    void *(*mem_alloc)  (size_t size);          /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);          /**< 内存释放函数指针 */
} ds_ArrayConfig;


/**
 * @brief 数组迭代器结构体
 *
 * 用于遍历数组元素，支持在遍历过程中安全地增删改元素。
 */
typedef struct ds_Array_iter_s
{
    ds_Array *ar;           /**< 被遍历的数组指针 */
    size_t    index;        /**< 下一个要返回的元素索引 */
    bool      last_removed; /**< 上一个元素是否已被删除 */
} ds_ArrayIter;


/* 生命周期管理 */
ds_Stat ds_array_new          (ds_Array **out);
ds_Stat ds_array_new_confing  (ds_ArrayConfig *ac, ds_Array **out);
void    ds_array_confing_init (ds_ArrayConfig *ac);
void    ds_array_destroy      (ds_Array *ar);
void    ds_array_destroy_cb   (ds_Array *ar, void (*cb)(void *));

/* 添加元素 */
ds_Stat ds_array_add          (ds_Array *ar, void *element);
ds_Stat ds_array_add_at       (ds_Array *ar, void *element, size_t index);

/* 修改元素 */
ds_Stat ds_array_replace_at   (ds_Array *ar, void *element, size_t index, void **out);
ds_Stat ds_array_swap_at      (ds_Array *ar, size_t index1, size_t index2);

/* 移除元素 */
ds_Stat ds_array_remove       (ds_Array *ar, void *element, void **out);
ds_Stat ds_array_remove_at    (ds_Array *ar, size_t index, void **out);
ds_Stat ds_array_remove_last  (ds_Array *ar, void **out);
void    ds_array_remove_all   (ds_Array *ar);
void    ds_array_remove_all_free (ds_Array *ar);

/* 获取元素 */
ds_Stat ds_array_get_at       (ds_Array *ar, size_t index, void **out);
ds_Stat ds_array_get_last     (ds_Array *ar, void **out);

/* 复制和子数组 */
ds_Stat ds_array_subrarry     (ds_Array *ar, size_t b, size_t e, ds_Array **out);
ds_Stat ds_array_copy_shallow (ds_Array *ar, ds_Array **out);
ds_Stat ds_array_copy_deep    (ds_Array *ar, void *(*cp)(void *), ds_Array **out);

/* 过滤 */
ds_Stat ds_array_filter_mut   (ds_Array *ar, bool (*pred)(const void *));
ds_Stat ds_array_filter       (ds_Array *ar, bool (*pred)(const void *), ds_Array **out);

/* 工具函数 */
void    ds_array_reverse      (ds_Array *ar);
size_t  ds_array_contains     (ds_Array *ar, void *element);
size_t  ds_array_contains_value (ds_Array *ar, void *element, int (*comper)(const void *, const void *));
size_t  ds_array_size         (ds_Array *ar);
size_t  ds_array_capacity     (ds_Array *ar);

/* 排序和映射 */
void    ds_array_sort         (ds_Array *ar, int (*comper)(const void *, const void *));
void    ds_array_map          (ds_Array *ar, void (*fn_map)(const void *));
void    ds_array_reduce       (ds_Array *ar, void (*fn)(void *, void *, void *), void *result);

#endif /* DS_ARRAY_H_ */
