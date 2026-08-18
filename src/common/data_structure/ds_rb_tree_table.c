#include <config.h>
#include <ds_rb_tree_table.h>

/**
 * @brief 红黑树表结构体
 *
 * 使用红黑树实现的关联数组（键值对映射表）。
 * 支持 O(log n) 的插入、删除和查找操作。
 * 键值对按键的顺序排列，支持有序遍历。
 */
struct ds_rbtree_table_s
{
    size_t  size;     /**< 树中键值对的数量 */
    RBnode *root;     /**< 根节点指针 */
    RBnode *sentinel; /**< 哨兵节点（表示空节点，所有叶子节点指向它） */

    int (*cmp)(void const *key1, void const *key2);  /**< 键比较函数指针 */
    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
};

/**
 * @brief 右旋转操作
 *
 * 对以 x 为轴心的子树进行右旋转，用于红黑树的平衡调整。
 *
 * @param[in,out] table 红黑树表
 * @param[in,out] x     旋转轴心节点
 */
static void rotate_right(ds_RBtree *table, RBnode *x)
{
    RBnode *y = x->left;

    /* 将 y 的右子树挂接到 x 的左子树 */
    x->left = y->right;

    if (y->right != table->sentinel)
    {
        y->right->parent = x;
    }

    /* 更新 y 的父指针 */
    y->parent = x->parent;

    /* 更新父节点对 y 的引用 */
    if (x->parent == table->sentinel)
    {
        table->root = y;
    }
    else if (x == x->parent->right)
    {
        x->parent->right = y;
    }
    else
    {
        x->parent->left = y;
    }

    /* 将 x 挂接到 y 的右子树 */
    y->right  = x;
    x->parent = y;
}

/**
 * @brief 左旋转操作
 *
 * 对以 x 为轴心的子树进行左旋转，用于红黑树的平衡调整。
 *
 * @param[in,out] table 红黑树表
 * @param[in,out] x     旋转轴心节点
 */
static void rotate_left(ds_RBtree *table, RBnode *x)
{
    RBnode *y = x->right;

    /* 将 y 的左子树挂接到 x 的右子树 */
    x->right = y->left;

    if (y->left != table->sentinel)
    {
        y->left->parent = x;
    }

    /* 更新 y 的父指针 */
    y->parent = x->parent;

    /* 更新父节点对 y 的引用 */
    if (x->parent == table->sentinel)
    {
        table->root = y;
    }
    else if (x == x->parent->right)
    {
        x->parent->right = y;
    }
    else
    {
        x->parent->left = y;
    }

    /* 将 x 挂接到 y 的左子树 */
    y->left   = x;
    x->parent = y;
}

/**
 * @brief 查找子树中的最小节点
 *
 * @param[in] table 红黑树表
 * @param[in] n     子树根节点
 *
 * @return 子树中最小的节点（最左节点），如果子树为空则返回哨兵节点
 */
static RBnode *tree_min(ds_RBtree const *const table, RBnode *n)
{
    RBnode *s = table->sentinel;

    if (n == s)
    {
        return s;
    }

    while (n->left != s)
    {
        n = n->left;
    }

    return n;
}

/**
 * @brief 查找子树中的最大节点
 *
 * @param[in] table 红黑树表
 * @param[in] n     子树根节点
 *
 * @return 子树中最大的节点（最右节点），如果子树为空则返回哨兵节点
 */
static RBnode *tree_max(ds_RBtree const *const table, RBnode *n)
{
    RBnode *s = table->sentinel;

    if (n == s)
    {
        return s;
    }

    while (n->right != s)
    {
        n = n->right;
    }

    return n;
}

/**
 * @brief 获取节点的后继节点
 *
 * 返回在中序遍历中位于 x 之后的下一个节点。
 *
 * @param[in] table 红黑树表
 * @param[in] x     当前节点
 *
 * @return x 的后继节点，如果 x 是最大节点则返回哨兵节点
 */
