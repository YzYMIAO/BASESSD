#include <config.h>
#include <ds_pqueue.h>


#define DEFAULT_CAPACITY           8   /**< 默认初始容量 */
#define DEFAULT_EXPANSION_FACTOR   2   /**< 默认扩容因子 */

/**
 * @brief 获取父节点索引
 *
 * @param[in] x 当前节点索引
 * @return 父节点索引（根节点的父节点为自身）
 */
#define INDEX_PARENT(x)             ((x > 0) ? ((x - 1) / 2) : 0)

/**
 * @brief 获取左子节点索引
 *
 * @param[in] x 当前节点索引
 * @return 左子节点索引
 */
#define INDEX_LEFT_CHILD(x)         (2 * x + 1)

/**
 * @brief 获取右子节点索引
 *
 * @param[in] x 当前节点索引
 * @return 右子节点索引
 */
#define INDEX_RIGHT_CHILD(x)        (2 * x + 2)


/**
 * @brief 优先队列结构体（最大堆）
 *
 * 使用二叉堆（完全二叉树）实现的优先队列。
 * 堆顶元素为最大值（由比较函数定义）。
 * 支持 O(log n) 的插入和删除操作，O(1) 的获取堆顶操作。
 */
struct ds_pqueue_s
{
    size_t      size;       /**< 当前队列中的元素个数 */
    size_t      capacity;   /**< 当前队列容量 */
    float       exp_factor; /**< 扩容因子 */
    void     **buffer;     /**< 指向存储元素指针的数组缓冲区 */

    int   (*cmp)        (void const *a, void const *b); /**< 元素比较函数指针 */
    void *(*mem_alloc)  (size_t size);                  /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size);   /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);                  /**< 内存释放函数指针 */
};


/**
 * @brief 扩容优先队列的容量
 *
 * 按照扩容因子计算新容量并重新分配内存。
 * 如果当前容量已达到 DS_MAX_ELEMENTS，则不再扩容。
 *
 * @param[in,out] pq 需要扩容的优先队列
 *
 * @return DS_OK 扩容成功，DS_ERR_MAX_CAPACITY 已达最大容量，
 *         DS_ERR_ALLOC 内存分配失败
 */
static ds_Stat pqueue_expand_capacity(ds_Pqueue *pq)
{
    if (pq->capacity == DS_MAX_ELEMENTS) {
        return DS_ERR_MAX_CAPACITY;
    }

    size_t new_capacity = (size_t)(pq->capacity * pq->exp_factor);

    /* 检查容量溢出：如果新容量小于原容量，说明发生了整数溢出 */
    if (new_capacity < pq->capacity) {
        pq->capacity = DS_MAX_ELEMENTS;
    } else {
        pq->capacity = new_capacity;
    }

    void **new_buffer = pq->mem_alloc(pq->capacity * sizeof(void *));

    if (new_buffer == NULL) {
        return DS_ERR_ALLOC;
    }

    /* 将旧缓冲区中的数据复制到新缓冲区 */
    memcpy(new_buffer, pq->buffer, pq->size * sizeof(void *));

    pq->mem_free(pq->buffer);
    pq->buffer = new_buffer;
    return DS_OK;
}


/**
 * @brief 创建一个新的优先队列（使用默认配置）
 *
 * 使用默认配置创建优先队列，默认初始容量为 8，扩容因子为 2。
 *
 * @param[out] pq  输出参数，存储新创建的优先队列指针
 * @param[in]  cmp 元素比较函数指针，用于确定优先级顺序
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 *
 * @note 比较函数应返回正值表示第一个元素优先级高于第二个元素
 */
ds_Stat ds_pqueue_new(ds_Pqueue **pq, int (*cmp)(void const *, void const *))
{
    ds_PqueueConfig conf;

    ds_pqueue_conf_init(&conf, cmp);
    return ds_pqueue_new_conf(&conf, pq);
}


/**
 * @brief 初始化优先队列配置结构体为默认值
 *
 * 设置默认容量为 8，默认扩容因子为 2，
 * 使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] conf 需要初始化的配置结构体指针
 * @param[in]     cmp  元素比较函数指针
 */
