#include <config.h>
#include <ds_stack.h>


#define DEFAULT_CAPACITY           8   /**< 默认初始容量 */
#define DEFAULT_EXPANSION_FACTOR   2   /**< 默认扩容因子 */


/**
 * @brief 栈结构体
 *
 * 使用动态数组实现的后进先出（LIFO）栈。
 * 支持 O(1) 的入栈、出栈和查看栈顶操作。
 * 当栈满时会自动扩容。
 */
struct ds_stack_s
{
    size_t      size;       /**< 当前栈中的元素个数 */
    size_t      capacity;   /**< 当前栈容量 */
    float       exp_factor; /**< 扩容因子 */
    void     **buffer;     /**< 指向存储元素指针的数组缓冲区 */

    void *(*mem_alloc)  (size_t size);          /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);          /**< 内存释放函数指针 */
};


/**
 * @brief 创建一个新的栈（使用默认配置）
 *
 * 使用默认配置创建栈，默认初始容量为 8，扩容因子为 2。
 *
 * @param[out] out 输出参数，存储新创建的栈指针
 *
 * @return DS_OK 创建成功，DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_stack_new(ds_Stack **out)
{
    ds_StackConfig conf;

    ds_stack_conf_init(&conf);
    return ds_stack_new_conf(&conf, out);
}


/**
 * @brief 初始化栈配置结构体为默认值
 *
 * 设置默认容量为 8，默认扩容因子为 2，
 * 使用标准内存分配器 malloc/calloc/free。
 *
 * @param[in,out] conf 需要初始化的配置结构体指针
 */
void ds_stack_conf_init(ds_StackConfig *conf)
{
    conf->capacity   = DEFAULT_CAPACITY;
    conf->exp_factor = DEFAULT_EXPANSION_FACTOR;

    conf->mem_alloc  = malloc;
    conf->mem_calloc = calloc;
    conf->mem_free   = free;
}


/**
 * @brief 根据指定配置创建新的栈
 *
 * 使用配置结构体中的参数创建栈。
 *
 * @param[in]  conf 栈配置结构体指针
 * @param[out] out  输出参数，存储新创建的栈指针
 *
 * @return DS_OK 创建成功，DS_ERR_INVALID_RANGE 容量配置无效，
 *         DS_ERR_ALLOC 内存分配失败
 */
ds_Stat ds_stack_new_conf(ds_StackConfig *conf, ds_Stack **out)
{
    float ex;

    /* 扩容因子必须大于 1 */
    if (conf->exp_factor <= 1) {
        ex = DEFAULT_EXPANSION_FACTOR;
    } else {
        ex = conf->exp_factor;
    }

    /* 检查容量配置是否有效 */
    if (conf->capacity == 0 || ex > DS_MAX_ELEMENTS / conf->capacity) {
        return DS_ERR_INVALID_RANGE;
    }

    ds_Stack *stack = conf->mem_calloc(1, sizeof(ds_Stack));

    if (stack == NULL) {
        return DS_ERR_ALLOC;
    }

    stack->buffer = conf->mem_alloc(conf->capacity * sizeof(void *));

    if (stack->buffer == NULL) {
        conf->mem_free(stack);
        return DS_ERR_ALLOC;
    }

    stack->mem_alloc  = conf->mem_alloc;
    stack->mem_calloc = conf->mem_calloc;
    stack->mem_free   = conf->mem_free;
    stack->capacity   = conf->capacity;
    stack->exp_factor = ex;
    stack->size       = 0;

    *out = stack;

    return DS_OK;
}


/**
 * @brief 扩容栈的容量
 *
 * 按照扩容因子计算新容量并重新分配内存。
 * 如果当前容量已达到 DS_MAX_ELEMENTS，则不再扩容。
 *
 * @param[in,out] stack 需要扩容的栈
 *
 * @return DS_OK 扩容成功，DS_ERR_MAX_CAPACITY 已达最大容量，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @warning 原代码中 memcpy 复制了 stack->capacity 个元素，
 *          但实际应复制 stack->size 个元素，存在潜在越界风险
 */