static RBnode *get_successor_node(ds_RBtree const *const table, RBnode *x)
{
    if (x == NULL)
    {
        return NULL;
    }

    /* 如果有右子树，后继为右子树中的最小节点 */
    if (x->right != table->sentinel)
    {
        return tree_min(table, x->right);
    }

    /* 否则向上查找，直到找到第一个作为左子树的祖先 */
    RBnode *y = x->parent;

    while (y != table->sentinel && x == y->right)
    {
        x = y;
        y = x->parent;
    }

    return y;
}

/**
 * @brief 获取节点的前驱节点
 *
 * 返回在中序遍历中位于 x 之前的前一个节点。
 *
 * @param[in] table 红黑树表
 * @param[in] x     当前节点
 *
 * @return x 的前驱节点，如果 x 是最小节点则返回哨兵节点
 */
static RBnode *get_predecessor_node(ds_RBtree const *const table, RBnode *x)
{
    if (x == NULL)
    {
        return NULL;
    }

    /* 如果有左子树，前驱为左子树中的最大节点 */
    if (x->left != table->sentinel)
    {
        return tree_max(table, x->left);
    }

    /* 否则向上查找，直到找到第一个作为右子树的祖先 */
    RBnode *y = x->parent;

    while (y != table->sentinel && x == y->left)
    {
        x = y;
        y = x->parent;
    }

    return y;
}

/**
 * @brief 用 v 替换 u（移植操作）
 *
 * 将节点 u 从树中移除，并用节点 v 替换其位置。
 * 注意：此操作不处理 v 的子树。
 *
 * @param[in,out] table 红黑树表
 * @param[in]     u     被替换的节点
 * @param[in,out] v     替换节点
 */
static void transplant(ds_RBtree *table, RBnode *u, RBnode *v)
{
    if (u->parent == table->sentinel)
    {
        table->root = v;
    }
    else if (u == u->parent->right)
    {
        u->parent->right = v;
    }
    else
    {
        u->parent->left = v;
    }

    v->parent = u->parent;
}

/**
 * @brief 创建一个新的红黑树表（使用默认配置）
 *
 * 使用默认配置创建红黑树表。
 *
 * @param[out] table  输出参数，存储新创建的表指针
 * @param[in]  comper 键比较函数指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码参数类型为 ds_RBtree *table（值传递），
 *          无法返回新创建的表指针，应为 ds_RBtree **table
 */
ds_Stat ds_rbtree_new(ds_RBtree **table, int (*comper)(const void *, const void *))
{
    ds_rbTreeConfig conf;

    ds_rbtree_conf_init(&conf);
    conf.cmp = comper;
    return ds_rbtree_new_conf(&conf, table);
}

/**
 * @brief 初始化红黑树表配置结构体为默认值
 *
 * 使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] conf 需要初始化的配置结构体指针
 */
void ds_rbtree_conf_init(ds_rbTreeConfig *conf)
{
    conf->cmp        = NULL;
    conf->mem_free   = free;
    conf->mem_alloc  = malloc;
    conf->mem_calloc = calloc;
}

/**
 * @brief 根据指定配置创建新的红黑树表
 *
 * 使用配置结构体中的参数创建红黑树表。
 * 同时创建哨兵节点。
 *
 * @param[in]  conf 配置结构体指针
 * @param[out] out  输出参数，存储新创建的表指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_rbtree_new_conf(ds_rbTreeConfig *conf, ds_RBtree **out)
{
    ds_RBtree *table = conf->mem_calloc(1, sizeof(ds_RBtree));

    if (table == NULL)
    {
        return DS_ERR_ALLOC;
    }

    RBnode *sentinel = conf->mem_calloc(1, sizeof(RBnode));

    if (sentinel == NULL)
    {
        conf->mem_free(table);
        return DS_ERR_ALLOC;
    }

    table->cmp        = conf->cmp;
    table->mem_alloc  = conf->mem_alloc;
    table->mem_calloc = conf->mem_calloc;
    table->mem_free   = conf->mem_free;
    table->root       = sentinel;
    table->sentinel   = sentinel;
    table->size       = 0;

    *out = table;
    return DS_OK;
}

/**
 * @brief 插入后的红黑树平衡调整
 *
 * 在插入新节点后，通过变色和旋转操作恢复红黑树的性质。
 *
 * @param[in,out] table 红黑树表
 * @param[in,out] z     新插入的节点
 */
