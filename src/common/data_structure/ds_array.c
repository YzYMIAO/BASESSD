#include "ds_array.h"

#include "config.h"

#define DEFAULT_CAPACITY         8 /**< 默认初始容量 */
#define DEFAULT_EXPANSION_FACTOR 2 /**< 默认扩容因子 */

/**
 * 动态数组结构体
 *
 * 该结构体管理一个动态数组，支持自动扩容、元素增删改查等操作。
 * 数组存储的是 void* 类型的指针，可以指向任意类型的数据。
 */
struct DS_ARRAY_S
{
    size_t size;       /**< 当前数组中元素的个数 */
    size_t capacity;   /**< 当前数组的容量（可容纳的最大元素数） */
    float  exp_factor; /**< 扩容因子，用于计算扩容时的新容量 */
    void **buffer;     /**< 指向存储元素指针的底层缓冲区 */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
};

/**
 * 在数组中查找指定元素并返回其索引
 *
 * 从数组起始位置开始顺序查找，返回第一个匹配元素的索引。
 * 匹配规则为指针地址相等（即比较指针值，而非指向的内容）。
 *
 * @param[in]  ar      目标数组
 * @param[in]  element 要查找的元素指针
 * @param[out] index   输出参数，存储找到的元素索引
 *
 * @return DS_OK 表示查找成功，DS_ERR_OUT_OF_RANGE 表示未找到该元素
 */
ds_status_e ar_array_index_of(ds_array_s *ar, void *element, size_t *index)
{
    for (size_t i = 0; i < ar->size; i++)
    {
        if (ar->buffer[i] == element)
        {
            *index = i;
            return DS_OK;
        }
    }
    return DS_ERR_OUT_OF_RANGE;
}

/**
 * 扩容数组的容量
 *
 * 当数组元素个数达到容量上限时，按照扩容因子计算新容量并重新分配内存。
 * 如果当前容量已达到 DS_MAX_ELEMENTS，则不再扩容。
 *
 * @param[in,out] ar 需要扩容的数组
 *
 * @return DS_OK 扩容成功，DS_ERR_MAX_CAPACITY 已达最大容量，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_status_e ar_expand_capacity(ds_array_s *ar)
{
    if (ar->capacity == DS_MAX_ELEMENTS)
    {
        return DS_ERR_MAX_CAPACITY;
    }

    size_t new_capacity = ar->capacity * ar->exp_factor;

    /* 检查容量溢出：如果新容量小于原容量，说明发生了整数溢出 */
    if (new_capacity < ar->capacity)
    {
        ar->capacity = DS_MAX_ELEMENTS;
    }
    else
    {
        ar->capacity = new_capacity;
    }

    void **new_buff = ar->mem_alloc(ar->capacity * sizeof(void *));

    if (new_buff == NULL)
    {
        return DS_ERR_ALLOC;
    }

    /* 将旧缓冲区中的数据复制到新缓冲区 */
    memcpy(new_buff, ar->buffer, sizeof(void *) * ar->capacity);

    ar->mem_free(ar->buffer);
    ar->buffer = new_buff;
    return DS_OK;
}

/**
 * 创建一个新的动态数组
 *
 * 使用默认配置创建数组，包括默认初始容量和默认扩容因子。
 * 实际创建工作由 ds_array_new_confing() 完成。
 *
 * @param[out] out 输出参数，存储新创建的数组指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_status_e ds_array_new(ds_array_s **out)
{
    ds_array_cfg_s ac;
    ds_array_confing_init(&ac);
    return ds_array_new_confing(&ac, out);
}

/**
 * 根据指定配置创建新数组
 *
 * 使用配置结构体中的参数（初始容量、扩容因子、内存分配器）创建数组。
 * 如果配置中的扩容因子小于等于1，则使用默认扩容因子2。
 *
 * @param[in]  ac  数组配置结构体指针
 * @param[out] out 输出参数，存储新创建的数组指针
 *
 * @return DS_OK 创建成功，DS_ERR_INVALID_CAPACITY 容量配置无效，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @note 配置中的容量不能为0，且扩容因子必须满足
 *       exp_factor < DS_MAX_ELEMENTS / capacity，否则返回 DS_ERR_INVALID_CAPACITY
 */
