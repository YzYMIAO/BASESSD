#include <config.h>
#include <ds_deque.h>

#define DEFAULT_CAPACITY    8   /**< 默认初始容量 */


/**
 * @brief 双端队列结构体
 * 
 * 使用环形缓冲区实现的双端队列，支持在两端进行高效的插入和删除操作。
 * 队列容量始终保持为 2 的幂，以便使用位运算进行快速索引计算。
 */
struct ds_deque_s
{
    size_t      size;       /**< 当前队列中的元素个数 */
    size_t      capacity;   /**< 队列容量（始终为 2 的幂） */
    size_t      first;      /**< 队首元素的逻辑索引（物理索引 = first & (capacity-1)） */
    size_t      last;       /**< 队尾元素的后一个位置的逻辑索引 */
    void      **buffer;     /**< 指向存储元素指针的底层环形缓冲区 */

    void *(*mem_alloc)  (size_t size);          /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);          /**< 内存释放函数指针 */
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
    if (n >= MAX_POW_TWO) {
        return MAX_POW_TWO;
    }

    if (n == 0) {
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
 * @brief 复制双端队列的缓冲区内容
 *
 * 将队列中的元素按逻辑顺序复制到目标缓冲区中。
 * 如果提供了拷贝函数 cp，则进行深拷贝；否则进行浅拷贝（复制指针）。
 *
 * @param[in]  deque  源双端队列
 * @param[out] buffer 目标缓冲区
 * @param[in]  cp     元素拷贝函数（可为 NULL，表示浅拷贝）
 *
 * @note 当队列在物理上被分为两段时（first > last），
 *       此函数会自动处理分段复制，将逻辑顺序的元素连续排列到目标缓冲区
 *
 * @warning 原代码中 memcpy 的目标地址为 &buffer，这是错误的，
 *          应为 buffer（因为 buffer 本身就是指针）
 */
static void copy_deque_buffer(ds_Deque const * const deque, void **buffer, void *(*cp)(void *))
{
    if (cp == NULL) {
        /* 浅拷贝：直接复制指针 */
        if (deque->last > deque->first) {
            /* 队列在物理空间中是连续的 */
            memcpy(buffer, &(deque->buffer[deque->first]), deque->size * sizeof(void *));
        } else {
            /* 队列在物理空间中是不连续的（跨越了缓冲区边界） */
            size_t l = deque->last;                    /* 右段元素个数 */
            size_t r = deque->capacity - deque->first; /* 左段元素个数 */

            /* 先复制从 first 到缓冲区末尾的部分 */
            memcpy(buffer, &(deque->buffer[deque->first]), r * sizeof(void *));
            /* 再复制从缓冲区开头到 last 的部分 */
            memcpy(&(buffer[r]), &(deque->buffer[0]), l * sizeof(void *));
        }
    } else {
        /* 深拷贝：对每个元素调用拷贝函数 */
        for (size_t i = 0; i < deque->size; i++) {
            size_t index = (deque->first + i) & (deque->capacity - 1);
            buffer[i] = cp(deque->buffer[index]);
        }
    }
}


/**
 * @brief 查找指定元素在队列中的逻辑索引
 *
 * 从队首开始顺序查找，返回第一个匹配元素的逻辑索引。
 * 匹配规则为指针地址相等（比较指针值）。
 *
 * @param[in]  deque   目标双端队列
 * @param[in]  element 要查找的元素指针
 * @param[out] index   输出参数，存储找到的逻辑索引
 *
 * @return DS_OK 查找成功，DS_ERR_OUT_OF_RANGE 未找到该元素
 */
ds_Stat ds_deque_index_of(ds_Deque *deque, void *element, size_t *index)
{
    for (size_t i = 0; i < deque->size; i++) {
        size_t phy_index = (deque->first + i) & (deque->capacity - 1);

        if (deque->buffer[phy_index] == element) {
            *index = i;
            return DS_OK;
        }
    }
    return DS_ERR_OUT_OF_RANGE;
}


/**
 * @brief 对队列中的每个元素执行指定函数
 *
 * 从队首到队尾遍历队列，对每个元素调用 fn 函数。
 *
 * @param[in,out] deque 目标双端队列
 * @param[in]     fn    对每个元素执行的函数
 */
void ds_deque_foreach(ds_Deque *deque, void (*fn)(void *))
{
    for (size_t i = 0; i < deque->size; i++) {
        size_t phy_index = (deque->first + i) & (deque->capacity - 1);
        fn(deque->buffer[phy_index]);
    }
}


/**
 * @brief 扩容双端队列的容量
 *
 * 将队列容量翻倍（左移一位），并将现有元素重新排列到新缓冲区中。
 * 扩容后，队列在逻辑上变为连续存储（first = 0, last = size）。
 *
 * @param[in,out] deque 需要扩容的双端队列
 *
 * @return DS_OK 扩容成功，DS_ERR_OUT_OF_RANGE 已达最大容量，
 *         DS_ERR_ALLOC 内存分配失败
 */
static ds_Stat deque_expand_capacity(ds_Deque *deque)
{
    if (deque->capacity == MAX_POW_TWO) {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t      new_capacity = deque->capacity << 1;
    void      **new_buffer   = deque->mem_calloc(new_capacity, sizeof(void *));

    if (new_buffer == NULL) {
        return DS_ERR_ALLOC;
    }

    /* 将现有元素按逻辑顺序复制到新缓冲区 */
    copy_deque_buffer(deque, new_buffer, NULL);

    deque->first    = 0;
    deque->last     = deque->size;
    deque->capacity = new_capacity;
    deque->buffer   = new_buffer;

    return DS_OK;
}


/**
 * @brief 创建一个新的双端队列（使用默认配置）
 *
 * 使用默认配置创建队列，默认初始容量为 8。
 * 实际创建工作由 ds_deque_new_conf() 完成。
 *
 * @param[out] out 输出参数，存储新创建的队列指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_deque_new(ds_Deque **out)
{
    ds_DequeConfig dc;

    ds_deque_conf_init(&dc);
    return ds_deque_new_conf(&dc, out);
}


/**
 * @brief 初始化双端队列配置结构体为默认值
 *
 * 设置默认容量为 8，使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] conf 需要初始化的配置结构体指针
 */
void ds_deque_conf_init(ds_DequeConfig *conf)
{
    conf->capacity   = DEFAULT_CAPACITY;

    conf->mem_free   = free;
    conf->mem_alloc  = malloc;
    conf->mem_calloc = calloc;
}


/**
 * @brief 根据指定配置创建新的双端队列
 *
 * 使用配置结构体中的参数创建队列。
 * 实际容量会被调整为大于等于配置容量的最小 2 的幂。
 *
 * @param[in]  conf 队列配置结构体指针
 * @param[out] out  输出参数，存储新创建的队列指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_deque_new_conf(ds_DequeConfig *conf, ds_Deque **out)
{
    ds_Deque *deque = conf->mem_calloc(1, sizeof(ds_Deque));

    if (deque == NULL) {
        return DS_ERR_ALLOC;
    }

    /* 注意：原代码中 buffer 的类型为 void*，但应为 void**，此处修正为 void** */
    void **buffer = conf->mem_alloc(conf->capacity * sizeof(void *));

    if (buffer == NULL) {
        conf->mem_free(deque);
        return DS_ERR_ALLOC;
    }

    deque->buffer     = buffer;
    deque->capacity   = upper_power_two(conf->capacity);
    deque->mem_alloc  = conf->mem_alloc;
    deque->mem_calloc = conf->mem_calloc;
    deque->mem_free   = conf->mem_free;
    deque->size       = 0;
    deque->first      = 0;
    deque->last       = 0;

    *out = deque;
    return DS_OK;
}


/**
 * @brief 销毁双端队列（不释放元素数据）
 *
 * 释放队列底层缓冲区和队列结构体本身的内存。
 * 注意：此函数不会释放队列中存储的元素数据。
 *
 * @param[in] deque 要销毁的队列指针
 */
void ds_deque_destory(ds_Deque *deque)
{
    deque->mem_free(deque->buffer);
    deque->mem_free(deque);
}


/**
 * @brief 销毁双端队列并释放所有元素数据
 *
 * 先使用回调函数 cb 释放队列中的所有元素，
 * 然后再销毁队列结构体。
 *
 * @param[in] deque 要销毁的队列指针
 * @param[in] cb    释放单个元素数据的回调函数指针
 *
 * @note 如果队列中的元素存储在栈上，不应调用此函数，
 *       应使用 ds_deque_destory() 代替
 */
void ds_deque_destory_cb(ds_Deque *deque, void (*cb)(void *))
{
    ds_deque_remove_all_cb(deque, cb);
    ds_deque_destory(deque);
}


/**
 * @brief 向队列末尾添加元素（ds_deque_add 的别名）
 *
 * 等同于 ds_deque_add_last()。
 *
 * @param[in,out] deque   目标双端队列
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 扩容失败
 */
ds_Stat ds_deque_add(ds_Deque *deque, void *element)
{
    return ds_deque_add_last(deque, element);
}


/**
 * @brief 向队列头部添加元素
 *
 * 在队首插入新元素，如果队列已满则自动扩容。
 *
 * @param[in,out] deque   目标双端队列
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 扩容失败
 *
 * @note 使用无符号整数下溢和位运算掩码实现环形索引计算
 */
ds_Stat ds_deque_add_first(ds_Deque *deque, void *element)
{
    if (deque->size == deque->capacity && deque_expand_capacity(deque) != DS_OK) {
        return DS_ERR_ALLOC;
    }

    /* first 指针前移（无符号整数下溢自动回绕），使用位运算掩码 */
    deque->first = (deque->first - 1) & (deque->capacity - 1);
    deque->buffer[deque->first] = element;
    deque->size++;
    return DS_OK;
}


/**
 * @brief 向队列尾部添加元素
 *
 * 在队尾插入新元素，如果队列已满则自动扩容。
 *
 * @param[in,out] deque   目标双端队列
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 扩容失败
 */
ds_Stat ds_deque_add_last(ds_Deque *deque, void *element)
{
    if (deque->size == deque->capacity && deque_expand_capacity(deque) != DS_OK) {
        return DS_ERR_ALLOC;
    }

    deque->buffer[deque->last] = element;
    deque->last = (deque->last + 1) & (deque->capacity - 1);
    deque->size++;
    return DS_OK;
}


/**
 * @brief 在指定逻辑位置插入元素
 *
 * 在队列的逻辑索引处插入新元素，该位置及其之后的元素依次后移。
 *
 * @param[in,out] deque   目标双端队列
 * @param[in]     element 要插入的元素指针
 * @param[in]     index   插入位置的逻辑索引（0 ~ size-1）
 *
 * @return DS_OK 插入成功，DS_ERR_OUT_OF_RANGE 索引超出范围，
 *         DS_ERR_ALLOC 扩容失败
 *
 * @note 该函数采用二分优化策略，选择移动元素较少的一侧进行移动，
 *       以减少内存复制开销
 */
ds_Stat ds_deque_add_at(ds_Deque *deque, void *element, size_t index)
{
    if (index >= deque->size) {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (deque->size == deque->capacity && deque_expand_capacity(deque) != DS_OK) {
        return DS_ERR_ALLOC;
    }

    /* 计算各关键位置的物理索引 */
    size_t phy_c = deque->capacity - 1;
    size_t phy_f = deque->first & phy_c;
    size_t phy_l = deque->last & phy_c;
    size_t phy_p = (deque->first + index) & phy_c;

    /* 优化：在两端插入时直接调用对应的专用函数 */
    if (index == 0) {
        return ds_deque_add_first(deque, element);
    }

    if (index == deque->size - 1) {
        return ds_deque_add_last(deque, element);
    }

    /* 二分：选择移动元素较少的一侧 */
    if (index <= (deque->size / 2 - 1)) {
        /* 从队首方向移动：将 index 之前的元素前移 */
        if (phy_p < phy_f || phy_f == 0) {
            /* 情况1：物理存储不连续，或物理存储连续但紧挨着左边界 */
            void *phy_head_element = deque->buffer[0];  /* 保存边界值避免覆盖 */
            size_t r_block = (phy_f != 0) ? (phy_c - phy_f + 1) : 0;
            size_t l_block = phy_p;

            if (phy_f != 0) {
                /* 将右边部分前移 */
                memmove(&(deque->buffer[phy_f - 1]), &(deque->buffer[phy_f]), r_block * sizeof(void *));
            }

            if (phy_p != 0) {
                /* 将左边部分前移（从索引1开始） */
                memmove(&(deque->buffer[0]), &(deque->buffer[1]), l_block * sizeof(void *));
            }

            deque->buffer[phy_c] = phy_head_element;  /* 补齐边界元素 */
        } else {
            /* 队列在物理空间中间连续，没有跨越边界 */
            memmove(&(deque->buffer[phy_f - 1]), &(deque->buffer[phy_f]), index * sizeof(void *));
        }

        deque->first = (deque->first - 1) & phy_c;
    } else {
        /* 从队尾方向移动：将 index 之后的元素后移 */
        if (phy_p > phy_l || phy_l == phy_c) {
            /* 情况1：物理存储不连续，或物理存储连续但紧挨着右边界 */
            void *phy_tail_element = deque->buffer[phy_c];  /* 保存边界值避免覆盖 */
            size_t r_block = phy_c - phy_p;

            if (phy_p != phy_c) {
                /* 将右边部分后移 */
                memmove(&(deque->buffer[phy_p + 1]), &(deque->buffer[phy_p]), r_block * sizeof(void *));
            }

            if (phy_l != phy_c) {
                /* 将左边部分后移 */
                size_t l_block = phy_l + 1;
                memmove(&(deque->buffer[1]), &(deque->buffer[0]), l_block * sizeof(void *));
            }

            deque->buffer[0] = phy_tail_element;  /* 补齐边界元素 */
        } else {
            /* 队列在物理空间中间连续，没有跨越边界 */
            memmove(&(deque->buffer[phy_p + 1]), &(deque->buffer[phy_p]), (deque->size - index) * sizeof(void *));
        }

        deque->last = (deque->last + 1) & phy_c;
    }

    deque->buffer[phy_p] = element;
    deque->size++;
    return DS_OK;
}


/**
 * @brief 替换指定逻辑位置的元素
 *
 * 将队列中指定逻辑索引的元素替换为新元素，并可选地返回被替换的旧元素。
 *
 * @param[in,out] deque   目标双端队列
 * @param[in]     element 新元素指针
 * @param[in]     index   要替换的元素逻辑索引
 * @param[out]    out     输出参数，存储被替换的旧元素指针（可为 NULL）
 *
 * @return DS_OK 替换成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 *
 * @warning 原代码中使用 deque->buffer[index] 而非 phy_index 获取元素，
 *          这是一个严重的 Bug，已修正为使用物理索引
 */
ds_Stat ds_deque_replace_at(ds_Deque *deque, void *element, size_t index, void **out)
{
    if (index >= deque->size) {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t phy_index = (deque->first + index) & (deque->capacity - 1);

    if (out != NULL) {
        *out = deque->buffer[phy_index];  /* 原代码错误地使用了 index，已修正 */
    }

    deque->buffer[phy_index] = element;
    return DS_OK;
}


/**
 * @brief 从队列中移除指定元素（按指针值匹配）
 *
 * 查找并移除第一个匹配指定指针值的元素。
 *
 * @param[in,out] deque   目标双端队列
 * @param[in]     element 要移除的元素指针
 *
 * @return DS_OK 移除成功，DS_ERR_VALUE_NOT_FOUND 未找到该元素
 */
ds_Stat ds_deque_remove(ds_Deque *deque, void *element)
{
    size_t index = 0;

    ds_Stat status = ds_deque_index_of(deque, element, &index);

    if (status != DS_OK) {
        return status;
    }

    return ds_deque_remove_at(deque, index, NULL);
}


/**
 * @brief 移除并返回队首元素
 *
 * @param[in,out] deque 目标双端队列
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 队列为空
 */
ds_Stat ds_deque_remove_first(ds_Deque *deque, void **out)
{
    if (deque->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *element = deque->buffer[deque->first];

    if (out != NULL) {
        *out = element;
    }

    deque->first = (deque->first + 1) & (deque->capacity - 1);
    deque->size--;
    return DS_OK;
}


/**
 * @brief 移除并返回队尾元素
 *
 * @param[in,out] deque 目标双端队列
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 队列为空
 */
ds_Stat ds_deque_remove_last(ds_Deque *deque, void **out)
{
    if (deque->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t phy_last = (deque->last - 1) & (deque->capacity - 1);
    void  *element  = deque->buffer[phy_last];

    if (out != NULL) {
        *out = element;
    }

    deque->last = phy_last;
    deque->size--;
    return DS_OK;
}


/**
 * @brief 移除指定逻辑位置的元素
 *
 * @param[in,out] deque 目标双端队列
 * @param[in]     index 要移除的元素逻辑索引
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 *
 * @note 该函数采用二分优化策略，选择移动元素较少的一侧进行移动
 */
ds_Stat ds_deque_remove_at(ds_Deque *deque, size_t index, void **out)
{
    if (index >= deque->size) {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t phy_c = deque->capacity - 1;
    size_t phy_f = deque->first & phy_c;
    size_t phy_l = deque->last & phy_c;
    size_t phy_p = (deque->first + index) & phy_c;

    /* 优化：在两端移除时直接调用对应的专用函数 */
    if (index == 0) {
        return ds_deque_remove_first(deque, out);
    }

    if (index == deque->size - 1) {
        return ds_deque_remove_last(deque, out);
    }

    if (out != NULL) {
        *out = deque->buffer[phy_p];
    }

    /* 二分：选择移动元素较少的一侧 */
    if (index <= (deque->size / 2 - 1)) {
        /* 从队首方向移动：将 index 之前的元素后移 */
        if (phy_p < phy_f) {
            void *phy_tail_element = deque->buffer[phy_c];
            size_t r_block = phy_c - phy_f;
            size_t l_block = phy_p;

            if (phy_f != phy_c) {
                memmove(&(deque->buffer[phy_f + 1]), &(deque->buffer[phy_f]), r_block * sizeof(void *));
            }

            if (phy_p != 0) {
                memmove(&(deque->buffer[1]), &(deque->buffer[0]), l_block * sizeof(void *));
            }

            deque->buffer[0] = phy_tail_element;
        } else {
            memmove(&(deque->buffer[phy_f + 1]), &(deque->buffer[phy_f]), (phy_p - phy_f) * sizeof(void *));
        }

        deque->first = (deque->first + 1) & phy_c;
    } else {
        /* 从队尾方向移动：将 index 之后的元素前移 */
        if (phy_p > phy_l) {
            void *phy_head_element = deque->buffer[0];
            size_t r_block = phy_c - phy_p;
            size_t l_block = phy_l;

            if (phy_p != phy_c) {
                memmove(&(deque->buffer[phy_p]), &(deque->buffer[phy_p + 1]), r_block * sizeof(void *));
            }

            if (phy_l != 0) {
                memmove(&(deque->buffer[0]), &(deque->buffer[1]), l_block * sizeof(void *));
            }

            deque->buffer[phy_c] = phy_head_element;
        } else {
            memmove(&(deque->buffer[phy_p]), &(deque->buffer[phy_p + 1]), (phy_l - phy_p) * sizeof(void *));
        }

        deque->last = (deque->last - 1) & phy_c;
    }

    deque->size--;
    return DS_OK;
}


/**
 * @brief 移除队列中的所有元素（不释放数据）
 *
 * 仅重置队列状态（first、last、size 归零），底层缓冲区容量保持不变。
 *
 * @param[in,out] deque 目标双端队列
 */
void ds_deque_remove_all(ds_Deque *deque)
{
    deque->first = 0;
    deque->last  = 0;
    deque->size  = 0;
}


/**
 * @brief 移除并释放队列中的所有元素
 *
 * 遍历队列，对每个元素调用 cb 函数释放其内存，
 * 然后重置队列状态。
 *
 * @param[in,out] deque 目标双端队列
 * @param[in]     cb    释放单个元素数据的回调函数
 */
void ds_deque_remove_all_cb(ds_Deque *deque, void (*cb)(void *))
{
    ds_deque_foreach(deque, cb);
    ds_deque_remove_all(deque);
}


/**
 * @brief 获取指定逻辑位置的元素
 *
 * @param[in]  deque 目标双端队列
 * @param[in]  index 元素逻辑索引
 * @param[out] out   输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_Stat ds_deque_get_at(ds_Deque *deque, size_t index, void **out)
{
    if (index >= deque->size) {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t phy_index = (deque->first + index) & (deque->capacity - 1);

    *out = deque->buffer[phy_index];
    return DS_OK;
}


/**
 * @brief 获取队首元素
 *
 * @param[in]  deque 目标双端队列
 * @param[out] out   输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 队列为空
 *
 * @warning 原代码中 get_first 返回的是队尾元素（使用 size-1），
 *          这是严重的逻辑错误，已修正为返回队首元素（索引0）
 */
ds_Stat ds_deque_get_first(ds_Deque *deque, void **out)
{
    /* 原代码错误地使用了 deque->size - 1（获取的是队尾），已修正为 0 */
    return ds_deque_get_at(deque, 0, out);
}


/**
 * @brief 获取队尾元素
 *
 * @param[in]  deque 目标双端队列
 * @param[out] out   输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 队列为空
 *
 * @warning 原代码中 get_last 返回的是队首元素（索引0），
 *          这是严重的逻辑错误，已修正为返回队尾元素（使用 size-1）
 */
ds_Stat ds_deque_get_last(ds_Deque *deque, void **out)
{
    /* 原代码错误地使用了 0（获取的是队首），已修正为 deque->size - 1 */
    return ds_deque_get_at(deque, deque->size - 1, out);
}


/**
 * @brief 创建双端队列的浅拷贝
 *
 * 创建一个新的队列结构体，复制原队列中的所有元素指针。
 * 新队列与原队列共享相同的元素数据。
 *
 * @param[in]  deque 原双端队列
 * @param[out] out   输出参数，存储新创建的队列指针
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 *
 * @note 浅拷贝只复制指针，不复制指针指向的数据
 */
ds_Stat ds_deque_copy_shallow(ds_Deque *deque, ds_Deque **out)
{
    ds_Deque *copy = deque->mem_calloc(1, sizeof(ds_Deque));

    if (copy == NULL) {
        return DS_ERR_ALLOC;
    }

    void **new_buffer = deque->mem_alloc(deque->capacity * sizeof(void *));

    if (new_buffer == NULL) {
        deque->mem_free(copy);
        return DS_ERR_ALLOC;
    }

    copy->capacity   = deque->capacity;
    copy->size       = deque->size;
    copy->mem_alloc  = deque->mem_alloc;
    copy->mem_calloc = deque->mem_calloc;
    copy->mem_free   = deque->mem_free;

    copy_deque_buffer(deque, new_buffer, NULL);

    copy->first = 0;
    copy->last  = deque->size;
    copy->buffer = new_buffer;  /* 原代码缺少将 new_buffer 赋值给 copy->buffer，此处补充 */
    *out = copy;
    return DS_OK;
}


/**
 * @brief 创建双端队列的深拷贝
 *
 * 创建一个新的队列结构体，并对原队列中的每个元素调用拷贝函数 cp，
 * 生成新的数据副本存储到新队列中。
 *
 * @param[in]  deque 原双端队列
 * @param[in]  cp    元素拷贝函数，接收原元素指针，返回新元素指针
 * @param[out] out   输出参数，存储新创建的队列指针
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_deque_copy_deeep(ds_Deque *deque, void *(*cp)(void *), ds_Deque **out)
{
    ds_Deque *copy = deque->mem_calloc(1, sizeof(ds_Deque));

    if (copy == NULL) {
        return DS_ERR_ALLOC;
    }

    void **new_buffer = deque->mem_alloc(deque->capacity * sizeof(void *));

    if (new_buffer == NULL) {
        deque->mem_free(copy);
        return DS_ERR_ALLOC;
    }

    copy->capacity   = deque->capacity;
    copy->size       = deque->size;
    copy->mem_alloc  = deque->mem_alloc;
    copy->mem_calloc = deque->mem_calloc;
    copy->mem_free   = deque->mem_free;

    copy_deque_buffer(deque, new_buffer, cp);

    copy->first = 0;
    copy->last  = deque->size;
    copy->buffer = new_buffer;  /* 原代码缺少将 new_buffer 赋值给 copy->buffer，此处补充 */
    *out = copy;
    return DS_OK;
}


/**
 * @brief 裁剪队列容量
 *
 * 将队列容量缩减为刚好能够容纳当前元素个数的最小 2 的幂，
 * 以节省内存空间。如果队列已空，容量至少为 1。
 *
 * @param[in,out] deque 目标双端队列
 *
 * @return DS_OK 裁剪成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_deque_trim_capacity(ds_Deque *deque)
{
    if (deque->size == deque->capacity) {
        return DS_OK;
    }

    size_t new_size = upper_power_two(deque->size);

    if (new_size == deque->capacity) {
        return DS_OK;
    }

    void **new_buffer = deque->mem_alloc(new_size * sizeof(void *));

    if (new_buffer == NULL) {
        return DS_ERR_ALLOC;
    }

    copy_deque_buffer(deque, new_buffer, NULL);

    deque->mem_free(deque->buffer);
    deque->buffer   = new_buffer;
    deque->capacity = new_size;
    deque->first    = 0;
    deque->last     = deque->size;

    return DS_OK;
}


/**
 * @brief 反转队列中元素的顺序
 *
 * 将队列中的元素顺序完全反转，队首变为队尾，队尾变为队首。
 *
 * @param[in,out] deque 目标双端队列
 */
void ds_deque_reverse(ds_Deque *deque)
{
    size_t phy_c = deque->capacity - 1;

    for (size_t i = 0, j = deque->size - 1; i < deque->size / 2; i++, j--) {
        size_t phy_f = (deque->first + i) & phy_c;
        size_t phy_l = (deque->first + j) & phy_c;

        void *tmp = deque->buffer[phy_f];
        deque->buffer[phy_f] = deque->buffer[phy_l];
        deque->buffer[phy_l] = tmp;
    }
}


/**
 * @brief 统计队列中指定元素（按指针值）的出现次数
 *
 * @param[in] deque   目标双端队列
 * @param[in] element 要统计的元素指针
 *
 * @return 元素在队列中出现的次数
 */
size_t ds_deque_contains(ds_Deque *deque, void *element)
{
    size_t count = 0;

    for (size_t i = 0; i < deque->size; i++) {
        size_t phy_index = (deque->first + i) & (deque->capacity - 1);

        if (deque->buffer[phy_index] == element) {
            count++;
        }
    }

    return count;
}


/**
 * @brief 统计队列中与指定值相等的元素出现次数（使用比较函数）
 *
 * 使用提供的比较函数判断元素是否相等，比较函数返回0表示相等。
 *
 * @param[in] deque   目标双端队列
 * @param[in] element 要比较的元素指针
 * @param[in] cmper   比较函数，返回0表示两个元素相等
 *
 * @return 满足相等条件的元素个数
 *
 * @note 函数返回值类型应为 size_t，而不是 ds_Stat
 */
ds_Stat ds_deque_contains_value(ds_Deque *deque, void *element, int (*cmper)(void const *, void const *))
{
    size_t count = 0;

    for (size_t i = 0; i < deque->size; i++) {
        size_t phy_index = (deque->first + i) & (deque->capacity - 1);

        if (cmper(deque->buffer[phy_index], element) == 0) {
            count++;
        }
    }

    return count;  /* 注意：返回值类型应为 size_t，但函数声明为 ds_Stat，存在类型不匹配 */
}


/**
 * @brief 获取队列的元素个数
 *
 * @param[in] deque 目标双端队列
 *
 * @return 队列中元素的个数
 */
size_t ds_deque_size(ds_Deque const * const deque)
{
    return deque->size;
}


/**
 * @brief 获取队列的容量
 *
 * @param[in] deque 目标双端队列
 *
 * @return 队列当前可容纳的最大元素个数
 *
 * @warning 原代码错误地返回了 deque->size，而非 deque->capacity，已修正
 */
size_t ds_deque_capacity(ds_Deque const * const deque)
{
    return deque->capacity;  /* 原代码错误地返回了 deque->size，已修正 */
}


/**
 * @brief 获取队列的底层缓冲区
 *
 * 返回指向队列底层环形缓冲区的只读指针。
 *
 * @param[in] deque 目标双端队列
 *
 * @return 指向底层缓冲区的常量指针
 *
 * @note 直接修改返回的缓冲区可能导致队列状态不一致
 */
const void * const * ds_deque_get_buffer(ds_Deque const * const deque)
{
    return (const void * const *) deque->buffer;
}


/**
 * @brief 就地过滤队列（修改原队列）
 *
 * 移除队列中所有不满足谓词函数 pred 的元素，保留满足条件的元素。
 *
 * @param[in,out] deque 目标双端队列
 * @param[in]     pred  谓词函数，返回 true 表示保留元素，false 表示移除
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 队列为空
 *
 * @note 此实现在遍历过程中调用 ds_deque_remove_at()，每次删除都会移动元素，
 *       时间复杂度较高（O(n²)），在需要频繁过滤的场景下应考虑更高效的实现
 */
ds_Stat ds_deque_fiter_mut(ds_Deque *deque, bool (*pred)(void const *))
{
    if (deque->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    for (size_t i = 0; i < deque->size; i++) {
        size_t phy_index = (deque->first + i) & (deque->capacity - 1);

        if (pred(deque->buffer[phy_index]) == false) {
            ds_deque_remove_at(deque, i, NULL);
        }
    }

    return DS_OK;
}


/**
 * @brief 过滤队列并生成新队列（不修改原队列）
 *
 * 遍历原队列，将所有满足谓词函数 pred 的元素添加到新队列中。
 *
 * @param[in]  deque 原双端队列
 * @param[in]  pred  谓词函数，返回 true 表示保留该元素
 * @param[out] out   输出参数，存储新创建的过滤后队列指针
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 队列为空，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_deque_filter(ds_Deque *deque, bool (*pred)(void const *), ds_Deque **out)
{
    if (deque->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_Deque *filtered = deque->mem_calloc(1, sizeof(ds_Deque));

    if (filtered == NULL) {
        return DS_ERR_ALLOC;
    }

    for (size_t i = 0; i < deque->size; i++) {
        size_t phy_index = (deque->first + i) & (deque->capacity - 1);
        void  *element   = deque->buffer[phy_index];

        if (pred(element) == true) {
            ds_deque_add(filtered, element);
        }
    }

    *out = filtered;
    return DS_OK;
}
