#ifndef DS_LIST_H_
#define DS_LIST_H_

#include "config.h"

/**
 * @brief 双向链表结构体（不透明指针）
 */
typedef struct DS_LIST_S ds_list_s;

/**
 * @brief 双向链表节点结构体
 */
typedef struct DS_LIST_NODE_S
{
    void                  *data; /**< 节点存储的数据指针 */
    struct DS_LIST_NODE_S *next; /**< 指向下一个节点的指针 */
    struct DS_LIST_NODE_S *prev; /**< 指向前一个节点的指针 */
} ds_list_node_s;

/**
 * @brief 链表配置结构体
 */
typedef struct DS_LIST_CONFIG_S
{
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_list_cfg_s;

/* 生命周期管理 */
void        ds_list_config_init(ds_list_cfg_s *lc);
ds_status_e ds_list_new_conf(ds_list_cfg_s const *const conf, ds_list_s **out);
ds_status_e ds_list_new(ds_list_s **out);
void        ds_list_destroy(ds_list_s *list);
void        ds_list_destroy_cb(ds_list_s *list, void (*cb)(void *));

/* 添加元素 */
ds_status_e ds_list_add(ds_list_s *list, void *element);
ds_status_e ds_list_add_last(ds_list_s *list, void *element);
ds_status_e ds_list_add_first(ds_list_s *list, void *element);
ds_status_e ds_list_add_at(ds_list_s *list, void *element, size_t index);
ds_status_e ds_list_add_list_at(ds_list_s *list1, ds_list_s *list2, size_t index);
ds_status_e ds_list_add_to_empty(ds_list_s *list1, ds_list_s *list2);
ds_status_e ds_list_add_list(ds_list_s *list1, ds_list_s *list2);

/* 拼接操作 */
ds_status_e ds_list_splice(ds_list_s *list1, ds_list_s *list2);
ds_status_e ds_list_splice_at(ds_list_s *list1, ds_list_s *list2, size_t index);

/* 移除元素 */
ds_status_e ds_list_remove(ds_list_s *list, void *element, void **out);
ds_status_e ds_list_remove_at(ds_list_s *list, size_t index, void **out);
ds_status_e ds_list_remove_fast(ds_list_s *list, void **out);
ds_status_e ds_list_remove_last(ds_list_s *list, void **out);
ds_status_e ds_list_remove_all(ds_list_s *list);
ds_status_e ds_list_remove_all_cb(ds_list_s *list, void (*cp)(void *));

/* 获取元素 */
ds_status_e ds_list_get_first(ds_list_s *list, void **out);
ds_status_e ds_list_get_last(ds_list_s *list, void **out);
ds_status_e ds_list_get_at(ds_list_s *list, size_t index, void **out);

/* 复制、子列表和转换 */
ds_status_e ds_list_sublist(ds_list_s *list, size_t base, size_t end, ds_list_s **out);
ds_status_e ds_list_copy_shallow(ds_list_s *list, ds_list_s **out);
ds_status_e ds_list_copy_deep(ds_list_s *list, void *(*cp)(void *), ds_list_s **out);
ds_status_e ds_list_to_array(ds_list_s *list, void ***out);

/* 查询 */
size_t      ds_list_contains(ds_list_s *list, void *element);
size_t      ds_list_contains_value(ds_list_s *list, void *element, int (*comper)(void *, void *));
ds_status_e ds_list_index_of(ds_list_s *list, void *element, int (*comper)(const void *, const void *), size_t *index);
size_t      ds_list_size(ds_list_s *list);

/* 排序和反转 */
ds_status_e ds_list_sort(ds_list_s *list, int (*comper)(void const *, void const *));
void        ds_list_sort_in_place(ds_list_s *list, int (*comper)(void *const, void *const));
void        ds_list_reverse(ds_list_s *list);

/* 遍历和归约 */
void        ds_list_foreach(ds_list_s *list, void (*op)(void *));
ds_status_e ds_list_reduce(ds_list_s *list, void (*fn)(void *, void *, void *), void *result);

/* 过滤 */
ds_status_e ds_list_fiter_mut(ds_list_s *list, BOOL (*pred)(void *));
ds_status_e ds_list_filter(ds_list_s *list, int (*pred)(void *), ds_list_s **out);

#endif /* DS_LIST_H_ */
