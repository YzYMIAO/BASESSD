#include "ds_list.h"

#include "config.h"

/**
 * @brief 双向链表结构体
 *
 * 使用双向链表实现，每个节点包含指向前驱和后继节点的指针。
 * 支持在任意位置进行高效的插入和删除操作。
 */
struct ds_List_s
{
    size_t size; /**< 链表中元素的个数 */
    Node  *head; /**< 指向链表头节点的指针 */
    Node  *tail; /**< 指向链表尾节点的指针 */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
};

/* 静态函数声明 */
static void  splice_between(ds_List *list1, ds_List *list2, Node *left, Node *right);
static Node *get_node(ds_List *list, void *element);
static void *unlinkn(ds_List *list, Node *node);

/**
 * @brief 获取指定索引位置的节点指针
 *
 * 从链表头或尾开始遍历，选择距离目标索引较近的一端开始搜索，
 * 以提高查找效率（O(n/2)）。
 *
 * @param[in]  list  目标链表
 * @param[in]  index 要获取的节点逻辑索引
 * @param[out] out   输出参数，存储获取到的节点指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
static ds_Stat get_node_at(ds_List *list, size_t index, Node **out)
{
    if (list->size == 0 || index >= list->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    Node *node = NULL;

    /* 选择从头部还是尾部开始遍历，以减少遍历次数 */
    if (index < list->size / 2)
    {
        /* 从头部开始向后遍历 */
        node = list->head;

        for (size_t i = 0; i < index; i++)
        {
            node = node->next;
        }
    }
    else
    {
        /* 从尾部开始向前遍历 */
        node = list->tail;

        for (size_t i = list->size - 1; i > index; i--)
        {
            node = node->prev;
        }
    }

    *out = node;
    return DS_OK;
}

/**
 * @brief 在指定节点之前插入新节点
 *
 * 将新节点插入到 base 节点的前面。如果 base 为 NULL，则行为未定义。
 * 此函数会正确处理新节点可能已经在其他链表中的情况。
 *
 * @param[in] base 基准节点（新节点将插入到其前面）
 * @param[in] ins  要插入的新节点
 */
static void link_behind(Node *const base, Node *ins)
{
    /* 从原链表中移除 ins 节点 */
    if (ins->prev != NULL)
    {
        ins->prev->next = ins->next;
    }

    if (ins->next != NULL)
    {
        ins->next->prev = ins->prev;
    }

    /* 将 ins 插入到 base 前面 */
    if (base->prev == NULL)
    {
        /* base 是头节点，ins 成为新的头节点 */
        ins->prev       = base->prev; /* NULL */
        ins->next       = base;
        ins->next->prev = ins;
    }
    else
    {
        ins->next       = base;
        ins->prev       = base->prev;
        ins->prev->next = ins;
        ins->next->prev = ins;
    }
}

/**
 * @brief 在指定节点之后插入新节点
 *
 * 将新节点插入到 base 节点的后面。
 * 此函数当前未被使用（__attribute__((unused))），保留供将来使用。
 *
 * @param[in] base 基准节点（新节点将插入到其后面）
 * @param[in] ins  要插入的新节点
 */
static void __attribute__((unused)) link_after(Node *const base, Node *ins)
{
    /* 从原链表中移除 ins 节点 */
    if (ins->prev != NULL)
    {
        ins->prev->next = ins->next;
    }

    if (ins->next != NULL)
    {
        ins->next->prev = ins->prev;
    }

    /* 将 ins 插入到 base 后面 */
    if (base->next == NULL)
    {
        /* base 是尾节点，ins 成为新的尾节点 */
        ins->next       = base->next; /* NULL */
        ins->prev       = base;
        ins->prev->next = ins;
    }
    else
    {
        ins->next       = base->next;
        ins->prev       = base;
        ins->next->prev = ins;
        ins->prev->next = ins;
    }
}

/**
 * @brief 创建链表所有节点的外部拷贝
 *
 * 遍历源链表，为每个节点创建新的节点副本（浅拷贝数据指针），
 * 并链接成新的链表。
 *
 * @param[in]  list 源链表
 * @param[out] head 输出参数，存储新链表的头节点指针
 * @param[out] tail 输出参数，存储新链表的尾节点指针
 *
 * @return true 拷贝成功，false 内存分配失败（已清理已分配的内存）
 */
static bool link_all_externally(ds_List *list, Node **head, Node **tail)
{
    Node *iter = list->head;

    for (size_t i = 0; i < list->size; i++)
    {
        Node *new = list->mem_calloc(1, sizeof(Node));

        if (new == NULL)
        {
            /* 内存分配失败，清理已分配的节点 */
            while (*head != NULL)
            {
                Node *tmp = *head;
                *head     = tmp->next;
                list->mem_free(tmp);
            }
            return false;
        }

        new->data = iter->data;

        if (*head == NULL)
        {
            /* 第一个节点 */
            *head = new;
            *tail = new;
        }
        else
        {
            /* 链接到链表末尾 */
            (*tail)->next       = new;
            (*tail)->next->prev = *tail;
            *tail               = new;
        }

        iter = iter->next;
    }

    return true;
}

