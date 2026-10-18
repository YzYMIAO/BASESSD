#include "ds_queue.h"

#include "config.h"
#include "ds_deque.h"

#define DEFAULT_CAPACITY 8 /**< 默认初始容量 */

/**
 * @brief 队列结构体（基于环形缓冲区）
 *
 * 使用环形缓冲区实现的先进先出（FIFO）队列。
 * 支持 O(1) 的入队和出队操作。
 * 队列容量始终保持为 2 的幂，以便使用位运算进行快速索引计算。
 */
struct ds_queue_s
{
    size_t size;     /**< 当前队列中的元素个数 */
    size_t capacity; /**< 队列容量（始终为 2 的幂） */
    size_t first;    /**< 队首元素的逻辑索引（物理索引 = first & (capacity-1)） */
    size_t last;     /**< 队尾元素的后一个位置的逻辑索引 */
    void **buffer;   /**< 指向存储元素指针的底层环形缓冲区 */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
};

/**
 * @brief 计算大于等于 n 的最小 2 的幂
 *
 * 用于确保队列容量始终为 2 的幂，以便使用位运算代替取模运算。
 *
 * @param[in] n 输入值
 *
 * @return 大于等于 n 的最小 2 的幂
 *
 * @note 如果 n >= MAX_POW_TWO，则直接返回 MAX_POW_TWO
 */
static INLINE size_t upper_power_two(size_t n)
{
    if (n >= MAX_POW_TWO)
    {
        return MAX_POW_TWO;
    }

    if (n == 0)
    {
        return 1;
    }

    n -= 1;

    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;

#ifdef ARCH_64
    n |= n >> 32;
#endif /* ARCH_64 */

    n += 1;
    return n;
}

/**
 * @brief 复制队列的缓冲区内容
 *
 * 将队列中的元素按逻辑顺序复制到目标缓冲区中。
 * 如果提供了拷贝函数 cp，则进行深拷贝；否则进行浅拷贝（复制指针）。
 *
 * @param[in]  queue  源队列
 * @param[out] buffer 目标缓冲区
 * @param[in]  cp     元素拷贝函数（可为 NULL，表示浅拷贝）
 *
 * @note 当队列在物理上被分为两段时（first > last），
 *       此函数会自动处理分段复制，将逻辑顺序的元素连续排列到目标缓冲区
 *
 * @warning 原代码中浅拷贝分支的 memcpy 目标地址为 &buffer，
 *          这是错误的，应为 buffer（因为 buffer 本身就是指针）
 */
static void copy_deque_buffer(ds_Queue const *const queue, void **buffer, void *(*cp)(void *))
{
    if (cp == NULL)
    {
        /* 浅拷贝：直接复制指针 */
        if (queue->last > queue->first)
        {
            /* 队列在物理空间中是连续的 */
            memcpy(buffer, &(queue->buffer[queue->first]), queue->size * sizeof(void *));
        }
        else
        {
            /* 队列在物理空间中是不连续的（跨越了缓冲区边界） */
            size_t l = queue->last;                    /* 右段元素个数 */
            size_t r = queue->capacity - queue->first; /* 左段元素个数 */

            /* 先复制从 first 到缓冲区末尾的部分 */
            memcpy(buffer, &(queue->buffer[queue->first]), r * sizeof(void *));
            /* 再复制从缓冲区开头到 last 的部分 */
            memcpy(&(buffer[r]), &(queue->buffer[0]), l * sizeof(void *));
        }
    }
    else
    {
        /* 深拷贝：对每个元素调用拷贝函数 */
        for (size_t i = 0; i < queue->size; i++)
        {
            size_t index = (queue->first + i) & (queue->capacity - 1);
            buffer[i]    = cp(queue->buffer[index]);
        }
    }
}

/**
 * @brief 扩容队列的容量
 *
 * 将队列容量翻倍（左移一位），并将现有元素重新排列到新缓冲区中。
 * 扩容后，队列在逻辑上变为连续存储（first = 0, last = size）。
 *
 * @param[in,out] queue 需要扩容的队列
 *
 * @return DS_OK 扩容成功，DS_ERR_MAX_CAPACITY 已达最大容量，
 *         DS_ERR_ALLOC 内存分配失败
 */
static ds_Stat queue_expand_capacity(ds_Queue *queue)
{
    if (queue->capacity == MAX_POW_TWO)
    {
        return DS_ERR_MAX_CAPACITY;
    }

    size_t new_capacity = queue->capacity << 1;
    void **new_buffer   = queue->mem_calloc(new_capacity, sizeof(void *));

    if (new_buffer == NULL)
    {
        return DS_ERR_ALLOC;
    }

    /* 将现有元素按逻辑顺序复制到新缓冲区 */
    copy_deque_buffer(queue, new_buffer, NULL);

    queue->buffer   = new_buffer;
    queue->capacity = new_capacity;
    queue->first    = 0;
    queue->last     = queue->size;

    return DS_OK;
}

/**
 * @brief 创建一个新的队列（使用默认配置）
 *
 * 使用默认配置创建队列，默认初始容量为 8。
 *
 * @param[out] queue 输出参数，存储新创建的队列指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码参数类型为 ds_Queue *queue（值传递），
 *          无法返回新创建的队列指针，应为 ds_Queue **queue
 */
ds_Stat ds_queue_new(ds_Queue **queue)
{
    ds_QueueConfig conf;

    ds_queue_conf_init(&conf);
    return ds_queue_new_conf(&conf, queue);
}

/**
 * @brief 初始化队列配置结构体为默认值
 *
 * 设置默认容量为 8，使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] conf 需要初始化的配置结构体指针
 */