static void rebalance_after_insert(ds_RBtree *table, RBnode *z)
{
    RBnode *y;

    /* 当父节点为红色时需要调整 */
    while (z->parent->color == RB_RED)
    {
        if (z->parent == z->parent->parent->left)
        {
            /* 父节点是祖父节点的左子节点 */
            y = z->parent->parent->right;

            if (y->color == RB_RED)
            {
                /* 情况1：叔节点为红色 → 变色 */
                y->color                 = RB_BLACK;
                z->parent->color         = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z                        = z->parent->parent;
            }
            else
            {
                /* 叔节点为黑色 */
                if (z == z->parent->right)
                {
                    /* 情况2：z 是右子节点 → 左旋 */
                    z = z->parent;
                    rotate_left(table, z);
                }
                /* 情况3：z 是左子节点 → 变色 + 右旋 */
                z->parent->color         = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_right(table, z->parent->parent);
            }
        }
        else
        {
            /* 父节点是祖父节点的右子节点（对称情况） */
            y = z->parent->parent->left;

            if (y->color == RB_RED)
            {
                /* 情况1：叔节点为红色 → 变色 */
                y->color                 = RB_BLACK;
                z->parent->color         = RB_BLACK;
                z->parent->parent->color = RB_RED;
                z                        = z->parent->parent;
            }
            else
            {
                /* 叔节点为黑色 */
                if (z == z->parent->left)
                {
                    /* 情况2：z 是左子节点 → 右旋 */
                    z = z->parent;
                    rotate_right(table, z);
                }
                /* 情况3：z 是右子节点 → 变色 + 左旋 */
                z->parent->color         = RB_BLACK;
                z->parent->parent->color = RB_RED;
                rotate_left(table, z->parent->parent);
            }
        }
    }

    /* 根节点必须为黑色 */
    table->root->color = RB_BLACK;
}

/**
 * @brief 删除后的红黑树平衡调整
 *
 * 在删除节点后，通过变色和旋转操作恢复红黑树的性质。
 *
 * @param[in,out] table 红黑树表
 * @param[in,out] x     需要调整的节点
 */
static void rebalance_after_delete(ds_RBtree *table, RBnode *x)
{
    RBnode *w;

    while (x != table->root && x->color == RB_BLACK)
    {
        if (x == x->parent->left)
        {
            /* x 是左子节点 */
            w = x->parent->right;

            if (w->color == RB_RED)
            {
                /* 情况1：兄弟节点为红色 → 变色 + 左旋 */
                w->color         = RB_BLACK;
                w->parent->color = RB_RED;
                rotate_left(table, w->parent);
                w = x->parent->right;
            }

            if (w->left->color == RB_BLACK && w->right->color == RB_BLACK)
            {
                /* 情况2：兄弟节点的两个子节点都为黑色 → 变色 */
                w->color = RB_RED;
                x        = x->parent;
            }
            else
            {
                if (w->left->color == RB_RED)
                {
                    /* 情况3：兄弟节点的左子节点为红色 → 变色 + 右旋 */
                    w->left->color = RB_BLACK;
                    w->color       = RB_RED;
                    rotate_right(table, w);
                    w = w->parent;
                }
                /* 情况4：兄弟节点的右子节点为红色 → 变色 + 左旋 */
                w->color         = w->parent->color;
                w->parent->color = RB_BLACK;
                w->right->color  = RB_BLACK;
                rotate_left(table, x->parent);
                x = table->root;
            }
        }
        else
        {
            /* x 是右子节点（对称情况） */
            w = x->parent->left;

            if (w->color == RB_RED)
            {
                /* 情况1：兄弟节点为红色 → 变色 + 右旋 */
                w->color         = RB_BLACK;
                w->parent->color = RB_RED;
                rotate_right(table, w->parent);
                w = x->parent->left;
            }

            if (w->left->color == RB_BLACK && w->right->color == RB_BLACK)
            {
                /* 情况2：兄弟节点的两个子节点都为黑色 → 变色 */
                w->color = RB_RED;
                x        = x->parent;
            }
            else
            {
                if (w->right->color == RB_RED)
                {
                    /* 情况3：兄弟节点的右子节点为红色 → 变色 + 左旋 */
                    w->right->color = RB_BLACK;
                    w->color        = RB_RED;
                    rotate_left(table, w);
                    w = w->parent;
                }
                /* 情况4：兄弟节点的左子节点为红色 → 变色 + 右旋 */
                w->color         = w->parent->color;
                w->parent->color = RB_BLACK;
                w->left->color   = RB_BLACK;
                rotate_right(table, x->parent);
                x = table->root;
            }
        }
    }

    x->color = RB_BLACK;
}