/**
 * @brief 初始化链表配置结构体为默认值
 *
 * 使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] lc 需要初始化的配置结构体指针
 */
void ds_list_config_init(ds_ListConfig *lc)
{
    lc->mem_alloc  = malloc;
    lc->mem_calloc = calloc;
    lc->mem_free   = free;
}

/**
 * @brief 根据指定配置创建新的链表
 *
 * 使用配置结构体中的内存分配器创建链表结构体。
 *
 * @param[in]  conf 链表配置结构体指针
 * @param[out] out  输出参数，存储新创建的链表指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_new_conf(ds_ListConfig const *const conf, ds_List **out)
{
    ds_List *list = conf->mem_calloc(1, sizeof(ds_List));

    if (list == NULL)
    {
        return DS_ERR_ALLOC;
    }

    list->mem_alloc  = conf->mem_alloc;
    list->mem_calloc = conf->mem_calloc;
    list->mem_free   = conf->mem_free;

    *out = list;
    return DS_OK;
}

/**
 * @brief 创建一个新的链表（使用默认配置）
 *
 * 使用默认配置创建链表，实际创建工作由 ds_list_new_conf() 完成。
 *
 * @param[out] out 输出参数，存储新创建的链表指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_new(ds_List **out)
{
    ds_ListConfig lc;

    ds_list_config_init(&lc);
    return ds_list_new_conf(&lc, out);
}

/**
 * @brief 向链表末尾添加元素
 *
 * 等同于 ds_list_add_last()。
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_add(ds_List *list, void *element)
{
    return ds_list_add_last(list, element);
}

/**
 * @brief 向链表尾部添加元素
 *
 * 在链表末尾插入新元素。
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_add_last(ds_List *list, void *element)
{
    Node *node = list->mem_calloc(1, sizeof(Node));

    if (node == NULL)
    {
        return DS_ERR_ALLOC;
    }

    node->data = element;

    if (list->size == 0)
    {
        /* 链表为空，新节点同时作为头节点和尾节点 */
        list->head = node;
        list->tail = node;
    }
    else
    {
        /* 链接到链表尾部 */
        node->prev       = list->tail;
        list->tail->next = node;
        list->tail       = node;
    }

    list->size++;
    return DS_OK;
}

/**
 * @brief 向链表头部添加元素
 *
 * 在链表头部插入新元素。
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_add_first(ds_List *list, void *element)
{
    Node *node = list->mem_calloc(1, sizeof(Node));

    if (node == NULL)
    {
        return DS_ERR_ALLOC;
    }

    node->data = element;

    if (list->size == 0)
    {
        /* 链表为空，新节点同时作为头节点和尾节点 */
        list->head = node;
        list->tail = node;
    }
    else
    {
        /* 链接到链表头部 */
        node->next       = list->head;
        list->head->prev = node;
        list->head       = node;
    }

    list->size++;
    return DS_OK;
}

/**
 * @brief 在指定索引位置插入元素
 *
 * 在链表的指定逻辑索引处插入新元素。
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 要插入的元素指针
 * @param[in]     index   插入位置的逻辑索引（0 ~ size）
 *
 * @return DS_OK 插入成功，DS_ERR_OUT_OF_RANGE 索引超出范围，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @note 当 index == size 时，等同于 ds_list_add_last()
 */
ds_Stat ds_list_add_at(ds_List *list, void *element, size_t index)
{
    Node *base = NULL;

    if (get_node_at(list, index, &base) != DS_OK)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    Node *new = list->mem_calloc(1, sizeof(Node));

    if (new == NULL)
    {
        return DS_ERR_ALLOC;
    }

    new->data = element;

    link_behind(base, new);

    if (index == 0)
    {
        list->head = new;
    }

    list->size++;
    return DS_OK;
}

/**
 * @brief 在指定位置插入另一个链表的所有元素
 *
 * 将 list2 中的所有元素（浅拷贝）插入到 list1 的指定索引位置。
 * list2 保持不变。
 *
 * @param[in,out] list1 目标链表
 * @param[in]     list2 源链表（不会被修改）
 * @param[in]     index 插入位置
 *
 * @return DS_OK 插入成功，DS_ERR_OUT_OF_RANGE 索引超出范围，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_add_list_at(ds_List *list1, ds_List *list2, size_t index)
{
    if (index > list1->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (list2->size == 0)
    {
        return DS_OK;
    }

    /* 创建 list2 的节点副本 */
    Node *head = NULL;
    Node *tail = NULL;

    if (link_all_externally(list2, &head, &tail) == false)
    {
        return DS_ERR_ALLOC;
    }

    /* 获取插入位置的边界节点 */
    Node *base = NULL;
    Node *end  = NULL;

    get_node_at(list1, index, &end);

    if (end != NULL)
    {
        base = end->prev;
    }
    else
    {
        get_node_at(list1, index - 1, &base);
    }

    /* 将新链表链接到 list1 中 */
    if (end == NULL)
    {
        /* index == size，尾插 */
        list1->tail->next = head;
        head->prev        = list1->tail;
        list1->tail       = tail;
    }
    else if (base == NULL)
    {
        /* index == 0，头插 */
        list1->head->prev = tail;
        tail->next        = list1->head;
        list1->head       = head;
    }
    else
    {
        /* 0 < index < size，中间插入 */
        base->next = head;
        tail->next = end;
        end->prev  = tail;
        head->prev = base;
    }

    list1->size += list2->size;

    return DS_OK;
}