void ds_queue_conf_init(ds_QueueConfig *conf)
{
    conf->capacity = DEFAULT_CAPACITY;

    conf->mem_free   = free;
    conf->mem_alloc  = malloc;
    conf->mem_calloc = calloc;
}

/**
 * @brief 根据指定配置创建新的队列
 *
 * 使用配置结构体中的参数创建队列。
 * 实际容量会被调整为大于等于配置容量的最小 2 的幂。
 *
 * @param[in]  conf 队列配置结构体指针
 * @param[out] out  输出参数，存储新创建的队列指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码中 buffer 的类型为 void*，但应为 void**，
 *          这是一个严重的类型错误，已修正
 */
ds_Stat ds_queue_new_conf(ds_QueueConfig *conf, ds_Queue **out)
{
    ds_Queue *queue = conf->mem_calloc(1, sizeof(ds_Queue));

    if (queue == NULL)
    {
        return DS_ERR_ALLOC;
    }

    /* 注意：原代码中 buffer 的类型为 void*，但应为 void** */
    void **buffer = conf->mem_alloc(conf->capacity * sizeof(void *));

    if (buffer == NULL)
    {
        conf->mem_free(queue);
        return DS_ERR_ALLOC;
    }

    queue->buffer     = buffer;
    queue->capacity   = upper_power_two(conf->capacity);
    queue->mem_alloc  = conf->mem_alloc;
    queue->mem_calloc = conf->mem_calloc;
    queue->mem_free   = conf->mem_free;
    queue->size       = 0;
    queue->first      = 0;
    queue->last       = 0;

    *out = queue;
    return DS_OK;
}

/**
 * @brief 销毁队列（不释放元素数据）
 *
 * 释放队列底层缓冲区和队列结构体本身的内存。
 * 注意：此函数不会释放队列中存储的元素数据。
 *
 * @param[in] queue 要销毁的队列指针
 */
void ds_queue_destroy(ds_Queue *queue)
{
    queue->mem_free(queue->buffer);
    queue->mem_free(queue);
}

/**
 * @brief 销毁队列并释放所有元素数据
 *
 * 遍历队列，对每个元素调用 cb 函数释放其内存，
 * 然后再销毁队列结构体。
 *
 * @param[in] queue 要销毁的队列指针
 * @param[in] cb    释放单个元素数据的回调函数指针
 *
 * @note 如果队列中的元素存储在栈上，不应调用此函数，
 *       应使用 ds_queue_destroy() 代替
 */
void ds_queue_destroy_cb(ds_Queue *queue, void (*cb)(void *))
{
    for (size_t i = 0; i < queue->size; i++)
    {
        size_t phy_index = (queue->first + i) & (queue->capacity - 1);
        cb(queue->buffer[phy_index]);
    }
    ds_queue_destroy(queue);
}

/**
 * @brief 入队操作（从队尾入队）
 *
 * 将元素插入到队列的尾部。
 * 此实现采用从队尾入队、从队首出队的策略。
 *
 * @param[in,out] queue   目标队列
 * @param[in]     element 要入队的元素指针
 *
 * @return DS_OK 入队成功，DS_ERR_ALLOC 扩容失败，
 *         DS_ERR_MAX_CAPACITY 已达最大容量
 *
 * @note 入队时 first 指针前移（无符号整数下溢自动回绕），
 *       使用位运算掩码实现环形索引计算
 */
ds_Stat ds_queue_push(ds_Queue *queue, void *element)
{
    if (queue->size >= queue->capacity)
    {
        ds_Stat status = queue_expand_capacity(queue);

        if (status != DS_OK)
        {
            return status;
        }
    }

    /* first 指针前移（无符号整数下溢自动回绕），使用位运算掩码 */
    queue->first                = (queue->first - 1) & (queue->capacity - 1);
    queue->buffer[queue->first] = element;
    queue->size++;

    return DS_OK;
}

/**
 * @brief 出队操作（从队首出队）
 *
 * 移除并返回队首元素。
 *
 * @param[in,out] queue 目标队列
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 出队成功，DS_ERR_OUT_OF_RANGE 队列为空
 *
 * @note 出队时 last 指针前移，指向新的队尾位置
 */
ds_Stat ds_queue_pop(ds_Queue *queue, void **out)
{
    if (queue->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    /* last 指针前移，指向当前队尾元素的位置 */
    queue->last = (queue->last - 1) & (queue->capacity - 1);

    if (out != NULL)
    {
        *out = queue->buffer[queue->last];
    }

    queue->size--;
    return DS_OK;
}

/**
 * @brief 查看队首元素（不移除）
 *
 * 返回队首元素的指针，但不将其从队列中移除。
 *
 * @param[in]  queue 目标队列
 * @param[out] out   输出参数，存储队首元素指针
 *
 * @return DS_OK 查看成功，DS_ERR_OUT_OF_RANGE 队列为空
 */
ds_Stat ds_queue_peek(ds_Queue *queue, void **out)
{
    if (queue->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t tail = (queue->last - 1) & (queue->capacity - 1);
    *out        = queue->buffer[tail];

    return DS_OK;
}

/**
 * @brief 对队列中的每个元素执行指定函数
 *
 * 从队首到队尾遍历队列，对每个元素调用 fn 函数。
 *
 * @param[in,out] queue 目标队列
 * @param[in]     fn    对每个元素执行的函数
 */
void ds_queue_foreach(ds_Queue *queue, void (*fn)(void *))
{
    for (size_t i = 0; i < queue->size; i++)
    {
        size_t phy = (queue->first + i) & (queue->capacity - 1);
        fn(queue->buffer[phy]);
    }
}

/**
 * @brief 获取队列的元素个数
 *
 * @param[in] queue 目标队列
 *
 * @return 队列中元素的个数
 */
size_t ds_queue_size(ds_Queue *queue)
{
    return queue->size;
}
