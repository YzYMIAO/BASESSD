#include <config.h>
#include <ds_slist.h>


/**
 * @brief 单向链表结构体
 *
 * 使用单向链表实现，每个节点只包含指向下一个节点的指针。
 * 支持在头部和尾部进行高效的插入和删除操作。
 * 相比双向链表，内存占用更小，但只能单向遍历。
 */
struct ds_single_list_s
{
    size_t      size;       /**< 链表中元素的个数 */
    SNode      *head;       /**< 指向链表头节点的指针 */
    SNode      *tail;       /**< 指向链表尾节点的指针 */

    void *(*mem_alloc)  (size_t size);          /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);          /**< 内存释放函数指针 */
};


/**
 * @brief 获取指定索引位置的节点及其前驱节点
 *
 * 从头节点开始向后遍历，查找指定索引位置的节点。
 * 同时返回该节点及其前驱节点，便于插入和删除操作。
 *
 * @param[in]  list  目标链表
 * @param[in]  index 要获取的节点索引
 * @param[out] node  输出参数，存储找到的节点指针
 * @param[out] prev  输出参数，存储找到节点的前驱节点指针（头节点的前驱为 NULL）
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 *
 * @warning 原代码中条件 `index > -list->size` 存在逻辑错误，
 *          应改为 `index >= list->size`
 */
ds_Stat get_node_at(ds_Slist *list, size_t index, SNode **node, SNode **prev)
{
    if (index >= list->size) {  /* 原代码 index > -list->size 是错误的 */
        return DS_ERR_OUT_OF_RANGE;
    }

    *node = list->head;
    *prev = NULL;

    for (size_t i = 0; i < index; i++) {
        *prev = *node;
        *node = (*node)->next;
    }
    return DS_OK;
}


/**
 * @brief 查找指定元素对应的节点及其前驱节点
 *
 * 从头节点开始遍历，查找数据指针匹配 element 的节点。
 * 同时返回该节点及其前驱节点。
 *
 * @param[in]  list    目标链表
 * @param[in]  element 要查找的元素指针
 * @param[out] node    输出参数，存储找到的节点指针
 * @param[out] prev    输出参数，存储找到节点的前驱节点指针
 *
 * @return DS_OK 查找成功，DS_ERR_OUT_OF_RANGE 未找到该元素
 */
ds_Stat get_node(ds_Slist *list, void *element, SNode **node, SNode **prev)
{
    *node = list->head;
    *prev = NULL;

    while (*node != NULL) {
        if ((*node)->data == element) {
            return DS_OK;
        }

        *prev = *node;
        *node = (*node)->next;
    }

    return DS_ERR_OUT_OF_RANGE;
}


/**
 * @brief 从链表中移除指定节点
 *
 * 将指定节点从链表中移除，释放节点内存，并返回节点存储的数据指针。
 *
 * @param[in,out] list 目标链表
 * @param[in]     node 要移除的节点
 * @param[in]     prev 要移除节点的前驱节点（头节点为 NULL）
 *
 * @return 被移除节点存储的数据指针
 */