/**
 * @brief 将 list2 的元素添加到空链表中
 *
 * 将 list2 的节点（浅拷贝）添加到空链表 list1 中。
 *
 * @param[in,out] list1 目标链表（必须为空）
 * @param[in]     list2 源链表
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_add_to_empty(ds_List *list1, ds_List *list2)
{
    if (list2->size == 0)
    {
        return DS_OK;
    }

    Node *head = NULL;
    Node *tail = NULL;

    if (link_all_externally(list2, &head, &tail) == false)
    {
        return DS_ERR_ALLOC;
    }

    list1->head = head;
    list1->tail = tail;
    list1->size = list2->size;
    return DS_OK;
}

/**
 * @brief 将 list2 的元素添加到 list1 末尾
 *
 * 将 list2 中的所有元素（浅拷贝）添加到 list1 的末尾。
 *
 * @param[in,out] list1 目标链表
 * @param[in]     list2 源链表
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_add_list(ds_List *list1, ds_List *list2)
{
    if (list1->size == 0)
    {
        return ds_list_add_to_empty(list1, list2);
    }

    return ds_list_add_list_at(list1, list2, list1->size);
}

/**
 * @brief 销毁链表（不释放元素数据）
 *
 * 释放链表中所有节点和链表结构体本身的内存。
 * 注意：此函数不会释放节点中存储的元素数据。
 *
 * @param[in] list 要销毁的链表指针
 */
void ds_list_destroy(ds_List *list)
{
    if (list->size > 0)
    {
        ds_list_remove_all(list);
    }

    list->mem_free(list);
}

/**
 * @brief 销毁链表并释放所有元素数据
 *
 * 先使用回调函数 cb 释放链表中的所有元素，
 * 然后再销毁链表结构体。
 *
 * @param[in] list 要销毁的链表指针
 * @param[in] cb   释放单个元素数据的回调函数指针
 *
 * @note 如果链表中的元素存储在栈上，不应调用此函数，
 *       应使用 ds_list_destroy() 代替
 */
void ds_list_destroy_cb(ds_List *list, void (*cb)(void *))
{
    if (list->size > 0)
    {
        ds_list_remove_all_cb(list, cb);
    }

    list->mem_free(list);
}

/**
 * @brief 将 list2 的节点拼接到 list1 末尾
 *
 * 将 list2 的节点直接拼接到 list1 末尾（移动节点，而非复制）。
 * 拼接后 list2 变为空链表。
 *
 * @param[in,out] list1 目标链表
 * @param[in,out] list2 源链表（拼接后变为空）
 *
 * @return DS_OK 拼接成功，DS_ERR_OUT_OF_RANGE list1 为空但 list2 非空时可能出错
 */
ds_Stat ds_list_splice(ds_List *list1, ds_List *list2)
{
    return ds_list_splice_at(list1, list2, list1->size);
}

/**
 * @brief 在指定位置拼接另一个链表
 *
 * 将 list2 的所有节点直接拼接到 list1 的指定索引位置（移动节点，而非复制）。
 * 拼接后 list2 变为空链表。
 *
 * @param[in,out] list1 目标链表
 * @param[in,out] list2 源链表（拼接后变为空）
 * @param[in]     index 拼接位置
 *
 * @return DS_OK 拼接成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_Stat ds_list_splice_at(ds_List *list1, ds_List *list2, size_t index)
{
    if (list2->size == 0)
    {
        return DS_OK;
    }

    if (index > list1->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    /* 如果 list1 为空，直接接管 list2 */
    if (list1->size == 0)
    {
        list1->head = list2->head;
        list1->tail = list2->tail;
        list1->size = list2->size;

        list2->head = NULL;
        list2->tail = NULL;
        list2->size = 0;

        return DS_OK;
    }

    Node *end  = NULL;
    Node *base = NULL;

    get_node_at(list1, index, &end);

    if (end != NULL)
    {
        base = end->prev;
    }
    else
    {
        get_node_at(list1, index - 1, &base);
    }

    splice_between(list1, list2, base, end);

    return DS_OK;
}

/**
 * @brief 在两个节点之间拼接链表
 *
 * 将 list2 的所有节点插入到 left 和 right 节点之间。
 * 拼接后 list2 变为空链表。
 *
 * @param[in,out] list1 目标链表
 * @param[in,out] list2 源链表（拼接后变为空）
 * @param[in]     left  左边界节点（可为 NULL，表示头部之前）
 * @param[in]     right 右边界节点（可为 NULL，表示尾部之后）
 */
