#ifndef DS_STACK_H_
#define DS_STACK_H_

#include "config.h"

/**
 * @brief 栈结构体（不透明指针）
 *
 * 使用动态数组实现的后进先出（LIFO）栈。
 */
typedef struct DS_STACK_S ds_stack_s;

/**
 * @brief 栈配置结构体
 */
typedef struct DS_STACK_CONFIG_S
{
    size_t capacity;   /**< 初始容量 */
    float  exp_factor; /**< 扩容因子 */

    void *(*mem_alloc)(size_t size);                 /**< 内存分配函数指针 */
    void *(*mem_calloc)(size_t blocks, size_t size); /**< 内存分配并清零函数指针 */
    void (*mem_free)(void *block);                   /**< 内存释放函数指针 */
} ds_stack_cfg_s;

/* 生命周期管理 */
ds_status_e ds_stack_new(ds_stack_s **out);
void        ds_stack_conf_init(ds_stack_cfg_s *conf);
ds_status_e ds_stack_new_conf(ds_stack_cfg_s *conf, ds_stack_s **out);
ds_status_e expand_capacity(ds_stack_s *stack);
void        ds_stack_destory(ds_stack_s *stack);
void        ds_stack_destory_cb(ds_stack_s *stack, void (*cb)(void *));

/* 核心操作 */
ds_status_e ds_stack_push(ds_stack_s *stack, void *element);
ds_status_e ds_stack_peek(ds_stack_s *stack, void **out);
ds_status_e ds_stack_pop(ds_stack_s *stack, void **out);

/* 查询和遍历 */
size_t ds_stack_size(ds_stack_s *stack);
void   ds_stack_map(ds_stack_s *stack, void (*fn)(void *));

/* 过滤 */
ds_status_e ds_stack_filter_mut(ds_stack_s *stack, BOOL (*pred)(void *));
ds_status_e ds_stack_filter(ds_stack_s *stack, int (*pred)(void *), ds_stack_s **out);

#endif /* DS_STACK_H_ */
