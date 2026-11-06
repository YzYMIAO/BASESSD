#ifndef DS_RB_TREE_TABLE_H_
#define DS_RB_TREE_TABLE_H_

#include "config.h"

/**
 * @brief 红黑树颜色常量
 */
#define RB_RED   0 /**< 红色节点 */
#define RB_BLACK 1 /**< 黑色节点 */

/**
 * @brief 红黑树表结构体（不透明指针）
 *
 * 使用红黑树实现的关联数组（键值对映射表）。
 * 支持 O(log n) 的插入、删除和查找操作。
 */
typedef struct DS_RBTREE_S ds_rbtree_s;

/**
 * @brief 红黑树节点结构体
 */
typedef struct DS_RBTREE_NODE_S
{
    void *key;   /**< 节点的键 */
    void *value; /**< 节点的值 */
    char  color; /**< 节点颜色（RB_RED 或 RB_BLACK） */

    struct DS_RBTREE_NODE_S *parent; /**< 父节点指针 */
    struct DS_RBTREE_NODE_S *left;   /**< 左子节点指针 */
    struct DS_RBTREE_NODE_S *right;  /**< 右子节点指针 */
} ds_rbtree_node_s;

/**
 * @brief 红黑树表配置结构体
 */
typedef struct DS_RBTREE_CONFIG_S
{
    int (*cmp)(const void *key1, const void *key2);  /**< 键比较函数 */
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_rbtree_cfg_s;

/* 生命周期管理 */
ds_status_e ds_rbtree_new(ds_rbtree_s **table, int (*comper)(const void *, const void *));
void        ds_rbtree_conf_init(ds_rbtree_cfg_s *conf);
ds_status_e ds_rbtree_new_conf(ds_rbtree_cfg_s *conf, ds_rbtree_s **out);
void        ds_rbtree_destroy(ds_rbtree_s *table);

/* 插入和删除 */
ds_status_e ds_rbtree_add(ds_rbtree_s *table, void *key, void *val);
ds_status_e ds_rbtree_remove(ds_rbtree_s *table, void *key, void **out);
ds_status_e ds_rbtree_remove_first(ds_rbtree_s *table, void **out);
ds_status_e ds_rbtree_remove_last(ds_rbtree_s *table, void **out);
void        ds_rbtree_remove_all(ds_rbtree_s *table);

/* 查找 */
ds_status_e ds_rbtree_get(ds_rbtree_s *table, void *key, void **out);
ds_status_e ds_rbtree_get_first_value(ds_rbtree_s *table, void **out);
ds_status_e ds_rbtree_get_last_value(ds_rbtree_s *table, void **out);
ds_status_e ds_rbtree_get_first_key(ds_rbtree_s *table, void **out);
ds_status_e ds_rbtree_get_last_key(ds_rbtree_s *table, void **out);
ds_status_e ds_rbtree_get_greater_then(ds_rbtree_s *table, void *key, void **out);
ds_status_e ds_rbtree_get_lesser_then(ds_rbtree_s *table, void *key, void **out);

/* 查询 */
size_t ds_rbtree_size(ds_rbtree_s *table);
size_t ds_rbtree_struct_size(void);
BOOL   ds_rbtree_contains_key(ds_rbtree_s *table, void *key);
size_t ds_rbtree_contains_value(ds_rbtree_s *table, void *val);

/* 遍历 */
void ds_rbtree_foreach_key(ds_rbtree_s *table, void (*fn)(void *));
void ds_rbtree_foreach_value(ds_rbtree_s *table, void (*fn)(void *));

#endif /* DS_RB_TREE_TABLE_H_ */