static void splice_between(ds_List *list1, ds_List *list2, Node *left, Node *right)
{
    if (right == NULL)
    {
        /* 拼接在尾部之后 */
        list1->tail->next = list2->head;
        list2->head->prev = list1->tail;
        list1->tail       = list2->tail;
    }
    else if (left == NULL)
    {
        /* 拼接在头部之前 */
        list2->tail->next = list1->head;
        list1->head->prev = list2->tail;
        list1->head       = list2->head;
    }
    else
    {
        /* 拼接在中间 */
        left->next        = list2->head;
        list2->tail->next = right;
        right->prev       = list2->tail;
        list2->head->prev = left;
    }

    list1->size += list2->size;

    list2->head = NULL;
    list2->tail = NULL;
    list2->size = 0;
}

/**
 * @brief 从链表中移除指定元素（按指针值匹配）
 *
 * 查找并移除第一个匹配指定指针值的元素。
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 要移除的元素指针
 * @param[out]    out     输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_VALUE_NOT_FOUND 未找到该元素
 */
ds_Stat ds_list_remove(ds_List *list, void *element, void **out)
{
    Node *node = get_node(list, element);

    if (node == NULL)
    {
        return DS_ERR_VALUE_NOT_FOUND;
    }

    if (out != NULL)
    {
        *out = node->data;
    }

    unlinkn(list, node);

    return DS_OK;
}

/**
 * @brief 在链表中查找指定元素对应的节点
 *
 * 从头节点开始遍历，查找数据指针匹配 element 的节点。
 *
 * @param[in] list    目标链表
 * @param[in] element 要查找的元素指针
 *
 * @return 找到的节点指针，未找到则返回 NULL
 */
static Node *get_node(ds_List *list, void *element)
{
    Node *node = list->head;

    while (node != NULL)
    {
        if (node->data == element)
        {
            return node;
        }
        node = node->next;
    }

    return NULL;
}

/**
 * @brief 从链表中移除指定节点
 *
 * 将指定节点从链表中移除，释放节点内存，并返回节点存储的数据指针。
 *
 * @param[in,out] list 目标链表
 * @param[in]     node 要移除的节点
 *
 * @return 被移除节点存储的数据指针
 */
static void *unlinkn(ds_List *list, Node *node)
{
    void *data = node->data;

    /* 处理头节点 */
    if (node->prev == NULL)
    {
        list->head = node->next;
    }

    if (node->prev != NULL)
    {
        node->prev->next = node->next;
    }

    /* 处理尾节点 */
    if (node->next == NULL)
    {
        list->tail = node->prev;
    }

    if (node->next != NULL)
    {
        node->next->prev = node->prev;
    }

    list->mem_free(node);
    list->size--;
    return data;
}

/**
 * @brief 移除指定索引位置的元素
 *
 * @param[in,out] list 目标链表
 * @param[in]     index 要移除的元素逻辑索引
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_Stat ds_list_remove_at(ds_List *list, size_t index, void **out)
{
    Node   *node;
    ds_Stat status = get_node_at(list, index, &node);

    if (status != DS_OK)
    {
        return status;
    }

    if (out != NULL)
    {
        *out = node->data;
    }

    unlinkn(list, node);
    return DS_OK;
}

/**
 * @brief 快速移除链表的头节点
 *
 * 移除并返回链表头节点的数据。
 *
 * @param[in,out] list 目标链表
 * @param[out]    out  输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_remove_fast(ds_List *list, void **out)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = unlinkn(list, list->head);

    if (out != NULL)
    {
        *out = data;
    }

    return DS_OK;
}

/**
 * @brief 移除链表的尾节点
 *
 * 移除并返回链表尾节点的数据。
 *
 * @param[in,out] list 目标链表
 * @param[out]    out  输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_remove_last(ds_List *list, void **out)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = unlinkn(list, list->tail);

    if (out != NULL)
    {
        *out = data;
    }

    return DS_OK;
}

/**
 * @brief 移除链表中的所有节点
 *
 * 遍历链表，移除所有节点并释放节点内存。
 * 可选地，对每个节点的数据调用回调函数 cb。
 *
 * @param[in,out] list 目标链表
 * @param[in]     cb   释放元素数据的回调函数（可为 NULL）
 *
 * @return true 移除了至少一个节点，false 链表为空
 */
static bool unlinkn_all(ds_List *list, void (*cb)(void *))
{
    if (list->size == 0)
    {
        return false;
    }

    Node *node = list->head;

    while (node != NULL)
    {
        Node *tmp = node->next;

        if (cb != NULL)
        {
            cb(node->data);
        }

        unlinkn(list, node);
        node = tmp;
    }

    return true;
}