/**
 * @brief 根据键查找对应的树节点
 *
 * @param[in] table 红黑树表
 * @param[in] key   要查找的键
 *
 * @return 找到的节点指针，未找到则返回 NULL
 */
static RBnode *get_tree_node_by_key(ds_RBtree *table, void *key)
{
    RBnode *n = table->root;
    RBnode *s = table->sentinel;

    int cmp;

    do
    {
        cmp = table->cmp(key, n->key);

        if (cmp < 0)
        {
            n = n->left;
        }
        else if (cmp > 0)
        {
            n = n->right;
        }
        else
        {
            return n;
        }
    } while (n != s);

    return NULL;
}

/**
 * @brief 向红黑树表中插入键值对
 *
 * 如果键已存在，则更新对应的值。
 *
 * @param[in,out] table 目标红黑树表
 * @param[in]     key   要插入的键
 * @param[in]     val   要插入的值
 *
 * @return DS_OK 插入成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_rbtree_add(ds_RBtree *table, void *key, void *val)
{
    RBnode *y = table->sentinel;
    RBnode *x = table->root;

    /* 查找插入位置 */
    while (x != table->sentinel)
    {
        y       = x;
        int cmp = table->cmp(key, x->key);

        if (cmp < 0)
        {
            x = x->left;
        }
        else if (cmp > 0)
        {
            x = x->right;
        }
        else
        {
            /* 键已存在，更新值 */
            x->value = val;
            return DS_OK;
        }
    }

    /* 创建新节点 */
    RBnode *new = table->mem_calloc(1, sizeof(RBnode));

    if (new == NULL)
    {
        return DS_ERR_ALLOC;
    }

    new->parent = y;
    new->key    = key;
    new->value  = val;
    new->left   = table->sentinel;
    new->right  = table->sentinel;

    /* 将新节点插入树中 */
    if (y == table->sentinel)
    {
        /* 树为空，新节点为根节点 */
        new->color  = RB_BLACK;
        table->root = new;
    }
    else
    {
        new->color = RB_RED;

        if (table->cmp(key, y->key) < 0)
        {
            y->left = new;
        }
        else
        {
            y->right = new;
        }

        /* 调整红黑树性质 */
        rebalance_after_insert(table, new);
    }

    table->size++;
    return DS_OK;
}

/**
 * @brief 递归销毁树中的所有节点
 *
 * @param[in,out] table 红黑树表
 * @param[in,out] n     当前节点
 */
static void tree_destroy(ds_RBtree *table, RBnode *n)
{
    if (n == table->sentinel)
    {
        return;
    }

    tree_destroy(table, n->left);
    tree_destroy(table, n->right);

    table->mem_free(n);
}

/**
 * @brief 销毁红黑树表（不释放元素数据）
 *
 * 释放树中所有节点和表结构体的内存。
 * 注意：此函数不会释放键和值的数据。
 *
 * @param[in] table 要销毁的表指针
 */
void ds_rbtree_destroy(ds_RBtree *table)
{
    tree_destroy(table, table->root);

    table->mem_free(table->sentinel);
    table->mem_free(table);
}

/**
 * @brief 从树中移除指定节点
 *
 * @param[in,out] table 红黑树表
 * @param[in,out] z     要移除的节点
 */
