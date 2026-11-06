#ifndef SLIST_H_
#define SLIST_H_

#include "config.h"

/**
 * @brief 单向链表节点结构体
 */
typedef struct DS_SLIST_NODE_S
{
    void                   *data; /**< 节点存储的数据指针 */
    struct DS_SLIST_NODE_S *next; /**< 指向下一个节点的指针 */
} ds_slist_node_s;

/**
 * @brief 单向链表结构体（不透明指针）
 */
typedef struct DS_SLIST_S ds_slist_s;

/**
 * @brief 单向链表配置结构体
 */
typedef struct DS_SLIST_CONFIG_S
{
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_slist_cfg_s;

/* 生命周期管理 */
ds_status_e ds_slist_new(ds_slist_s **list);
void        ds_slist_conf_init(ds_slist_cfg_s *conf);
ds_status_e ds_slist_new_conf(ds_slist_cfg_s *conf, ds_slist_s **out);
void        ds_slist_destroy(ds_slist_s *list);
void        ds_slist_destroy_cb(ds_slist_s *list, void (*cb)(void *));

/* 添加元素 */
ds_status_e ds_slist_add(ds_slist_s *list, void *element);
ds_status_e ds_slist_add_first(ds_slist_s *list, void *element);
ds_status_e ds_slist_add_last(ds_slist_s *list, void *element);
ds_status_e ds_slist_add_at(ds_slist_s *list, void *element, size_t index);
ds_status_e ds_slist_add_all(ds_slist_s *list1, ds_slist_s *list2);
ds_status_e ds_slist_add_all_at(ds_slist_s *list1, ds_slist_s *list2, size_t index);

/* 拼接操作 */
ds_status_e ds_slist_splice(ds_slist_s *list1, ds_slist_s *list2);
ds_status_e ds_slist_splice_at(ds_slist_s *list1, ds_slist_s *list2, size_t index);

/* 移除元素 */
ds_status_e ds_slist_remove(ds_slist_s *list, void *element, void **out);
ds_status_e ds_slist_remove_first(ds_slist_s *list, void **out);
ds_status_e ds_slist_remove_last(ds_slist_s *list, void **out);
ds_status_e ds_slist_remove_at(ds_slist_s *list, size_t index, void **out);
ds_status_e ds_slist_remove_all(ds_slist_s *list);
ds_status_e ds_slist_remove_all_cb(ds_slist_s *list, void (*cb)(void *));

/* 修改元素 */
ds_status_e ds_slist_replace_at(ds_slist_s *list, void *element, size_t index, void **out);

/* 获取元素 */
ds_status_e ds_slist_get_at(ds_slist_s *list, size_t index, void **out);
ds_status_e ds_slist_get_first(ds_slist_s *list, void **out);
ds_status_e ds_slist_get_last(ds_slist_s *list, void **out);

/* 查询 */
size_t      ds_slist_contains(ds_slist_s *list, void *element);
size_t      ds_slist_contains_value(ds_slist_s *list, void *value, int (*cmp)(void *, void *));
ds_status_e ds_slist_index_of(ds_slist_s *list, void *element, size_t *index);
size_t      ds_slist_size(ds_slist_s *list);

/* 复制、子列表和转换 */
ds_status_e ds_slist_sublist(ds_slist_s *list, size_t from, size_t to, ds_slist_s **out);
ds_status_e ds_slist_copy_shallow(ds_slist_s *list, ds_slist_s **out);
ds_status_e ds_slist_copy_deep(ds_slist_s *list, ds_slist_s **out, void *(*cp)(void *), void (*cb)(void *));
ds_status_e ds_slist_to_array(ds_slist_s *list, void ***out);

/* 排序和反转 */
ds_status_e ds_slist_sort(ds_slist_s *list, int (*comper)(void const *, void const *));
void        ds_slist_reverse(ds_slist_s *list);

/* 遍历和过滤 */
void        ds_slist_foreach(ds_slist_s *list, void (*fn)(void *));
ds_status_e ds_slist_filter_mut(ds_slist_s *list, BOOL (*pred)(void *));
ds_status_e ds_slist_filter(ds_slist_s *list, BOOL (*pred)(void *), ds_slist_s **out);

#endif /* SLIST_H_ */