void ds_pqueue_conf_init(ds_PqueueConfig *conf, int (*cmp)(void const *, void const *))
{
    conf->capacity    = DEFAULT_CAPACITY;
    conf->exp_factor  = DEFAULT_EXPANSION_FACTOR;
    conf->cmp         = cmp;
    conf->mem_alloc   = malloc;
    conf->mem_calloc  = calloc;
    conf->mem_free    = free;
}


/**
 * @brief 根据指定配置创建新的优先队列
 *
 * 使用配置结构体中的参数创建优先队列。
 *
 * @param[in]  conf 优先队列配置结构体指针
 * @param[out] out  输出参数，存储新创建的优先队列指针
 *
 * @return DS_OK 创建成功，DS_ERR_INVALID_CAPACITY 容量配置无效，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_pqueue_new_conf(ds_PqueueConfig *conf, ds_Pqueue **out)
{
    float ex = 0;

    /* 扩容因子必须大于 1 */
    if (conf->exp_factor <= 1) {
        ex = DEFAULT_EXPANSION_FACTOR;
    } else {
        ex = conf->exp_factor;
    }

    /* 检查容量配置是否有效 */
    if (conf->capacity == 0 || ex >= DS_MAX_ELEMENTS / conf->capacity) {
        return DS_ERR_INVALID_CAPACITY;
    }

    ds_Pqueue *pq = conf->mem_calloc(1, sizeof(ds_Pqueue));

    if (pq == NULL) {
        return DS_ERR_ALLOC;
    }

    void **new_buffer = conf->mem_alloc(conf->capacity * sizeof(void *));

    if (new_buffer == NULL) {
        conf->mem_free(pq);
        return DS_ERR_ALLOC;
    }

    pq->buffer      = new_buffer;
    pq->capacity    = conf->capacity;
    pq->cmp         = conf->cmp;
    pq->exp_factor  = ex;
    pq->mem_alloc   = conf->mem_alloc;
    pq->mem_calloc  = conf->mem_calloc;
    pq->mem_free    = conf->mem_free;
    pq->size        = 0;

    *out = pq;
    return DS_OK;
}


/**
 * @brief 销毁优先队列（不释放元素数据）
 *
 * 释放队列底层缓冲区和队列结构体本身的内存。
 * 注意：此函数不会释放队列中存储的元素数据。
 *
 * @param[in] pq 要销毁的优先队列指针
 */
void ds_pqueue_destroy(ds_Pqueue *pq)
{
    pq->mem_free(pq->buffer);
    pq->mem_free(pq);
}


/**
 * @brief 销毁优先队列并释放所有元素数据
 *
 * 遍历队列，对每个元素调用 cb 函数释放其内存，
 * 然后再销毁队列结构体。
 *
 * @param[in] pq 要销毁的优先队列指针
 * @param[in] cb 释放单个元素数据的回调函数指针
 *
 * @note 如果队列中的元素存储在栈上，不应调用此函数，
 *       应使用 ds_pqueue_destroy() 代替
 */
void ds_pqueue_destroy_cb(ds_Pqueue *pq, void (*cb)(void *))
{
    for (size_t i = 0; i < pq->size; i++) {
        cb(pq->buffer[i]);
    }

    ds_pqueue_destroy(pq);
}


/**
 * @brief 向优先队列中插入元素
 *
 * 将元素插入到优先队列中，并通过上浮操作（向上调整）维护堆性质。
 * 插入时间复杂度为 O(log n)。
 *
 * @param[in,out] pq      目标优先队列
 * @param[in]     element 要插入的元素指针
 *
 * @return DS_OK 插入成功，DS_ERR_ALLOC 扩容失败，
 *         DS_ERR_MAX_CAPACITY 已达最大容量
 */