static void remove_node(ds_RBtree *table, RBnode *z)
{
    RBnode *x;
    RBnode *y = z;

    char y_color = z->color;

    if (z->left == table->sentinel)
    {
        /* 只有右子树或没有子树 */
        x = z->right;
        transplant(table, z, z->right);
    }
    else if (y->right == table->sentinel)
    {
        /* 只有左子树 */
        x = z->left;
        transplant(table, z, z->left);
    }
    else
    {
        /* 有两个子树，找后继节点 */
        y       = tree_min(table, z->right);
        y_color = y->color;
        x       = y->right;

        if (y->parent == z)
        {
            x->parent = y;
        }
        else
        {
            transplant(table, y, y->right);
            y->right         = z->right;
            y->right->parent = y;
        }

        transplant(table, z, y);
        y->left         = z->left;
        y->left->parent = y;
        y->color        = z->color;
    }

    /* 如果被删除（或移动）的节点是黑色，需要调整 */
    if (y_color == RB_BLACK)
    {
        rebalance_after_delete(table, x);
    }

    table->mem_free(z);
    table->size--;
}

/**
 * @brief 根据键移除键值对
 *
 * @param[in,out] table 目标红黑树表
 * @param[in]     key   要移除的键
 * @param[out]    out   输出参数，存储被移除的值（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_KEY_NOT_FOUND 键不存在
 */
ds_Stat ds_rbtree_remove(ds_RBtree *table, void *key, void **out)
{
    RBnode *node = get_tree_node_by_key(table, key);

    if (node == NULL)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    if (out != NULL)
    {
        *out = node->value;
    }

    remove_node(table, node);
    return DS_OK;
}

/**
 * @brief 移除最小的键值对（第一个节点）
 *
 * @param[in,out] table 目标红黑树表
 * @param[out]    out   输出参数，存储被移除的值（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_KEY_NOT_FOUND 表为空
 */
ds_Stat ds_rbtree_remove_first(ds_RBtree *table, void **out)
{
    if (table->size == 0)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    RBnode *node = tree_min(table, table->root);

    if (out != NULL)
    {
        *out = node->value;
    }

    remove_node(table, node);
    return DS_OK;
}

/**
 * @brief 移除最大的键值对（最后一个节点）
 *
 * @param[in,out] table 目标红黑树表
 * @param[out]    out   输出参数，存储被移除的值（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_KEY_NOT_FOUND 表为空
 */
ds_Stat ds_rbtree_remove_last(ds_RBtree *table, void **out)
{
    if (table->size == 0)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    RBnode *node = tree_max(table, table->root);

    if (out != NULL)
    {
        *out = node->value;
    }

    remove_node(table, node);
    return DS_OK;
}

/**
 * @brief 移除表中所有的键值对
 *
 * @param[in,out] table 目标红黑树表
 */
void ds_rbtree_remove_all(ds_RBtree *table)
{
    tree_destroy(table, table->root);
    table->size = 0;
    table->root = table->sentinel;
}

/**
 * @brief 根据键获取对应的值
 *
 * @param[in]  table 目标红黑树表
 * @param[in]  key   要查找的键
 * @param[out] out   输出参数，存储找到的值
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 键不存在
 */
ds_Stat ds_rbtree_get(ds_RBtree *table, void *key, void **out)
{
    RBnode *node = get_tree_node_by_key(table, key);

    if (node == NULL)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = node->value;
    return DS_OK;
}

/**
 * @brief 获取最小键对应的值
 *
 * @param[in]  table 目标红黑树表
 * @param[out] out   输出参数，存储找到的值
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 表为空
 */
ds_Stat ds_rbtree_get_first_value(ds_RBtree *table, void **out)
{
    RBnode *node = tree_min(table, table->root);

    if (node == table->sentinel)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = node->value;
    return DS_OK;
}

/**
 * @brief 获取最大键对应的值
 *
 * @param[in]  table 目标红黑树表
 * @param[out] out   输出参数，存储找到的值
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 表为空
 */
ds_Stat ds_rbtree_get_last_value(ds_RBtree *table, void **out)
{
    RBnode *node = tree_max(table, table->root);

    if (node == table->sentinel)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = node->value;
    return DS_OK;
}

/**
 * @brief 获取最小的键
 *
 * @param[in]  table 目标红黑树表
 * @param[out] out   输出参数，存储找到的键
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 表为空
 */
ds_Stat ds_rbtree_get_first_key(ds_RBtree *table, void **out)
{
    RBnode *node = tree_min(table, table->root);

    if (node == table->sentinel)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = node->key;
    return DS_OK;
}