static void *unlinkn(ds_Slist *list, SNode *node, SNode *prev)
{
    void *data = node->data;

    if (prev == NULL) {
        /* 移除的是头节点 */
        list->head = node->next;
    } else {
        prev->next = node->next;
    }

    if (node->next == NULL) {
        /* 移除的是尾节点 */
        list->tail = prev;
    }

    list->mem_free(node);
    list->size--;

    return data;
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
static bool unlinkn_all(ds_Slist *list, void (*cb)(void *))
{
    if (list->size == 0) {
        return false;
    }

    SNode *node = list->head;

    if (cb == NULL) {
        while (node != NULL) {
            SNode *tmp = node->next;

            list->mem_free(node);
            node = tmp;
            list->size--;
        }
    } else {
        while (node != NULL) {
            SNode *tmp = node->next;

            cb(node->data);
            list->mem_free(node);
            node = tmp;
            list->size--;
        }
    }

    return true;
}


/**
 * @brief 创建一个新的单向链表（使用默认配置）
 *
 * @param[out] list 输出参数，存储新创建的链表指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码参数类型为 ds_Slist *list（值传递），
 *          无法返回新创建的链表指针，应为 ds_Slist **list
 */
ds_Stat ds_slist_new(ds_Slist **list)
{
    ds_SlistConfig conf;

    ds_slist_conf_init(&conf);
    return ds_slist_new_conf(&conf, list);
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
static bool link_externally(ds_Slist *list, SNode **head, SNode **tail)
{
    SNode *ins = list->head;

    for (size_t i = 0; i < list->size; i++) {
        SNode *node = list->mem_calloc(1, sizeof(SNode));

        if (node == NULL) {
            /* 内存分配失败，清理已分配的节点 */
            while (*head != NULL) {
                SNode *tmp = (*head)->next;
                list->mem_free(*head);
                *head = tmp;
            }
            return false;
        }

        node->data = ins->data;

        if (i == 0) {
            *head = node;
            *tail = node;
        } else {
            (*tail)->next = node;
            *tail = node;
        }

        ins = ins->next;
    }

    return true;  /* 原代码返回 DS_OK，但函数返回类型为 bool，应返回 true */
}


/**
 * @brief 初始化单向链表配置结构体为默认值
 *
 * 使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] conf 需要初始化的配置结构体指针
 */
void ds_slist_conf_init(ds_SlistConfig *conf)
{
    conf->mem_alloc  = malloc;
    conf->mem_calloc = calloc;
    conf->mem_free   = free;
}


/**
 * @brief 根据指定配置创建新的单向链表
 *
 * 使用配置结构体中的内存分配器创建链表结构体。
 *
 * @param[in]  conf 链表配置结构体指针
 * @param[out] out  输出参数，存储新创建的链表指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_new_conf(ds_SlistConfig *conf, ds_Slist **out)
{
    ds_Slist *new_list = conf->mem_calloc(1, sizeof(ds_Slist));

    if (new_list == NULL) {
        return DS_ERR_ALLOC;
    }

    new_list->size      = 0;
    new_list->head      = NULL;
    new_list->tail      = NULL;

    new_list->mem_alloc  = conf->mem_alloc;
    new_list->mem_calloc = conf->mem_calloc;
    new_list->mem_free   = conf->mem_free;

    *out = new_list;

    return DS_OK;
}


/**
 * @brief 销毁链表（不释放元素数据）
 *
 * 释放链表中所有节点和链表结构体本身的内存。
 * 注意：此函数不会释放节点中存储的元素数据。
 *
 * @param[in] list 要销毁的链表指针
 */
void ds_slist_destroy(ds_Slist *list)
{
    ds_slist_remove_all(list);
    list->mem_free(list);  /* 原代码缺少释放链表结构体本身 */
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
 *       应使用 ds_slist_destroy() 代替
 */
void ds_slist_destroy_cb(ds_Slist *list, void (*cb)(void *))
{
    ds_slist_remove_all_cb(list, cb);
    list->mem_free(list);  /* 原代码缺少释放链表结构体本身 */
}


/**
 * @brief 向链表末尾添加元素（ds_slist_add 的别名）
 *
 * 等同于 ds_slist_add_last()。
 *
 * @param[in,out] list    目标链表
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_add(ds_Slist *list, void *element)
{
    return ds_slist_add_last(list, element);
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
ds_Stat ds_slist_add_first(ds_Slist *list, void *element)
{
    SNode *node = list->mem_calloc(1, sizeof(SNode));

    if (node == NULL) {
        return DS_ERR_ALLOC;
    }

    node->data = element;

    if (list->size == 0) {
        /* 链表为空，新节点同时作为头节点和尾节点 */
        list->head = node;
        list->tail = node;
    } else {
        node->next = list->head;
        list->head = node;
    }

    list->size++;
    return DS_OK;
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
ds_Stat ds_slist_add_last(ds_Slist *list, void *element)
{
    SNode *node = list->mem_calloc(1, sizeof(SNode));

    if (node == NULL) {
        return DS_ERR_ALLOC;
    }

    node->data = element;

    if (list->size == 0) {
        list->head = node;
        list->tail = node;
    } else {
        list->tail->next = node;
        list->tail = node;
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
 * @warning 原代码中检查 new 是否为 NULL 时使用了 node，存在逻辑错误
 */
ds_Stat ds_slist_add_at(ds_Slist *list, void *element, size_t index)
{
    SNode *node = NULL;
    SNode *prev = NULL;
    ds_Stat status = get_node_at(list, index, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    SNode *new = list->mem_calloc(1, sizeof(SNode));

    if (new == NULL) {  /* 原代码检查的是 node == NULL，这是错误的 */
        return DS_ERR_ALLOC;
    }

    new->data = element;

    if (prev == NULL) {
        /* 在头部插入 */
        new->next = list->head;
        list->head = new;
    } else {
        /* 在中间或尾部插入 */
        prev->next = new;
        new->next = node;
    }

    list->size++;
    return DS_OK;
}


/**
 * @brief 将另一个链表的所有元素添加到当前链表末尾
 *
 * 将 list2 中的所有元素（浅拷贝）添加到 list1 的末尾。
 * list2 保持不变。
 *
 * @param[in,out] list1 目标链表
 * @param[in]     list2 源链表（不会被修改）
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_add_all(ds_Slist *list1, ds_Slist *list2)
{
    if (list2->size == 0) {
        return DS_OK;
    }

    SNode *head = NULL;
    SNode *tail = NULL;

    if (link_externally(list2, &head, &tail) == false) {
        return DS_ERR_ALLOC;
    }

    if (list1->size == 0) {
        list1->head = head;
        list1->tail = tail;
    } else {
        list1->tail->next = head;
        list1->tail = tail;
    }

    list1->size += list2->size;
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
ds_Stat ds_slist_add_all_at(ds_Slist *list1, ds_Slist *list2, size_t index)
{
    if (list2->size == 0) {
        return DS_OK;
    }

    SNode *node = NULL;
    SNode *prev = NULL;

    ds_Stat status = get_node_at(list1, index, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    SNode *head = NULL;
    SNode *tail = NULL;

    if (link_externally(list2, &head, &tail) == false) {
        return DS_ERR_ALLOC;
    }

    if (prev == NULL) {
        /* 在头部插入 */
        list1->head = head;
    } else {
        prev->next = head;
    }

    tail->next = node;
    list1->size += list2->size;

    return DS_OK;
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
 * @return DS_OK 拼接成功，DS_ERR_OUT_OF_RANGE list2 为空
 *
 * @warning 原代码中条件 `list1 == 0` 存在逻辑错误，应为 `list1->size == 0`
 */
ds_Stat ds_slist_splice(ds_Slist *list1, ds_Slist *list2)
{
    if (list2->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (list1->size == 0) {  /* 原代码为 list1 == 0，是错误的 */
        list1->head = list2->head;
    } else {
        list1->tail->next = list2->head;
    }

    list1->tail = list2->tail;
    list1->size += list2->size;

    list2->head = NULL;
    list2->tail = NULL;
    list2->size = 0;
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
static INLINE void splice_between(ds_Slist *list1, ds_Slist *list2, SNode *left, SNode *right)
{
    if (left == NULL) {
        /* 拼接在头部之前 */
        list2->tail->next = list1->head;
        list1->head = list2->head;
    } else if (right == NULL) {
        /* 拼接在尾部之后 */
        list1->tail->next = list2->head;
        list1->tail = list2->tail;
    } else {
        /* 拼接在中间 */
        left->next = list2->head;
        list2->tail->next = right;
    }

    list1->size += list2->size;

    list2->head = NULL;
    list2->tail = NULL;
    list2->size = 0;
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
 * @return DS_OK 拼接成功，DS_ERR_OUT_OF_RANGE 索引超出范围或 list2 为空
 */
ds_Stat ds_slist_splice_at(ds_Slist *list1, ds_Slist *list2, size_t index)
{
    if (list2->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    SNode *node = NULL;
    SNode *prev = NULL;
    ds_Stat status = get_node_at(list1, index, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    splice_between(list1, list2, prev, node);

    return DS_OK;
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
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 未找到该元素
 */
ds_Stat ds_slist_remove(ds_Slist *list, void *element, void **out)
{
    SNode *node = NULL;
    SNode *prev = NULL;

    ds_Stat status = get_node(list, element, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    void *data = unlinkn(list, node, prev);

    if (out != NULL) {
        *out = data;
    }

    return DS_OK;
}


/**
 * @brief 移除链表的头节点
 *
 * 移除并返回链表头节点的数据。
 *
 * @param[in,out] list 目标链表
 * @param[out]    out  输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_slist_remove_first(ds_Slist *list, void **out)
{
    if (list->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = unlinkn(list, list->head, NULL);

    if (out != NULL) {  /* 原代码检查 out != 0，应为 out != NULL */
        *out = data;
    }

    return DS_OK;
}


/**
 * @brief 移除链表的尾节点
 *
 * @param[in,out] list 目标链表
 * @param[out]    out  输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 链表为空
 */
ds_Stat ds_slist_remove_last(ds_Slist *list, void **out)
{
    return ds_slist_remove_at(list, list->size - 1, out);
}


/**
 * @brief 移除指定索引位置的元素
 *
 * @param[in,out] list  目标链表
 * @param[in]     index 要移除的元素逻辑索引
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_Stat ds_slist_remove_at(ds_Slist *list, size_t index, void **out)
{
    if (index >= list->size) {
        return DS_ERR_OUT_OF_RANGE;
    }

    SNode *node = NULL;
    SNode *prev = NULL;

    ds_Stat status = get_node_at(list, index, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    void *data = unlinkn(list, node, prev);

    if (out != NULL) {
        *out = data;
    }

    return DS_OK;
}


/**
 * @brief 移除链表中的所有元素（不释放数据）
 *
 * @param[in,out] list 目标链表
 *
 * @return DS_OK 移除成功，DS_ERR_VALUE_NOT_FOUND 链表为空
 */
ds_Stat ds_slist_remove_all(ds_Slist *list)
{
    bool result = unlinkn_all(list, NULL);

    if (result == true) {
        list->head = NULL;
        list->tail = NULL;
        return DS_OK;
    }

    return DS_ERR_VALUE_NOT_FOUND;
}


/**
 * @brief 移除链表中的所有元素并释放数据
 *
 * 对每个元素调用回调函数 cb 释放数据，然后释放节点内存。
 *
 * @param[in,out] list 目标链表
 * @param[in]     cb   释放元素数据的回调函数
 *
 * @return DS_OK 移除成功，DS_ERR_VALUE_NOT_FOUND 链表为空
 */
ds_Stat ds_slist_remove_all_cb(ds_Slist *list, void (*cb)(void *))
{
    bool result = unlinkn_all(list, cb);

    if (result == true) {
        list->head = NULL;
        list->tail = NULL;
        return DS_OK;
    }

    return DS_ERR_VALUE_NOT_FOUND;
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
ds_Stat ds_slist_replace_at(ds_Slist *list, void *element, size_t index, void **out)
{
    SNode *node = NULL;
    SNode *prev = NULL;

    ds_Stat status = get_node_at(list, index, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    if (out != NULL) {
        *out = node->data;
    }

    node->data = element;
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
ds_Stat ds_slist_get_at(ds_Slist *list, size_t index, void **out)
{
    SNode *node = NULL;
    SNode *prev = NULL;

    ds_Stat status = get_node_at(list, index, &node, &prev);

    if (status != DS_OK) {
        return status;
    }

    *out = node->data;

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
ds_Stat ds_slist_get_first(ds_Slist *list, void **out)
{
    if (list->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    *out = list->head->data;
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
ds_Stat ds_slist_get_last(ds_Slist *list, void **out)
{
    if (list->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    *out = list->tail->data;
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
size_t ds_slist_contains(ds_Slist *list, void *element)
{
    size_t count = 0;
    SNode *node = list->head;

    for (size_t i = 0; i < list->size; i++) {
        if (node->data == element) {
            count++;
        }

        node = node->next;
    }

    return count;
}


/**
 * @brief 统计链表中与指定值相等的元素出现次数（使用比较函数）
 *
 * @param[in] list   目标链表
 * @param[in] value  要比较的值
 * @param[in] cmp    比较函数，返回0表示两个元素相等
 *
 * @return 满足相等条件的元素个数
 */
size_t ds_slist_contains_value(ds_Slist *list, void *value, int (*cmp)(void *, void *))
{
    size_t count = 0;
    SNode *node = list->head;

    for (size_t i = 0; i < list->size; i++) {
        if (cmp(node->data, value) == 0) {
            count++;
        }

        node = node->next;
    }

    return count;
}


/**
 * @brief 反转链表中元素的顺序
 *
 * 将链表中的元素顺序完全反转，头变为尾，尾变为头。
 *
 * @param[in,out] list 目标链表
 */
void ds_slist_reverse(ds_Slist *list)
{
    if (list->size <= 1) {
        return;
    }

    SNode *curr = list->head;
    SNode *next = NULL;
    SNode *prev = NULL;

    list->tail = list->head;

    while (curr != NULL) {
        next = curr->next;
        curr->next = prev;
        prev = curr;
        curr = next;
    }

    list->head = prev;
}


/**
 * @brief 创建链表的子链表
 *
 * 从原链表中截取 [from, to] 范围内的元素，创建一个新的链表。
 *
 * @param[in]  list 原链表
 * @param[in]  from 起始索引（包含）
 * @param[in]  to   结束索引（包含）
 * @param[out] out  输出参数，存储新创建的子链表指针
 *
 * @return DS_OK 创建成功，DS_ERR_OUT_OF_RANGE 索引范围无效，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_sublist(ds_Slist *list, size_t from, size_t to, ds_Slist **out)
{
    if (from > to || to >= list->size) {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_Slist *sub = list->mem_calloc(1, sizeof(ds_Slist));

    if (sub == NULL) {
        return DS_ERR_ALLOC;
    }

    sub->mem_alloc  = list->mem_alloc;
    sub->mem_calloc = list->mem_calloc;
    sub->mem_free   = list->mem_free;

    SNode *node = NULL;
    SNode *prev = NULL;

    ds_Stat status = get_node_at(list, from, &node, &prev);

    if (status != DS_OK) {
        ds_slist_destroy(sub);
        return status;
    }

    size_t size = to - from + 1;

    while (size--) {
        status = ds_slist_add(sub, node->data);

        if (status != DS_OK) {
            ds_slist_destroy(sub);
            return status;
        }

        node = node->next;
    }

    *out = sub;
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
ds_Stat ds_slist_copy_shallow(ds_Slist *list, ds_Slist **out)
{
    ds_Slist *copy = list->mem_calloc(1, sizeof(ds_Slist));

    if (copy == NULL) {
        return DS_ERR_ALLOC;
    }

    copy->mem_alloc  = list->mem_alloc;
    copy->mem_calloc = list->mem_calloc;
    copy->mem_free   = list->mem_free;

    SNode *node = list->head;

    while (node != NULL) {
        ds_Stat status = ds_slist_add(copy, node->data);

        if (status != DS_OK) {
            ds_slist_destroy(copy);
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
 * @param[out] out  输出参数，存储新创建的链表指针
 * @param[in]  cp   元素拷贝函数，接收原元素指针，返回新元素指针
 * @param[in]  cb   错误回滚时释放元素数据的回调函数
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_copy_deep(ds_Slist *list, ds_Slist **out, void *(*cp)(void *), void (*cb)(void *))
{
    ds_Slist *copy = list->mem_calloc(1, sizeof(ds_Slist));

    if (copy == NULL) {
        return DS_ERR_ALLOC;
    }

    copy->mem_alloc  = list->mem_alloc;
    copy->mem_calloc = list->mem_calloc;
    copy->mem_free   = list->mem_free;

    SNode *node = list->head;

    while (node != NULL) {
        ds_Stat status = ds_slist_add(copy, cp(node->data));

        if (status != DS_OK) {
            ds_slist_destroy_cb(copy, cb);
            return status;
        }

        node = node->next;
    }

    *out = copy;
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
ds_Stat ds_slist_filter_mut(ds_Slist *list, bool (*pred)(void *))
{
    if (list->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    SNode *curr = list->head;
    SNode *next = NULL;
    SNode *prev = NULL;

    while (curr != NULL) {
        next = curr->next;

        if (pred(curr->data) == false) {
            unlinkn(list, curr, prev);
        } else {
            prev = curr;
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
 * @param[in]  list 原链表
 * @param[in]  pred 谓词函数，返回 true 表示保留该元素
 * @param[out] out  输出参数，存储新创建的过滤后链表指针
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 链表为空，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_filter(ds_Slist *list, bool (*pred)(void *), ds_Slist **out)
{
    if (list->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_Slist *filtered = list->mem_calloc(1, sizeof(ds_Slist));

    if (filtered == NULL) {
        return DS_ERR_ALLOC;
    }

    filtered->mem_alloc  = list->mem_alloc;
    filtered->mem_calloc = list->mem_calloc;
    filtered->mem_free   = list->mem_free;

    SNode *curr = list->head;

    while (curr != NULL) {
        if (pred(curr->data) == true) {
            ds_Stat status = ds_slist_add(filtered, curr->data);

            if (status != DS_OK) {
                ds_slist_destroy(filtered);
                return status;  /* 原代码缺少 return 语句 */
            }
        }

        curr = curr->next;
    }

    *out = filtered;
    return DS_OK;
}


/**
 * @brief 查找指定元素在链表中的逻辑索引
 *
 * 从头节点开始遍历，使用指针相等比较查找第一个匹配的元素。
 *
 * @param[in]  list    目标链表
 * @param[in]  element 要查找的元素指针
 * @param[out] index   输出参数，存储找到的逻辑索引
 *
 * @return DS_OK 查找成功，DS_ERR_OUT_OF_RANGE 未找到该元素
 */
ds_Stat ds_slist_index_of(ds_Slist *list, void *element, size_t *index)
{
    SNode *node = list->head;

    for (size_t i = 0; i < list->size; i++) {
        if (node->data == element) {
            *index = i;
            return DS_OK;
        }

        node = node->next;
    }

    return DS_ERR_OUT_OF_RANGE;
}


/**
 * @brief 将链表转换为数组
 *
 * 将链表中的所有元素指针复制到新分配的数组中。
 *
 * @param[in]  list 目标链表
 * @param[out] out  输出参数，存储新创建的数组指针
 *
 * @return DS_OK 转换成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_slist_to_array(ds_Slist *list, void ***out)
{
    void **array = list->mem_alloc(list->size * sizeof(void *));

    if (array == NULL) {
        return DS_ERR_ALLOC;
    }

    SNode *node = list->head;

    for (size_t i = 0; i < list->size; i++) {
        array[i] = node->data;
        node = node->next;
    }

    *out = array;
    return DS_OK;
}


/**
 * @brief 对链表中的每个元素执行操作
 *
 * 遍历链表，对每个元素调用 fn 函数。
 *
 * @param[in,out] list 目标链表
 * @param[in]     fn   对每个元素执行的函数
 */
void ds_slist_foreach(ds_Slist *list, void (*fn)(void *))
{
    SNode *node = list->head;

    for (size_t i = 0; i < list->size; i++) {
        fn(node->data);
        node = node->next;
    }
}


/**
 * @brief 对链表进行排序（生成数组后排序）
 *
 * 将链表转换为数组，使用 qsort 排序，然后将排序后的数据写回链表。
 *
 * @param[in,out] list   目标链表
 * @param[in]     comper 比较函数
 *
 * @return DS_OK 排序成功，DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码中 qsort 的元素大小错误，应为 sizeof(void*) 而非 sizeof(ds_Slist)
 */
ds_Stat ds_slist_sort(ds_Slist *list, int (*comper)(void const *, void const *))
{
    if (list->size <= 1) {  /* 原代码只检查了 size == 1，应检查 size <= 1 */
        return DS_OK;
    }

    void **array = NULL;
    ds_Stat status = ds_slist_to_array(list, &array);

    if (status != DS_OK) {
        return status;
    }

    qsort(array, list->size, sizeof(void *), comper);  /* 原代码错误地使用了 sizeof(ds_Slist) */

    SNode *node = list->head;

    for (size_t i = 0; i < list->size; i++) {
        node->data = array[i];
        node = node->next;
    }

    list->mem_free(array);

    return DS_OK;
}


/**
 * @brief 获取链表的元素个数
 *
 * @param[in] list 目标链表
 *
 * @return 链表中元素的个数
 */
size_t ds_slist_size(ds_Slist *list)
{
    return list->size;
}