ds_status_e ds_array_new_confing(ds_array_cfg_s *ac, ds_array_s **out)
{
    float ex;

    if (ac->exp_factor <= 1)
    {
        ex = DEFAULT_EXPANSION_FACTOR;
    }
    else
    {
        ex = ac->exp_factor;
    }

    /* 检查容量配置是否有效 */
    if (ac->capacity == 0 || ex > DS_MAX_ELEMENTS / ac->capacity)
    {
        return DS_ERR_INVALID_CAPACITY;
    }

    ds_array_s *ar = ac->mem_calloc(1, sizeof(ds_array_s));

    if (ar == NULL)
    {
        return DS_ERR_ALLOC;
    }

    void **buff = ac->mem_alloc(ac->capacity * sizeof(void *));

    if (buff == NULL)
    {
        ac->mem_free(ar);
        return DS_ERR_ALLOC;
    }

    ar->buffer     = buff;
    ar->exp_factor = ex;
    ar->capacity   = ac->capacity;
    ar->mem_alloc  = ac->mem_alloc;
    ar->mem_calloc = ac->mem_calloc;
    ar->mem_free   = ac->mem_free;

    *out = ar;
    return DS_OK;
}

/**
 * 初始化数组配置结构体为默认值
 *
 * 设置默认容量为8，默认扩容因子为2，
 * 内存分配器使用标准的 malloc/calloc/free。
 *
 * @param[in,out] ac 需要初始化的配置结构体指针
 */
void ds_array_confing_init(ds_array_cfg_s *ac)
{
    ac->capacity   = DEFAULT_CAPACITY;
    ac->exp_factor = DEFAULT_EXPANSION_FACTOR;

    ac->mem_free   = free;
    ac->mem_alloc  = malloc;
    ac->mem_calloc = calloc;
}

/**
 * 销毁数组（不释放元素数据）
 *
 * 释放数组底层缓冲区和数组结构体本身的内存。
 * 注意：此函数不会释放数组中存储的元素数据。
 *
 * @param[in] ar 要销毁的数组指针
 */
void ds_array_destroy(ds_array_s *ar)
{
    ar->mem_free(ar->buffer);
    ar->mem_free(ar);
}

/**
 * 销毁数组并释放所有元素数据
 *
 * 遍历数组中的每个元素，调用回调函数 cb 释放元素数据，
 * 然后再销毁数组结构体。
 *
 * @param[in] ar 要销毁的数组指针
 * @param[in] cb 释放单个元素数据的回调函数指针
 *
 * @note 如果数组中的元素存储在栈上，不应调用此函数，
 *       应使用 ds_array_destroy() 代替。
 */
void ds_array_destroy_cb(ds_array_s *ar, void (*cb)(void *))
{
    for (size_t i = 0; i < ar->size; i++)
    {
        cb(ar->buffer[i]);
    }
    ds_array_destroy(ar);
}

/**
 * 向数组末尾添加一个元素
 *
 * 如果数组已满，会自动触发扩容操作。
 *
 * @param[in,out] ar      目标数组
 * @param[in]     element 要添加的元素指针
 *
 * @return DS_OK 添加成功，DS_ERR_ALLOC 扩容失败，
 *         DS_ERR_MAX_CAPACITY 已达最大容量
 */
ds_status_e ds_array_add(ds_array_s *ar, void *element)
{
    if (ar->size >= ar->capacity)
    {
        ds_status_e status = ar_expand_capacity(ar);
        if (status != DS_OK)
        {
            return status;
        }
    }

    ar->buffer[ar->size] = element;
    ar->size++;

    return DS_OK;
}

