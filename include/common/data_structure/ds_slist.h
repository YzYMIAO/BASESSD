#ifndef SLIST_H_
#define SLIST_H_

/**
 * @brief 单向链表节点结构体
 */
typedef struct snode_s
{
    void           *data; /**< 节点存储的数据指针 */
    struct snode_s *next; /**< 指向下一个节点的指针 */
} SNode;

/**
 * @brief 单向链表结构体（不透明指针）
 */
typedef struct ds_single_list_s ds_Slist;

/**
 * @brief 单向链表配置结构体
 */
typedef struct ds_slist_config_s
{
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_SlistConfig;

/* 生命周期管理 */
ds_Stat ds_slist_new(ds_Slist **list);
void    ds_slist_conf_init(ds_SlistConfig *conf);
ds_Stat ds_slist_new_conf(ds_SlistConfig *conf, ds_Slist **out);
void    ds_slist_destroy(ds_Slist *list);
void    ds_slist_destroy_cb(ds_Slist *list, void (*cb)(void *));

/* 添加元素 */
ds_Stat ds_slist_add(ds_Slist *list, void *element);
ds_Stat ds_slist_add_first(ds_Slist *list, void *element);
ds_Stat ds_slist_add_last(ds_Slist *list, void *element);
ds_Stat ds_slist_add_at(ds_Slist *list, void *element, size_t index);
ds_Stat ds_slist_add_all(ds_Slist *list1, ds_Slist *list2);
ds_Stat ds_slist_add_all_at(ds_Slist *list1, ds_Slist *list2, size_t index);

/* 拼接操作 */
ds_Stat ds_slist_splice(ds_Slist *list1, ds_Slist *list2);
ds_Stat ds_slist_splice_at(ds_Slist *list1, ds_Slist *list2, size_t index);

/* 移除元素 */
ds_Stat ds_slist_remove(ds_Slist *list, void *element, void **out);
ds_Stat ds_slist_remove_first(ds_Slist *list, void **out);
ds_Stat ds_slist_remove_last(ds_Slist *list, void **out);
ds_Stat ds_slist_remove_at(ds_Slist *list, size_t index, void **out);
ds_Stat ds_slist_remove_all(ds_Slist *list);
ds_Stat ds_slist_remove_all_cb(ds_Slist *list, void (*cb)(void *));

/* 修改元素 */
ds_Stat ds_slist_replace_at(ds_Slist *list, void *element, size_t index, void **out);

/* 获取元素 */
ds_Stat ds_slist_get_at(ds_Slist *list, size_t index, void **out);
ds_Stat ds_slist_get_first(ds_Slist *list, void **out);
ds_Stat ds_slist_get_last(ds_Slist *list, void **out);

/* 查询 */
size_t  ds_slist_contains(ds_Slist *list, void *element);
size_t  ds_slist_contains_value(ds_Slist *list, void *value, int (*cmp)(void *, void *));
ds_Stat ds_slist_index_of(ds_Slist *list, void *element, size_t *index);
size_t  ds_slist_size(ds_Slist *list);

/* 复制、子列表和转换 */
ds_Stat ds_slist_sublist(ds_Slist *list, size_t from, size_t to, ds_Slist **out);
ds_Stat ds_slist_copy_shallow(ds_Slist *list, ds_Slist **out);
ds_Stat ds_slist_copy_deep(ds_Slist *list, ds_Slist **out, void *(*cp)(void *), void (*cb)(void *));
ds_Stat ds_slist_to_array(ds_Slist *list, void ***out);

/* 排序和反转 */
ds_Stat ds_slist_sort(ds_Slist *list, int (*comper)(void const *, void const *));
void    ds_slist_reverse(ds_Slist *list);

/* 遍历和过滤 */
void    ds_slist_foreach(ds_Slist *list, void (*fn)(void *));
ds_Stat ds_slist_filter_mut(ds_Slist *list, bool (*pred)(void *));
ds_Stat ds_slist_filter(ds_Slist *list, bool (*pred)(void *), ds_Slist **out);

#endif /* SLIST_H_ */