/**
 * @brief 移除链表中的所有元素（不释放数据）
 *
 * 移除所有节点，释放节点内存，但不释放元素数据。
 * 链表变为空链表。
 *
 * @param[in,out] list 目标链表
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_remove_all(ds_List *list)
{
    bool unlinked = unlinkn_all(list, NULL);

    if (unlinked == true)
    {
        list->head = NULL;
        list->tail = NULL;
        return DS_OK;
    }

    return DS_ERR_OUT_OF_RANGE;
}

/**
 * @brief 移除链表中的所有元素并释放数据
 *
 * 移除所有节点，对每个元素调用回调函数 cp 释放数据，
 * 然后释放节点内存。
 *
 * @param[in,out] list 目标链表
 * @param[in]     cp   释放元素数据的回调函数
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_remove_all_cb(ds_List *list, void (*cp)(void *))
{
    bool unlinked = unlinkn_all(list, cp);

    if (unlinked == true)
    {
        list->head = NULL;
        list->tail = NULL;
        return DS_OK;
    }

    return DS_ERR_OUT_OF_RANGE;
}

/**
 * @brief 替换指定索引位置的元素
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 新元素指针
 * @param[in]     index   要替换的元素逻辑索引
 * @param[out]    out     输出参数，存储被替换的旧元素指针（可为 NULL）
 *
 * @return DS_OK 替换成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_Stat ds_list_replace_at(ds_List *list, void *element, size_t index, void **out)
{
    Node   *node   = NULL;
    ds_Stat status = get_node_at(list, index, &node);

    if (status != DS_OK)
    {
        return status;
    }

    if (out != NULL)
    {
        *out = node->data;
    }

    node->data = element;
    return DS_OK;
}

/**
 * @brief 获取链表的头元素
 *
 * @param[in]  list 目标链表
 * @param[out] out  输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_get_first(ds_List *list, void **out)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = list->head->data;

    if (out != NULL)
    {
        *out = data;
    }

    return DS_OK;
}

/**
 * @brief 获取链表的尾元素
 *
 * @param[in]  list 目标链表
 * @param[out] out  输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_get_last(ds_List *list, void **out)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = list->tail->data;

    if (out != NULL)
    {
        *out = data;
    }

    return DS_OK;
}

/**
 * @brief 获取指定索引位置的元素
 *
 * @param[in]  list  目标链表
 * @param[in]  index 元素逻辑索引
 * @param[out] out   输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_Stat ds_list_get_at(ds_List *list, size_t index, void **out)
{
    Node   *node   = NULL;
    ds_Stat status = get_node_at(list, index, &node);

    if (status == DS_OK)
    {
        *out = node->data;
    }

    return status;
}

/**
 * @brief 交换两个相邻节点
 *
 * 交换链表中两个相邻节点的位置。
 *
 * @param[in,out] n1 第一个节点
 * @param[in,out] n2 第二个节点（必须与 n1 相邻）
 */
static void swap_adjacent(Node *n1, Node *n2)
{
    /* 先交换外节点，再链接内节点 */
    if (n1->next == n2)
    {
        /* 情况1: n1 在 n2 前面 */
        /* ... A -> n1 -> n2 -> B ... */
        /* 交换后: ... A -> n2 -> n1 -> B ... */
        if (n2->next)
        {
            n2->next->prev = n1; /* B->prev = n1 */
        }

        n1->next = n2->next; /* n1->next = B */

        if (n1->prev)
        {
            n1->prev->next = n2; /* A->next = n2 */
        }

        n2->prev = n1->prev; /* n2->prev = A */

        n2->next = n1; /* n2->next = n1 */
        n1->prev = n2; /* n1->prev = n2 */
    }
    else
    {
        /* 情况2: n2 在 n1 前面 */
        /* ... A -> n2 -> n1 -> B ... */
        /* 交换后: ... A -> n1 -> n2 -> B ... */
        if (n1->next)
        {
            n1->next->prev = n2; /* B->prev = n2 */
        }

        n2->next = n1->next; /* n2->next = B */

        if (n2->prev)
        {
            n2->prev->next = n1; /* A->next = n1 */
        }

        n1->prev = n2->prev; /* n1->prev = A */

        n1->next = n2; /* n1->next = n2 */
        n2->prev = n1; /* n2->prev = n1 */
    }
}

/**
 * @brief 交换链表中两个节点的位置
 *
 * 交换链表中任意两个节点的位置。如果两个节点相邻，调用 swap_adjacent() 处理。
 *
 * @param[in,out] n1 第一个节点
 * @param[in,out] n2 第二个节点
 */
static void swap(Node *n1, Node *n2)
{
    if (n1->next == n2 || n2->next == n1)
    {
        swap_adjacent(n1, n2);
        return;
    }

    /* 保存 n1 和 n2 的邻居节点 */
    Node *n1_left  = n1->prev;
    Node *n1_right = n1->next;
    Node *n2_left  = n2->prev;
    Node *n2_right = n2->next;

    /* 交换 n1 和 n2 在链表中的位置 */
    if (n1_left != NULL)
    {
        n1_left->next = n2;
    }

    n2->prev = n1_left;
    n2->next = n1_right;

    if (n1_right != NULL)
    {
        n1_right->prev = n2;
    }

    if (n2_left != NULL)
    {
        n2_left->next = n1;
    }

    n1->prev = n2_left;
    n1->next = n2_right;

    if (n2_right != NULL)
    {
        n2_right->prev = n1;
    }
}