ds_Stat expand_capacity(ds_Stack *stack)
{
    if (stack->capacity == DS_MAX_ELEMENTS) {
        return DS_ERR_MAX_CAPACITY;
    }

    size_t new_capacity = stack->capacity * stack->exp_factor;

    if (new_capacity < stack->capacity) {
        new_capacity = DS_MAX_ELEMENTS;
    }

    void **new_buffer = stack->mem_alloc(new_capacity * sizeof(void *));

    if (new_buffer == NULL) {
        return DS_ERR_ALLOC;
    }

    /* 注意：应复制 stack->size 个元素，而非 stack->capacity */
    memcpy(new_buffer, stack->buffer, stack->size * sizeof(void *));

    stack->capacity = new_capacity;

    stack->mem_free(stack->buffer);
    stack->buffer = new_buffer;

    return DS_OK;
}


/**
 * @brief 销毁栈（不释放元素数据）
 *
 * 释放栈底层缓冲区和栈结构体本身的内存。
 * 注意：此函数不会释放栈中存储的元素数据。
 *
 * @param[in] stack 要销毁的栈指针
 */
void ds_stack_destroy(ds_Stack *stack)
{
    stack->mem_free(stack->buffer);
    stack->mem_free(stack);
}


/**
 * @brief 销毁栈并释放所有元素数据
 *
 * 遍历栈，对每个元素调用 cb 函数释放其内存，
 * 然后再销毁栈结构体。
 *
 * @param[in] stack 要销毁的栈指针
 * @param[in] cb    释放单个元素数据的回调函数指针
 *
 * @note 如果栈中的元素存储在栈上，不应调用此函数，
 *       应使用 ds_stack_destroy() 代替
 */
void ds_stack_destroy_cb(ds_Stack *stack, void (*cb)(void *))
{
    for (size_t i = 0; i < stack->size; i++) {
        cb(stack->buffer[i]);
    }
    stack->mem_free(stack->buffer);
    stack->mem_free(stack);
}


/**
 * @brief 入栈操作
 *
 * 将元素压入栈顶。如果栈已满，自动扩容。
 *
 * @param[in,out] stack   目标栈
 * @param[in]     element 要入栈的元素指针
 *
 * @return DS_OK 入栈成功，DS_ERR_ALLOC 扩容失败，
 *         DS_ERR_MAX_CAPACITY 已达最大容量
 */
ds_Stat ds_stack_push(ds_Stack *stack, void *element)
{
    if (stack->size >= stack->capacity) {
        ds_Stat status = expand_capacity(stack);

        if (status != DS_OK) {
            return status;
        }
    }

    stack->buffer[stack->size] = element;
    stack->size++;
    return DS_OK;
}


/**
 * @brief 查看栈顶元素（不移除）
 *
 * 返回栈顶元素的指针，但不将其从栈中移除。
 *
 * @param[in]  stack 目标栈
 * @param[out] out   输出参数，存储栈顶元素指针
 *
 * @return DS_OK 查看成功，DS_ERR_OUT_OF_RANGE 栈为空
 */