/**
 * 在指定位置插入元素
 *
 * 在数组的指定索引处插入新元素，该位置及其之后的元素依次后移。
 * 如果索引等于数组大小，则相当于在末尾追加。
 *
 * @param[in,out] ar      目标数组
 * @param[in]     element 要插入的元素指针
 * @param[in]     index   插入位置的索引（0 ~ size）
 *
 * @return DS_OK 插入成功，DS_ERR_OUT_OF_RANGE 索引超出范围，
 *         DS_ERR_ALLOC 扩容失败，DS_ERR_MAX_CAPACITY 已达最大容量
 *
 * @note 当 index == ar->size 时，行为与 ds_array_add() 相同
 */
ds_status_e ds_array_add_at(ds_array_s *ar, void *element, size_t index)
{
    /* 如果索引等于size，等同于在末尾添加 */
    if (ar->size == index)
    {
        ar->buffer[index] = element;
        ar->size++;
        return DS_OK;
    }

    /* 检查索引是否有效 */
    if ((ar->size == 0 && index != 0) || (index > ar->size - 1))
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (ar->size >= ar->capacity)
    {
        ds_status_e status = ar_expand_capacity(ar);
        if (status != DS_OK)
        {
            return status;
        }
    }

    /* 将 index 及其之后的元素向后移动一个位置 */
    size_t shift = (ar->size - index) * sizeof(void *);
    memmove(&(ar->buffer[index + 1]), &(ar->buffer[index]), shift);

    ar->buffer[index] = element;
    ar->size++;
    return DS_OK;
}

/**
 * 替换指定位置的元素
 *
 * 将数组中指定索引的元素替换为新元素，并可选地返回被替换的旧元素。
 *
 * @param[in,out] ar      目标数组
 * @param[in]     element 新元素指针
 * @param[in]     index   要替换的元素索引
 * @param[out]    out     输出参数，存储被替换的旧元素指针（可为 NULL）
 *
 * @return DS_OK 替换成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_status_e ds_array_replace_at(ds_array_s *ar, void *element, size_t index, void **out)
{
    if (index >= ar->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (out != NULL)
    {
        *out = ar->buffer[index]; /* 注意：原代码写的是 out = ar->buffer[index]，已修正 */
    }

    ar->buffer[index] = element;
    return DS_OK;
}

/**
 * 交换数组中两个位置的元素
 *
 * @param[in,out] ar     目标数组
 * @param[in]     index1 第一个元素的索引
 * @param[in]     index2 第二个元素的索引
 *
 * @return DS_OK 交换成功，DS_ERR_OUT_OF_RANGE 任一索引超出范围
 */