/**
 * @brief 反转链表中元素的顺序
 *
 * 将链表中的元素顺序完全反转，头变为尾，尾变为头。
 *
 * @param[in,out] list 目标链表
 */
void ds_list_reverse(ds_List *list)
{
    if (list->size < 2)
    {
        return;
    }

    Node *old_head = list->head;
    Node *old_tail = list->tail;

    Node *left_ptr  = old_head;
    Node *right_ptr = old_tail;

    for (size_t i = 0; i < list->size / 2; i++)
    {
        Node *tmp_lp = left_ptr->next;
        Node *tmp_rp = right_ptr->prev;

        swap(left_ptr, right_ptr);

        left_ptr  = tmp_lp;
        right_ptr = tmp_rp;
    }

    list->head = old_tail;
    list->tail = old_head;
}

/**
 * @brief 创建链表的子链表
 *
 * 从原链表中截取 [base, end] 范围内的元素，创建一个新的链表。
 *
 * @param[in]  list 原链表
 * @param[in]  base 起始索引（包含）
 * @param[in]  end  结束索引（包含）
 * @param[out] out  输出参数，存储新创建的子链表指针
 *
 * @return DS_OK 创建成功，DS_ERR_OUT_OF_RANGE 索引范围无效，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_sublist(ds_List *list, size_t base, size_t end, ds_List **out)
{
    if (base > end || end >= list->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_List      *sublist;
    ds_ListConfig conf;

    conf.mem_alloc  = list->mem_alloc;
    conf.mem_calloc = list->mem_calloc;
    conf.mem_free   = list->mem_free;

    ds_Stat status = ds_list_new_conf(&conf, &sublist);

    if (status != DS_OK)
    {
        return status;
    }

    Node *node = NULL;
    status     = get_node_at(list, base, &node);

    if (status != DS_OK)
    {
        ds_list_destroy(sublist);
        return status;
    }

    for (size_t i = base; i <= end; i++)
    {
        status = ds_list_add(sublist, node->data);

        if (status != DS_OK)
        {
            ds_list_destroy(sublist);
            return status;
        }

        node = node->next;
    }

    *out = sublist;
    return DS_OK;
}

/**
 * @brief 创建链表的浅拷贝
 *
 * 创建一个新的链表结构体，复制原链表中的所有元素指针。
 * 新链表与原链表共享相同的元素数据。
 *
 * @param[in]  list 原链表
 * @param[out] out  输出参数，存储新创建的链表指针
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 *
 * @note 浅拷贝只复制指针，不复制指针指向的数据
 */
ds_Stat ds_list_copy_shallow(ds_List *list, ds_List **out)
{
    ds_List      *copy = NULL;
    ds_ListConfig conf;

    conf.mem_alloc  = list->mem_alloc;
    conf.mem_calloc = list->mem_calloc;
    conf.mem_free   = list->mem_free;

    ds_Stat status = ds_list_new_conf(&conf, &copy);

    if (status != DS_OK)
    {
        return status;
    }

    Node *node = list->head;

    while (node != NULL)
    {
        status = ds_list_add(copy, node->data);

        if (status != DS_OK)
        {
            ds_list_destroy(copy);
            return status;
        }

        node = node->next;
    }

    *out = copy;
    return DS_OK;
}

/**
 * @brief 创建链表的深拷贝
 *
 * 创建一个新的链表结构体，并对原链表中的每个元素调用拷贝函数 cp，
 * 生成新的数据副本存储到新链表中。
 *
 * @param[in]  list 原链表
 * @param[in]  cp   元素拷贝函数，接收原元素指针，返回新元素指针
 * @param[out] out  输出参数，存储新创建的链表指针
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_copy_deep(ds_List *list, void *(*cp)(void *), ds_List **out)
{
    ds_List      *copy = NULL;
    ds_ListConfig conf;

    conf.mem_alloc  = list->mem_alloc;
    conf.mem_calloc = list->mem_calloc;
    conf.mem_free   = list->mem_free;

    ds_Stat status = ds_list_new_conf(&conf, &copy);

    if (status != DS_OK)
    {
        return status;
    }

    Node *node = list->head;

    while (node != NULL)
    {
        status = ds_list_add(copy, cp(node->data));

        if (status != DS_OK)
        {
            /* 只销毁链表，数据的释放应由 cp 函数负责 */
            ds_list_destroy(copy);
            return status;
        }

        node = node->next;
    }

    *out = copy;
    return DS_OK;
}