ds_Stat ds_stack_peek(ds_Stack *stack, void **out)
{
    if (stack->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = stack->buffer[stack->size - 1];

    *out = data;
    return DS_OK;
}


/**
 * @brief 出栈操作
 *
 * 移除并返回栈顶元素。
 *
 * @param[in,out] stack 目标栈
 * @param[out]    out   输出参数，存储被移除的元素指针（可为 NULL）
 *
 * @return DS_OK 出栈成功，DS_ERR_OUT_OF_RANGE 栈为空
 */
ds_Stat ds_stack_pop(ds_Stack *stack, void **out)
{
    if (stack->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    void *data = stack->buffer[stack->size - 1];

    stack->size--;
    *out = data;
    return DS_OK;
}


/**
 * @brief 获取栈的元素个数
 *
 * @param[in] stack 目标栈
 *
 * @return 栈中元素的个数
 */
size_t ds_stack_size(ds_Stack *stack)
{
    return stack->size;
}


/**
 * @brief 对栈中的每个元素执行操作
 *
 * 从栈底到栈顶遍历，对每个元素调用 fn 函数。
 *
 * @param[in,out] stack 目标栈
 * @param[in]     fn    对每个元素执行的函数
 */
void ds_stack_map(ds_Stack *stack, void (*fn)(void *))
{
    for (size_t i = 0; i < stack->size; i++) {
        fn(stack->buffer[i]);
    }
}


/**
 * @brief 就地过滤栈（修改原栈）
 *
 * 移除栈中所有不满足谓词函数 pred 的元素。
 *
 * @param[in,out] stack 目标栈
 * @param[in]     pred  谓词函数，返回 true 表示保留元素，false 表示移除
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 栈为空
 *
 * @note 此函数从栈顶向栈底遍历，通过 memmove 移动元素来保持顺序。
 *       时间复杂度 O(n)。
 *
 * @warning 原代码中 memmove 的地址计算存在潜在错误，
 *          在边界情况下可能导致访问越界
 */
ds_Stat ds_stack_filter_mut(ds_Stack *stack, bool (*pred)(void *))
{
    if (stack->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    size_t keep = 0;
    size_t rm   = 0;
    size_t i    = 0;

    /* 从栈顶向栈底遍历 */
    for (i = stack->size - 1; i != ((size_t)-1); i--) {
        if (pred(stack->buffer[i]) == false) {
            rm++;
            continue;
        }

        if (rm > 0) {
            /* 将需要保留的元素移动到正确位置 */
            size_t block_size = keep * sizeof(void *);
            memmove(&(stack->buffer[i + 1]), &(stack->buffer[i + rm + 1]), block_size);
            stack->size -= rm;
            rm = 0;
        }

        keep++;
    }

    /* 处理栈顶剩余需要移除的元素 */
    if (rm > 0) {
        size_t block_size = keep * sizeof(void *);
        memmove(&(stack->buffer[i + 1]), &(stack->buffer[i + rm + 1]), block_size);
        stack->size -= rm;
    }

    return DS_OK;
}


/**
 * @brief 过滤栈并生成新栈（不修改原栈）
 *
 * 遍历原栈，将所有满足谓词函数 pred 的元素复制到新栈中。
 *
 * @param[in]  stack 原栈
 * @param[in]  pred  谓词函数，返回 true 表示保留该元素
 * @param[out] out   输出参数，存储新创建的过滤后栈指针
 *
 * @return DS_OK 过滤成功，DS_ERR_OUT_OF_RANGE 栈为空，
 *         DS_ERR_ALLOC 内存分配失败
 *
 * @note 函数参数类型 int(*pred)(void*) 与 bool(*pred)(void*) 不一致，建议统一
 */
ds_Stat ds_stack_filter(ds_Stack *stack, int (*pred)(void *), ds_Stack **out)
{
    if (stack->size == 0) {
        return DS_ERR_OUT_OF_RANGE;
    }

    ds_Stack *filtered = stack->mem_calloc(1, sizeof(ds_Stack));

    if (filtered == NULL) {
        return DS_ERR_ALLOC;
    }

    filtered->buffer = stack->mem_alloc(stack->capacity * sizeof(void *));

    if (filtered->buffer == NULL) {
        stack->mem_free(filtered);
        return DS_ERR_ALLOC;
    }

    filtered->capacity   = stack->capacity;
    filtered->exp_factor = stack->exp_factor;
    filtered->mem_alloc  = stack->mem_alloc;
    filtered->mem_calloc = stack->mem_calloc;
    filtered->mem_free   = stack->mem_free;
    filtered->size       = 0;

    for (size_t i = 0; i < stack->size; i++) {
        if (pred(stack->buffer[i]) == true) {
            filtered->buffer[filtered->size++] = stack->buffer[i];
        }
    }

    *out = filtered;
    return DS_OK;
}