ds_Stat ds_pqueue_push(ds_Pqueue *pq, void *element)
{
    /* 如果队列已满，先扩容 */
    if (pq->size >= pq->capacity) {
        ds_Stat status = pqueue_expand_capacity(pq);

        if (status != DS_OK) {
            return status;
        }
    }

    size_t index = pq->size;

    /* 将新元素放在堆的末尾 */
    pq->buffer[index] = element;
    pq->size++;

    /* 如果只有一个元素，无需调整 */
    if (index == 0) {
        return DS_OK;
    }

    /* 上浮操作：将新元素与父节点比较，若优先级更高则交换 */
    void *child  = pq->buffer[index];
    size_t pt_idx = INDEX_PARENT(index);
    void *parent = pq->buffer[pt_idx];

    while (index != 0 && pq->cmp(child, parent) > 0) {
        /* 交换当前节点与父节点 */
        void *tmp = pq->buffer[index];

        pq->buffer[index]   = pq->buffer[pt_idx];
        pq->buffer[pt_idx]  = tmp;

        /* 更新索引并继续向上比较 */
        index  = pt_idx;
        pt_idx = INDEX_PARENT(index);
        child  = pq->buffer[index];
        parent = pq->buffer[pt_idx];
    }

    return DS_OK;
}


/**
 * @brief 获取堆顶元素（优先级最高的元素）
 *
 * 返回堆顶元素的指针，但不移除该元素。
 * 时间复杂度为 O(1)。
 *
 * @param[in]  pq  目标优先队列
 * @param[out] out 输出参数，存储堆顶元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 队列为空
 */
ds_Stat ds_pqueue_top(ds_Pqueue *pq, void **out)
{
    if (pq->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    *out = pq->buffer[0];
    return DS_OK;
}


/**
 * @brief 移除并返回堆顶元素
 *
 * 移除堆顶元素，并将堆的最后一个元素移到堆顶，
 * 然后通过下沉操作（向下调整）恢复堆性质。
 * 时间复杂度为 O(log n)。
 *
 * @param[in,out] pq  目标优先队列
 * @param[out]    out 输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 队列为空
 */
ds_Stat ds_pqueue_pop(ds_Pqueue *pq, void **out)
{
    if (pq->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    /* 将堆顶元素与最后一个元素交换 */
    void *tmp = pq->buffer[0];

    pq->buffer[0]               = pq->buffer[pq->size - 1];
    pq->buffer[pq->size - 1]    = tmp;

    /* 保存被移除的元素 */
    tmp = pq->buffer[pq->size - 1];
    pq->size--;

    /* 从堆顶开始下沉调整 */
    ds_pqueue_heapify(pq, 0);

    if (out != NULL) {
        *out = tmp;
    }

    return DS_OK;
}


/**
 * @brief 堆化操作（下沉调整）
 *
 * 从指定索引开始，将节点与其子节点比较，
 * 如果子节点优先级更高，则交换并继续向下调整。
 * 用于修复堆性质。
 *
 * @param[in,out] pq    目标优先队列
 * @param[in]     index 开始调整的节点索引
 */
void ds_pqueue_heapify(ds_Pqueue *pq, size_t index)
{
    if (pq->size <= 1) {
        return;
    }

    size_t L       = INDEX_LEFT_CHILD(index);
    size_t R       = INDEX_RIGHT_CHILD(index);
    size_t largest = index;

    /* 找出当前节点和两个子节点中优先级最高的节点 */
    if (L < pq->size) {
        void *left = pq->buffer[L];
        void *curr = pq->buffer[largest];

        if (pq->cmp(curr, left) < 0) {
            largest = L;
        }
    }

    if (R < pq->size) {
        void *right = pq->buffer[R];
        void *curr  = pq->buffer[largest];

        if (pq->cmp(curr, right) < 0) {
            largest = R;
        }
    }

    /* 如果当前节点不是优先级最高的，则交换并递归调整 */
    if (index != largest) {
        void *swap_tmp = pq->buffer[index];

        pq->buffer[index]   = pq->buffer[largest];
        pq->buffer[largest] = swap_tmp;

        ds_pqueue_heapify(pq, largest);
    }
}


/**
 * @brief 获取优先队列结构体的大小
 *
 * @return 结构体 ds_Pqueue 的字节大小
 */
size_t ds_pqueue_struct_size()
{
    return sizeof(ds_Pqueue);
}