/**
 * @brief 将链表转换为数组
 *
 * 将链表中的所有元素指针复制到新分配的数组中。
 *
 * @param[in]  list 目标链表
 * @param[out] out  输出参数，存储新创建的数组指针
 *
 * @return DS_OK 转换成功，DS_ERR_OUT_OF_RANGE 链表为空，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_to_array(ds_List *list, void ***out)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    void **array = list->mem_alloc(list->size * sizeof(void *));

    if (array == NULL)
    {
        return DS_ERR_ALLOC;
    }

    size_t index = 0;
    Node  *node  = list->head;

    while (node != NULL)
    {
        array[index++] = node->data;
        node           = node->next;
    }

    *out = array;
    return DS_OK;
}

/**
 * @brief 统计链表中指定元素（按指针值）的出现次数
 *
 * @param[in] list    目标链表
 * @param[in] element 要统计的元素指针
 *
 * @return 元素在链表中出现的次数
 */
size_t ds_list_contains(ds_List *list, void *element)
{
    size_t count = 0;
    Node  *node  = list->head;

    while (node != NULL)
    {
        if (node->data == element)
        {
            count++;
        }
        node = node->next;
    }

    return count;
}

/**
 * @brief 统计链表中与指定值相等的元素出现次数（使用比较函数）
 *
 * 使用提供的比较函数判断元素是否相等，比较函数返回0表示相等。
 *
 * @param[in] list    目标链表
 * @param[in] element 要比较的元素指针
 * @param[in] comper  比较函数，返回0表示两个元素相等
 *
 * @return 满足相等条件的元素个数
 */
size_t ds_list_contains_value(ds_List *list, void *element, int (*comper)(void *, void *))
{
    size_t count = 0;
    Node  *node  = list->head;

    while (node != NULL)
    {
        if (comper(element, node->data) == 0)
        {
            count++;
        }
        node = node->next;
    }

    return count;
}

/**
 * @brief 查找指定元素在链表中的逻辑索引（使用比较函数）
 *
 * 从头节点开始遍历，使用比较函数查找第一个匹配的元素。
 *
 * @param[in]  list    目标链表
 * @param[in]  element 要查找的元素指针
 * @param[in]  comper  比较函数，返回0表示两个元素相等
 * @param[out] index   输出参数，存储找到的逻辑索引
 *
 * @return DS_OK 查找成功，DS_ERR_OUT_OF_RANGE 未找到该元素
 */
ds_Stat ds_list_index_of(ds_List *list, void *element, int (*comper)(const void *, const void *), size_t *index)
{
    size_t i    = 0;
    Node  *node = list->head;

    while (node != NULL)
    {
        if (comper(element, node->data) == 0)
        {
            *index = i;
            return DS_OK;
        }

        i++;
        node = node->next;
    }

    return DS_ERR_OUT_OF_RANGE;
}

/**
 * @brief 获取链表的元素个数
 *
 * @param[in] list 目标链表
 *
 * @return 链表中元素的个数
 */
size_t ds_list_size(ds_List *list)
{
    return list->size;
}

/**
 * @brief 对链表进行排序（生成数组后排序）
 *
 * 将链表转换为数组，使用 qsort 排序，然后将排序后的数据写回链表。
 * 此方法会破坏链表的节点结构，但保持节点顺序。
 *
 * @param[in,out] list   目标链表
 * @param[in]     comper 比较函数
 *
 * @return DS_OK 排序成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_list_sort(ds_List *list, int (*comper)(void const *, void const *))
{
    void  **array  = NULL;
    ds_Stat status = ds_list_to_array(list, &array);

    if (status != DS_OK)
    {
        return status;
    }

    qsort(array, list->size, sizeof(void *), comper);

    Node *node = list->head;

    for (size_t i = 0; i < list->size; i++)
    {
        node->data = array[i];
        node       = node->next;
    }

    list->mem_free(array);
    return DS_OK;
}

/**
 * @brief 归并两个已排序的子链表
 *
 * 将两个已排序的子链表合并为一个有序链表。
 *
 * @param[in,out] left    左子链表的头节点指针（会被更新）
 * @param[in,out] right   右子链表的头节点指针（会被更新为合并后的尾节点）
 * @param[in]     l_size  左子链表的大小
 * @param[in]     r_size  右子链表的大小
 * @param[in]     comper  比较函数
 */
static void split_merge(Node **left, Node **right, size_t l_size, size_t r_size, int (*comper)(void *const, void *const))
{
    size_t size = l_size + r_size;
    size_t l    = 0;
    size_t r    = 0;

    Node *l_ptr = *left;
    Node *r_ptr = *right;

    for (size_t i = 0; i < size; i++)
    {
        int c = comper(l_ptr->data, r_ptr->data);

        if (c <= 0)
        {
            if (i == 0 && size == 2)
            {
                break;
            }

            l++;

            if (l == l_size)
            {
                /* 左子链表已全部归并 */
                for (; r < r_size - 1; r++)
                {
                    r_ptr = r_ptr->next;
                }
                *right = r_ptr;
                break;
            }

            l_ptr = l_ptr->next;
        }
        else
        {
            Node *tmp = r_ptr->next;

            link_behind(l_ptr, r_ptr);

            if (i == 0 && size == 2)
            {
                break;
            }

            r++;

            if (r == r_size)
            {
                /* 右子链表已全部归并 */
                for (; l < l_size - 1; l++)
                {
                    l_ptr = l_ptr->next;
                }
                *right = l_ptr;
                break;
            }

            if (i == 0)
            {
                *left = r_ptr;
            }

            r_ptr = tmp;
        }
    }
}