ds_status_e ds_array_swap_at(ds_array_s *ar, size_t index1, size_t index2)
{
    if (index1 >= ar->size || index2 >= ar->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *temp         = ar->buffer[index1];
    ar->buffer[index1] = ar->buffer[index2];
    ar->buffer[index2] = temp;

    return DS_OK;
}

/**
 * 从数组中移除指定元素（按指针值匹配）
 *
 * 查找并移除第一个匹配指定指针值的元素，被移除元素之后的元素依次前移。
 * 如果提供了 out 参数，会返回被移除的元素指针。
 *
 * @param[in,out] ar      目标数组
 * @param[in]     element 要移除的元素指针
 * @param[out]    out     输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_VALUE_NOT_FOUND 未找到该元素
 */
ds_status_e ds_array_remove(ds_array_s *ar, void *element, void **out)
{
    size_t index;

    ds_status_e status = ar_array_index_of(ar, element, &index);

    if (status == DS_ERR_OUT_OF_RANGE)
    {
        return DS_ERR_VALUE_NOT_FOUND;
    }

    if (index != ar->size - 1)
    {
        size_t shift_size = (ar->size - 1 - index) * sizeof(void *);
        memmove(&(ar->buffer[index]), &(ar->buffer[index + 1]), shift_size);
    }

    ar->size--;

    if (out != NULL)
    {
        *out = element;
    }

    return DS_OK;
}

/**
 * 移除指定索引位置的元素
 *
 * 移除数组中指定索引的元素，被移除元素之后的元素依次前移。
 *
 * @param[in,out] ar      目标数组
 * @param[in]     index   要移除的元素索引
 * @param[out]    out     输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_status_e ds_array_remove_at(ds_array_s *ar, size_t index, void **out)
{
    if (index >= ar->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    if (out != NULL)
    {
        *out = ar->buffer[index];
    }

    if (index != ar->size - 1)
    {
        size_t shift_size = (ar->size - index - 1) * sizeof(void *);
        memmove(&(ar->buffer[index]), &(ar->buffer[index + 1]), shift_size);
    }

    ar->size--;
    return DS_OK;
}

/**
 * 移除数组末尾的元素
 *
 * @param[in,out] ar  目标数组
 * @param[out]    out 输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_OUT_OF_RANGE 数组为空
 */
ds_status_e ds_array_remove_last(ds_array_s *ar, void **out)
{
    return ds_array_remove_at(ar, ar->size - 1, out);
}

/**
 * 移除数组中的所有元素（不释放数据）
 *
 * 仅将数组大小设置为0，底层缓冲区容量保持不变。
 * 数组中存储的元素指针不会被释放。
 *
 * @param[in,out] ar 目标数组
 */
void ds_array_remove_all(ds_array_s *ar)
{
    ar->size = 0;
}

/**
 * 移除并释放数组中的所有元素
 *
 * 遍历数组中的每个元素，调用 free() 释放元素数据，
 * 然后将数组大小设置为0。
 *
 * @param[in,out] ar 目标数组
 *
 * @note 此函数使用标准 free() 释放内存，如果元素使用自定义分配器分配，
 *       应使用 ds_array_destroy_cb() 配合自定义释放函数
 */
void ds_array_remove_all_free(ds_array_s *ar)
{
    for (size_t i = 0; i < ar->size; i++)
    {
        free(ar->buffer[i]);
    }
    ds_array_remove_all(ar);
}

/**
 * 获取指定索引位置的元素
 *
 * @param[in]  ar    目标数组
 * @param[in]  index 元素索引
 * @param[out] out   输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 索引超出范围
 */
ds_status_e ds_array_get_at(ds_array_s *ar, size_t index, void **out)
{
    if (index >= ar->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    *out = ar->buffer[index];
    return DS_OK;
}

/**
 * 获取数组末尾的元素
 *
 * @param[in]  ar  目标数组
 * @param[out] out 输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ERR_OUT_OF_RANGE 数组为空
 */
ds_status_e ds_array_get_last(ds_array_s *ar, void **out)
{
    if (ar->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    return ds_array_get_at(ar, ar->size - 1, out);
}

/**
 * 获取数组的底层缓冲区
 *
 * 返回指向数组底层缓冲区的只读指针。
 *
 * @param[in] ar 目标数组
 *
 * @return 指向底层缓冲区的常量指针
 *
 * @note 直接修改返回的缓冲区可能导致数组状态不一致，
 *       应使用数组提供的 API 函数进行修改
 */
const void *const *ds_array_get_buffer(ds_array_s *ar)
{
    return (const void *const *)ar->buffer;
}

/**
 * 创建数组的子数组（副本）
 *
 * 从原数组中截取 [b, e] 范围内的元素，创建一个新的数组副本。
 * 新数组的容量与原数组相同，但实际大小等于截取的元素个数。
 *
 * @param[in]  ar   原数组
 * @param[in]  b    起始索引（包含）
 * @param[in]  e    结束索引（包含）
 * @param[out] out  输出参数，存储新创建的子数组指针
 *
 * @return DS_OK 创建成功，DS_ERR_OUT_OF_RANGE 索引范围无效，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @note 子数组继承原数组的内存分配器和扩容因子配置
 */
ds_status_e ds_array_subrarry(ds_array_s *ar, size_t b, size_t e, ds_array_s **out)
{
    if (b > e || e >= ar->size)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_array_s *sub_ar = ar->mem_calloc(1, sizeof(ds_array_s));

    if (sub_ar == NULL)
    {
        return DS_ERR_ALLOC;
    }

    sub_ar->buffer = ar->mem_alloc(ar->capacity * sizeof(void *));

    if (sub_ar->buffer == NULL)
    {
        ar->mem_free(sub_ar);
        return DS_ERR_ALLOC;
    }

    sub_ar->capacity   = ar->capacity;
    sub_ar->exp_factor = ar->exp_factor;
    sub_ar->mem_alloc  = ar->mem_alloc;
    sub_ar->mem_calloc = ar->mem_calloc;
    sub_ar->mem_free   = ar->mem_free;
    sub_ar->size       = e - b + 1;

    /* 注意：原代码 memcpy 的源地址参数错误，缺少取地址符 &，此处修正 */
    memcpy(sub_ar->buffer, &(ar->buffer[b]), sub_ar->size * sizeof(void *));
    *out = sub_ar;

    return DS_OK;
}

/**
 * 创建数组的浅拷贝
 *
 * 创建一个新的数组结构体，复制原数组中的所有元素指针。
 * 新数组与原数组共享相同的元素数据。
 *
 * @param[in]  ar  原数组
 * @param[out] out 输出参数，存储新创建的数组指针
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 *
 * @note 浅拷贝只复制指针，不复制指针指向的数据
 */
ds_status_e ds_array_copy_shallow(ds_array_s *ar, ds_array_s **out)
{
    ds_array_s *copy = ar->mem_calloc(1, sizeof(ds_array_s));

    if (copy == NULL)
    {
        return DS_ERR_ALLOC;
    }

    /* 原代码这里使用了 copy->mem_alloc，但 copy 尚未完全初始化，
     * 且 copy->capacity 未初始化，存在潜在问题，已修正为 ar->mem_alloc */
    copy->buffer = ar->mem_alloc(ar->capacity * sizeof(void *));

    if (copy->buffer == NULL)
    {
        ar->mem_free(copy);
        return DS_ERR_ALLOC;
    }

    copy->capacity   = ar->capacity;
    copy->exp_factor = ar->exp_factor;
    copy->mem_alloc  = ar->mem_alloc;
    copy->mem_calloc = ar->mem_calloc;
    copy->mem_free   = ar->mem_free;
    copy->size       = ar->size; /* 原代码缺少设置 size，此处补充 */

    memcpy(copy->buffer, ar->buffer, ar->size * sizeof(void *));
    *out = copy;
    return DS_OK;
}

/**
 * 创建数组的深拷贝
 *
 * 创建一个新的数组结构体，并对原数组中的每个元素调用拷贝函数 cp，
 * 生成新的数据副本存储到新数组中。
 *
 * @param[in]  ar  原数组
 * @param[in]  cp  元素拷贝函数，接收原元素指针，返回新元素指针
 * @param[out] out 输出参数，存储新创建的数组指针
 *
 * @return DS_OK 复制成功，DS_ERR_ALLOC 内存分配失败
 *
 * @note 拷贝函数 cp 需要正确处理元素的深层复制
 */
ds_status_e ds_array_copy_deep(ds_array_s *ar, void *(*cp)(void *), ds_array_s **out)
{
    ds_array_s *copy = ar->mem_calloc(1, sizeof(ds_array_s));

    if (copy == NULL)
    {
        return DS_ERR_ALLOC;
    }

    /* 原代码使用了 copy->mem_alloc，但 copy->capacity 未初始化，已修正 */
    copy->buffer = ar->mem_alloc(ar->capacity * sizeof(void *));

    if (copy->buffer == NULL)
    {
        ar->mem_free(copy);
        return DS_ERR_ALLOC;
    }

    copy->capacity   = ar->capacity;
    copy->exp_factor = ar->exp_factor;
    copy->mem_alloc  = ar->mem_alloc;
    copy->mem_calloc = ar->mem_calloc;
    copy->mem_free   = ar->mem_free;
    copy->size       = ar->size;

    for (size_t i = 0; i < ar->size; i++)
    {
        copy->buffer[i] = cp(ar->buffer[i]);
    }

    *out = copy;
    return DS_OK;
}

/**
 * 就地过滤数组（修改原数组）
 *
 * 移除数组中所有不满足谓词函数 pred 的元素，保留满足条件的元素。
 *
 * @param[in,out] ar   目标数组
 * @param[in]     pred 谓词函数，返回 true 表示保留元素，false 表示移除
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 数组为空
 */
ds_status_e ds_array_filter_mut(ds_array_s *ar, BOOL (*pred)(const void *))
{
    if (ar->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t rm   = 0;
    size_t keep = 0;
    size_t i    = 0;

    /* 从后向前遍历，减少元素移动次数 */
    for (i = ar->size - 1; i != ((size_t)-1); i--)
    {
        if (pred(ar->buffer[i]) == false)
        {
            rm++;
            continue;
        }

        if (rm > 0)
        {
            if (keep > 0)
            {
                /* 注意：原代码 memmove 参数地址错误，已修正为取地址符 */
                memmove(&(ar->buffer[i + 1]), &(ar->buffer[i + 1 + rm]), keep * sizeof(void *));
                keep = 0;
            }
            ar->size -= rm;
            rm = 0;
        }
        keep++;
    }

    /* 处理剩余需要移除的元素 */
    if (rm > 0)
    {
        if (keep > 0)
        {
            memmove(&(ar->buffer[0]), &(ar->buffer[rm]), keep * sizeof(void *));
            keep = 0;
        }
        ar->size -= rm;
    }

    return DS_OK;
}

/**
 * 过滤数组并生成新数组（不修改原数组）
 *
 * 遍历原数组，将所有满足谓词函数 pred 的元素复制到新数组中。
 *
 * @param[in]  ar   原数组
 * @param[in]  pred 谓词函数，返回 true 表示保留该元素
 * @param[out] out  输出参数，存储新创建的过滤后数组指针
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 数组为空，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码中存在严重 bug：filter->buffer 分配成功后，
 *          错误地将 ar->buffer 指向了新分配的内存，这会导致原数组被破坏。
 *          以下注释已修正该问题。
 */
ds_status_e ds_array_filter(ds_array_s *ar, BOOL (*pred)(const void *), ds_array_s **out)
{
    if (ar->size == 0)
    {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_array_s *filter = ar->mem_calloc(1, sizeof(ds_array_s));

    if (filter == NULL)
    {
        return DS_ERR_ALLOC;
    }

    /* 注意：原代码为 ar->buffer = ar->mem_alloc(...)，这会覆盖原数组的缓冲区！
     * 正确做法是为 filter->buffer 分配内存，已修正 */
    filter->buffer = ar->mem_alloc(ar->capacity * sizeof(void *));

    if (filter->buffer == NULL)
    {
        ar->mem_free(filter);
        return DS_ERR_ALLOC;
    }

    size_t f = 0;

    for (size_t i = 0; i < ar->size; i++)
    {
        if (pred(ar->buffer[i]) == true)
        {
            filter->buffer[f++] = ar->buffer[i];
        }
    }

    filter->capacity   = ar->capacity;
    filter->exp_factor = ar->exp_factor;
    filter->mem_alloc  = ar->mem_alloc;
    filter->mem_calloc = ar->mem_calloc;
    filter->mem_free   = ar->mem_free;
    filter->size       = f;

    *out = filter;
    return DS_OK;
}

/**
 * 反转数组中元素的顺序
 *
 * 将数组中的元素顺序完全反转，第一个变为最后一个，依此类推。
 *
 * @param[in,out] ar 目标数组
 */
void ds_array_reverse(ds_array_s *ar)
{
    if (ar->size == 0)
    {
        return;
    }

    for (size_t i = 0, j = ar->size - 1; i < ar->size / 2; i++, j--)
    {
        void *temp    = ar->buffer[i];
        ar->buffer[i] = ar->buffer[j];
        ar->buffer[j] = temp;
    }
}

/**
 * 统计数组中指定元素（按指针值）的出现次数
 *
 * @param[in] ar      目标数组
 * @param[in] element 要统计的元素指针
 *
 * @return 元素在数组中出现的次数
 */
size_t ds_array_contains(ds_array_s *ar, void *element)
{
    size_t count = 0;

    for (size_t i = 0; i < ar->size; i++)
    {
        if (ar->buffer[i] == element)
        {
            count++;
        }
    }

    return count;
}

/**
 * 统计数组中与指定值相等的元素出现次数（使用比较函数）
 *
 * 使用提供的比较函数判断元素是否相等，比较函数返回0表示相等。
 *
 * @param[in] ar      目标数组
 * @param[in] element 要比较的元素指针
 * @param[in] comper  比较函数，返回0表示两个元素相等
 *
 * @return 满足相等条件的元素个数
 */
size_t ds_array_contains_value(ds_array_s *ar, void *element, int (*comper)(const void *, const void *))
{
    size_t count = 0;

    for (size_t i = 0; i < ar->size; i++)
    {
        if (comper(ar->buffer[i], element) == 0)
        {
            count++;
        }
    }

    return count;
}

/**
 * 获取数组的元素个数
 *
 * @param[in] ar 目标数组
 *
 * @return 数组中元素的个数
 */
size_t ds_array_size(ds_array_s *ar)
{
    return ar->size;
}

/**
 * 获取数组的容量
 *
 * @param[in] ar 目标数组
 *
 * @return 数组当前可容纳的最大元素个数
 */
size_t ds_array_capacity(ds_array_s *ar)
{
    return ar->capacity;
}

/**
 * 对数组进行排序
 *
 * 使用标准库的 qsort 函数对数组元素进行排序。
 *
 * @param[in,out] ar     目标数组
 * @param[in]     comper 比较函数，返回值 < 0 表示第一个元素小于第二个，
 *                        0 表示相等，> 0 表示大于
 *
 * @note 比较函数接收的参数类型为 void**，需要进行双重解引用才能获取实际数据
 */
void ds_array_sort(ds_array_s *ar, int (*comper)(const void *, const void *))
{
    qsort(ar->buffer, ar->size, sizeof(void *), comper);
}

/**
 * 对数组中的每个元素执行映射操作
 *
 * 遍历数组，对每个元素调用 fn_map 函数进行处理。
 *
 * @param[in,out] ar     目标数组
 * @param[in]     fn_map 映射函数，接收元素指针作为参数
 */
void ds_array_map(ds_array_s *ar, void (*fn_map)(const void *))
{
    for (size_t i = 0; i < ar->size; i++)
    {
        fn_map(ar->buffer[i]);
    }
}

/**
 * 对数组进行归约操作
 *
 * 将数组中的所有元素通过二元函数聚合成一个结果。
 * 例如：对 [a, b, c, d] 执行归约，结果为 ((a op b) op c) op d。
 *
 * @param[in,out] ar     目标数组
 * @param[in]     fn     二元归约函数，接收两个元素和一个结果指针
 * @param[in,out] result 存储归约结果的指针
 *
 * @note 如果数组为空，result 不会被修改
 */
void ds_array_reduce(ds_array_s *ar, void (*fn)(void *, void *, void *), void *result)
{
    if (ar->size == 1)
    {
        fn(ar->buffer[0], NULL, result);
        return;
    }

    if (ar->size > 1)
    {
        fn(ar->buffer[0], ar->buffer[1], result);
    }

    for (size_t i = 2; i < ar->size; i++)
    {
        fn(result, ar->buffer[i], result);
    }
}

/**
 * 初始化数组迭代器
 *
 * 将迭代器指向数组的起始位置（索引0）。
 *
 * @param[in,out] iter 要初始化的迭代器指针
 * @param[in]     ar   要遍历的数组指针
 */
void ds_array_iter_init(ds_array_iter_s *iter, ds_array_s *ar)
{
    iter->ar           = ar;
    iter->index        = 0; /* 原代码缺少分号，已修正 */
    iter->last_removed = false;
}

/**
 * 获取迭代器的下一个元素
 *
 * 返回迭代器当前位置的元素，并将迭代器向前移动一位。
 *
 * @param[in,out] iter 迭代器指针
 * @param[out]    out  输出参数，存储获取到的元素指针
 *
 * @return DS_OK 获取成功，DS_ITER_END 已到达数组末尾
 */
ds_status_e ds_array_iter_next(ds_array_iter_s *iter, void **out)
{
    if (iter->index >= iter->ar->size)
    {
        return DS_ITER_END;
    }

    *out = iter->ar->buffer[iter->index];
    iter->index++;
    iter->last_removed = false;
    return DS_OK;
}

/**
 * 移除迭代器上次返回的元素
 *
 * 移除迭代器上一次调用 ds_array_iter_next() 返回的元素。
 * 此操作不会使迭代器失效。
 *
 * @param[in,out] iter 迭代器指针
 * @param[out]    out  输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 移除成功，DS_ERR_VALUE_NOT_FOUND 没有可移除的元素
 *
 * @note 必须在调用 ds_array_iter_next() 之后才能调用此函数，
 *       且同一个元素只能被移除一次
 */
ds_status_e ds_array_iter_remove(ds_array_iter_s *iter, void **out)
{
    ds_status_e status = DS_ERR_VALUE_NOT_FOUND;

    if (iter->last_removed == false)
    {
        status = ds_array_remove_at(iter->ar, iter->index - 1, out);
        if (status == DS_OK)
        {
            iter->last_removed = true;
        }
    }
    return status;
}

/**
 * 在迭代器当前位置插入元素
 *
 * 在迭代器上一次返回的元素之后插入新元素。
 * 新元素将位于迭代器下次返回的元素之前。
 *
 * @param[in,out] iter    迭代器指针
 * @param[in]     element 要插入的元素指针
 *
 * @return DS_OK 插入成功，DS_ERR_OUT_OF_RANGE 索引无效，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @note 函数名 ds_array_iter_all 可能是拼写错误，应为 ds_array_iter_add
 *       此处保留原名，但建议修正为 add
 */
ds_status_e ds_array_iter_all(ds_array_iter_s *iter, void *element)
{
    return ds_array_add_at(iter->ar, element, iter->index++);
}

/**
 * 替换迭代器上次返回的元素
 *
 * 将迭代器上一次调用 ds_array_iter_next() 返回的元素替换为新元素。
 *
 * @param[in,out] iter    迭代器指针
 * @param[in]     element 新元素指针
 * @param[out]    out     输出参数，存储被替换的旧元素指针（可为 NULL）
 *
 * @return DS_OK 替换成功，DS_ERR_OUT_OF_RANGE 没有可替换的元素
 */
ds_status_e ds_array_iter_replace(ds_array_iter_s *iter, void *element, void **out)
{
    return ds_array_replace_at(iter->ar, element, iter->index - 1, out);
}

/**
 * 获取迭代器上次返回元素的索引
 *
 * @param[in] iter 迭代器指针
 *
 * @return 上次返回元素的索引
 *
 * @note 必须在调用 ds_array_iter_next() 之后才能调用此函数，
 *       否则索引值可能无效（当 index = 0 时返回 SIZE_MAX）
 */
size_t ds_array_iter_index(ds_array_iter_s *iter)
{
    return iter->index - 1;
}