/**
 * @brief 获取最大的键
 *
 * @param[in]  table 目标红黑树表
 * @param[out] out   输出参数，存储找到的键
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 表为空
 */
ds_Stat ds_rbtree_get_last_key(ds_RBtree *table, void **out)
{
    RBnode *node = tree_max(table, table->root);

    if (node == table->sentinel)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = node->key;
    return DS_OK;
}

/**
 * @brief 获取大于指定键的后继键
 *
 * @param[in]  table 目标红黑树表
 * @param[in]  key   参考键
 * @param[out] out   输出参数，存储找到的后继键
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 键不存在或没有后继
 */
ds_Stat ds_rbtree_get_greater_then(ds_RBtree *table, void *key, void **out)
{
    RBnode *n = get_tree_node_by_key(table, key);
    RBnode *s = get_successor_node(table, n);

    if (n == NULL && s == NULL)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = s->key;
    return DS_OK;
}

/**
 * @brief 获取小于指定键的前驱键
 *
 * @param[in]  table 目标红黑树表
 * @param[in]  key   参考键
 * @param[out] out   输出参数，存储找到的前驱键
 *
 * @return DS_OK 查找成功，DS_ERR_KEY_NOT_FOUND 键不存在或没有前驱
 */
ds_Stat ds_rbtree_get_lesser_then(ds_RBtree *table, void *key, void **out)
{
    RBnode *n = get_tree_node_by_key(table, key);
    RBnode *s = get_predecessor_node(table, n);

    if (n == NULL && s == NULL)
    {
        return DS_ERR_KEY_NOT_FOUND;
    }

    *out = s->key;
    return DS_OK;
}

/**
 * @brief 获取表中键值对的数量
 *
 * @param[in] table 目标红黑树表
 *
 * @return 键值对的数量
 */
size_t ds_rbtree_size(ds_RBtree *table)
{
    return table->size;
}

/**
 * @brief 获取红黑树表结构体的大小
 *
 * @return 结构体 ds_RBtree 的字节大小
 */
size_t ds_rbtree_struct_size()
{
    return sizeof(ds_RBtree);
}

/**
 * @brief 检查表中是否存在指定的键
 *
 * @param[in] table 目标红黑树表
 * @param[in] key   要检查的键
 *
 * @return true 键存在，false 键不存在
 */
bool ds_rbtree_contains_key(ds_RBtree *table, void *key)
{
    RBnode *node = get_tree_node_by_key(table, key);

    if (node == NULL)
    {
        return false;
    }

    return true;
}

/**
 * @brief 统计表中指定值（按指针值）的出现次数
 *
 * @param[in] table 目标红黑树表
 * @param[in] val   要统计的值指针
 *
 * @return 值在表中出现的次数
 */
size_t ds_rbtree_contains_value(ds_RBtree *table, void *val)
{
    RBnode *node  = tree_min(table, table->root);
    size_t  count = 0;

    while (node != table->sentinel)
    {
        if (node->value == val)
        {
            count++;
        }

        node = get_successor_node(table, node);
    }

    return count;
}

/**
 * @brief 对表中的所有键执行指定函数
 *
 * 按键的顺序从小到大遍历，对每个键调用 fn 函数。
 *
 * @param[in,out] table 目标红黑树表
 * @param[in]     fn    对每个键执行的函数
 */
void ds_rbtree_foreach_key(ds_RBtree *table, void (*fn)(void *))
{
    RBnode *node = tree_min(table, table->root);

    while (node != table->sentinel)
    {
        fn(node->key);
        node = get_successor_node(table, node);
    }
}

/**
 * @brief 对表中的所有值执行指定函数
 *
 * 按键的顺序从小到大遍历，对每个值调用 fn 函数。
 *
 * @param[in,out] table 目标红黑树表
 * @param[in]     fn    对每个值执行的函数
 */
void ds_rbtree_foreach_value(ds_RBtree *table, void (*fn)(void *))
{
    RBnode *node = tree_min(table, table->root);

    while (node != table->sentinel)
    {
        fn(node->value);
        node = get_successor_node(table, node);
    }
}