/**
 * @brief 自底向上的归并排序
 *
 * 使用归并排序对链表进行排序，不额外分配数组。
 *
 * @param[in,out] list   目标链表
 * @param[in]     base   当前子链表的头节点
 * @param[in]     size   当前子链表的大小
 * @param[in]     comper 比较函数
 *
 * @return 排序后子链表的头节点
 */
static Node *split(ds_List *list, Node *base, size_t size, int (*comper)(void *const, void *const))
{
    if (size < 2)
    {
        return base;
    }

    size_t l_size = size / 2;
    size_t r_size = size / 2 + size % 2;

    Node *center = base;

    for (size_t i = 0; i < l_size; i++)
    {
        center = center->next;
    }

    Node *l_head = split(list, base, l_size, comper);
    Node *r_head = split(list, center, r_size, comper);

    split_merge(&l_head, &r_head, l_size, r_size, comper);

    list->head = l_head;
    list->tail = r_head;

    return l_head;
}

/**
 * @brief 原地归并排序链表
 *
 * 使用归并排序对链表进行排序，不额外分配数组，时间复杂度 O(n log n)。
 *
 * @param[in,out] list   目标链表
 * @param[in]     comper 比较函数
 */
void ds_list_sort_in_place(ds_List *list, int (*comper)(void *const, void *const))
{
    split(list, list->head, list->size, comper);
}

/**
 * @brief 对链表中的每个元素执行操作
 *
 * 遍历链表，对每个元素调用 op 函数。
 *
 * @param[in,out] list 目标链表
 * @param[in]     op   对每个元素执行的函数
 */
void ds_list_foreach(ds_List *list, void (*op)(void *))
{
    Node *node = list->head;

    while (node != NULL)
    {
        op(node->data);
        node = node->next;
    }
}

/**
 * @brief 对链表进行归约操作
 *
 * 将链表中的所有元素通过二元函数聚合成一个结果。
 *
 * @param[in]     list   目标链表
 * @param[in]     fn     二元归约函数
 * @param[in,out] result 存储归约结果的指针
 *
 * @return DS_OK 归约成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_reduce(ds_List *list, void (*fn)(void *, void *, void *), void *result)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (list->size == 1)
    {
        fn(list->head->data, NULL, result);
        return DS_OK;
    }

    fn(list->head->data, list->head->next->data, result);

    Node *curr = list->head->next->next;
    Node *next = NULL;

    while (curr)
    {
        next = curr->next;
        fn(result, curr->data, result);
        curr = next;
    }

    return DS_OK;
}

/**
 * @brief 就地过滤链表（修改原链表）
 *
 * 移除链表中所有不满足谓词函数 pred 的元素。
 *
 * @param[in,out] list 目标链表
 * @param[in]     pred 谓词函数，返回 true 表示保留元素，false 表示移除
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_list_fiter_mut(ds_List *list, bool (*pred)(void *))
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    Node *curr = list->head;
    Node *next = NULL;

    while (curr)
    {
        next = curr->next;

        if (pred(curr->data) == false)
        {
            unlinkn(list, curr);
        }

        curr = next;
    }

    return DS_OK;
}

/**
 * @brief 过滤链表并生成新链表（不修改原链表）
 *
 * 遍历原链表，将所有满足谓词函数 pred 的元素添加到新链表中。
 *
 * @param[in]  list  原链表
 * @param[in]  pred  谓词函数，返回 true 表示保留该元素
 * @param[out] out   输出参数，存储新创建的过滤后链表指针
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 链表为空，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @note 函数参数类型 int(*pred)(void*) 与 bool(*pred)(void*) 不一致，建议统一
 */
ds_Stat ds_list_filter(ds_List *list, int (*pred)(void *), ds_List **out)
{
    if (list->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_List      *filtered = NULL;
    ds_ListConfig conf;

    conf.mem_alloc  = list->mem_alloc;
    conf.mem_calloc = list->mem_calloc;
    conf.mem_free   = list->mem_free;

    if (ds_list_new_conf(&conf, &filtered) != DS_OK)
    {
        return DS_ERR_ALLOC;
    }

    Node *curr = list->head;
    Node *next = NULL;

    while (curr)
    {
        next = curr->next;

        if (pred(curr->data) == true)
        {
            if (ds_list_add(filtered, curr->data) != DS_OK)
            {
                ds_list_destroy(filtered);
                return DS_ERR_ALLOC;
            }
        }

        curr = next;
    }

    *out = filtered;
    return DS_OK;
}
