#ifndef DS_RB_TREE_TABLE_H_
#define DS_RB_TREE_TABLE_H_

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
typedef struct ds_rbtree_table_s ds_RBtree;

/**
 * @brief 红黑树节点结构体
 */
typedef struct ds_RBnode_s
{
    void *key;   /**< 节点的键 */
    void *value; /**< 节点的值 */
    char  color; /**< 节点颜色（RB_RED 或 RB_BLACK） */

    struct ds_RBnode_s *parent; /**< 父节点指针 */
    struct ds_RBnode_s *left;   /**< 左子节点指针 */
    struct ds_RBnode_s *right;  /**< 右子节点指针 */
} RBnode;

/**
 * @brief 红黑树表配置结构体
 */
typedef struct ds_rbtree_config_s
{
    int (*cmp)(const void *key1, const void *key2);  /**< 键比较函数 */
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_rbTreeConfig;

/* 生命周期管理 */
ds_Stat ds_rbtree_new(ds_RBtree **table, int (*comper)(const void *, const void *));
void    ds_rbtree_conf_init(ds_rbTreeConfig *conf);
ds_Stat ds_rbtree_new_conf(ds_rbTreeConfig *conf, ds_RBtree **out);
void    ds_rbtree_destroy(ds_RBtree *table);

/* 插入和删除 */
ds_Stat ds_rbtree_add(ds_RBtree *table, void *key, void *val);
ds_Stat ds_rbtree_remove(ds_RBtree *table, void *key, void **out);
ds_Stat ds_rbtree_remove_first(ds_RBtree *table, void **out);
ds_Stat ds_rbtree_remove_last(ds_RBtree *table, void **out);
void    ds_rbtree_remove_all(ds_RBtree *table);

/* 查找 */
ds_Stat ds_rbtree_get(ds_RBtree *table, void *key, void **out);
ds_Stat ds_rbtree_get_first_value(ds_RBtree *table, void **out);
ds_Stat ds_rbtree_get_last_value(ds_RBtree *table, void **out);
ds_Stat ds_rbtree_get_first_key(ds_RBtree *table, void **out);
ds_Stat ds_rbtree_get_last_key(ds_RBtree *table, void **out);
ds_Stat ds_rbtree_get_greater_then(ds_RBtree *table, void *key, void **out);
ds_Stat ds_rbtree_get_lesser_then(ds_RBtree *table, void *key, void **out);

/* 查询 */
size_t ds_rbtree_size(ds_RBtree *table);
size_t ds_rbtree_struct_size(void);
bool   ds_rbtree_contains_key(ds_RBtree *table, void *key);
size_t ds_rbtree_contains_value(ds_RBtree *table, void *val);

/* 遍历 */
void ds_rbtree_foreach_key(ds_RBtree *table, void (*fn)(void *));
void ds_rbtree_foreach_value(ds_RBtree *table, void (*fn)(void *));

#endif /* DS_RB_TREE_TABLE_H_ */
