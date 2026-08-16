#ifndef DS_STACK_H_
#define DS_STACK_H_

/**
 * @brief 栈结构体（不透明指针）
 *
 * 使用动态数组实现的后进先出（LIFO）栈。
 */
typedef struct ds_stack_s ds_Stack;


/**
 * @brief 栈配置结构体
 */
typedef struct ds_stack_config_s
{
    size_t capacity;    /**< 初始容量 */
    float  exp_factor;  /**< 扩容因子 */

    void *(*mem_alloc)  (size_t size);          /**< 内存分配函数指针 */
    void *(*mem_calloc) (size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void  (*mem_free)   (void *block);          /**< 内存释放函数指针 */
} ds_StackConfig;


/* 生命周期管理 */
ds_Stat ds_stack_new        (ds_Stack **out);
void    ds_stack_conf_init  (ds_StackConfig *conf);
ds_Stat ds_stack_new_conf   (ds_StackConfig *conf, ds_Stack **out);
ds_Stat expand_capacity     (ds_Stack *stack);
void    ds_stack_destory    (ds_Stack *stack);
void    ds_stack_destory_cb (ds_Stack *stack, void (*cb)(void *));

/* 核心操作 */
ds_Stat ds_stack_push       (ds_Stack *stack, void *element);
ds_Stat ds_stack_peek       (ds_Stack *stack, void **out);
ds_Stat ds_stack_pop        (ds_Stack *stack, void **out);

/* 查询和遍历 */
size_t  ds_stack_size       (ds_Stack *stack);
void    ds_stack_map        (ds_Stack *stack, void (*fn)(void *));

/* 过滤 */
ds_Stat ds_stack_filter_mut (ds_Stack *stack, bool (*pred)(void *));
ds_Stat ds_stack_filter     (ds_Stack *stack, int (*pred)(void *), ds_Stack **out);

#endif /* DS_STACK_H_ */
