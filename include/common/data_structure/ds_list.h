#ifndef DS_LIST_H_
#define DS_LIST_H_

/**
 * @brief 双向链表结构体（不透明指针）
 */
typedef struct ds_List_s ds_List;


/**
 * @brief 双向链表节点结构体
 */
typedef struct node_s
{
    void *data;              /**< 节点存储的数据指针 */
    struct node_s *next;     /**< 指向下一个节点的指针 */
    struct node_s *prev;     /**< 指向前一个节点的指针 */
} Node;


/**
 * @brief 链表配置结构体
 */
typedef struct ds_list_config_s
{
    void *(*mem_alloc)  (size_t size);          /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);          /**< 内存释放函数指针 */
} ds_ListConfig;


/* 生命周期管理 */
void    ds_list_config_init  (ds_ListConfig *lc);
ds_Stat ds_list_new_conf     (ds_ListConfig const * const conf, ds_List **out);
ds_Stat ds_list_new          (ds_List **out);
void    ds_list_destroy      (ds_List *list);
void    ds_list_destroy_cb   (ds_List *list, void (*cb)(void *));

/* 添加元素 */
ds_Stat ds_list_add          (ds_List *list, void *element);
ds_Stat ds_list_add_last     (ds_List *list, void *element);
ds_Stat ds_list_add_first    (ds_List *list, void *element);
ds_Stat ds_list_add_at       (ds_List *list, void *element, size_t index);
ds_Stat ds_list_add_list_at  (ds_List *list1, ds_List *list2, size_t index);
ds_Stat ds_list_add_to_empty (ds_List *list1, ds_List *list2);
ds_Stat ds_list_add_list     (ds_List *list1, ds_List *list2);

/* 拼接操作 */
ds_Stat ds_list_splice       (ds_List *list1, ds_List *list2);
ds_Stat ds_list_splice_at    (ds_List *list1, ds_List *list2, size_t index);

/* 移除元素 */
ds_Stat ds_list_remove       (ds_List *list, void *element, void **out);
ds_Stat ds_list_remove_at    (ds_List *list, size_t index, void **out);
ds_Stat ds_list_remove_fast  (ds_List *list, void **out);
ds_Stat ds_list_remove_last  (ds_List *list, void **out);
ds_Stat ds_list_remove_all   (ds_List *list);
ds_Stat ds_list_remove_all_cb(ds_List *list, void (*cp)(void *));

/* 获取元素 */
ds_Stat ds_list_get_first    (ds_List *list, void **out);
ds_Stat ds_list_get_last     (ds_List *list, void **out);
ds_Stat ds_list_get_at       (ds_List *list, size_t index, void **out);

/* 复制、子列表和转换 */
ds_Stat ds_list_sublist      (ds_List *list, size_t base, size_t end, ds_List **out);
ds_Stat ds_list_copy_shallow (ds_List *list, ds_List **out);
ds_Stat ds_list_copy_deep    (ds_List *list, void *(*cp)(void *), ds_List **out);
ds_Stat ds_list_to_array     (ds_List *list, void ***out);

/* 查询 */
size_t  ds_list_contains     (ds_List *list, void *element);
size_t  ds_list_contains_value (ds_List *list, void *element, int (*comper)(void *, void *));
ds_Stat ds_list_index_of     (ds_List *list, void *element, int (*comper)(const void *, const void *), size_t *index);
size_t  ds_list_size         (ds_List *list);

/* 排序和反转 */
ds_Stat ds_list_sort         (ds_List *list, int (*comper)(void const *, void const *));
void    ds_list_sort_in_place(ds_List *list, int (*comper)(void * const, void * const));
void    ds_list_reverse      (ds_List *list);

/* 遍历和归约 */
void    ds_list_foreach      (ds_List *list, void (*op)(void *));
ds_Stat ds_list_reduce       (ds_List *list, void (*fn)(void *, void *, void *), void *result);

/* 过滤 */
ds_Stat ds_list_fiter_mut    (ds_List *list, bool (*pred)(void *));
ds_Stat ds_list_filter       (ds_List *list, int (*pred)(void *), ds_List **out);

#endif /* DS_LIST_H_ */
