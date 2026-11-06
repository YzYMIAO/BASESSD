#ifndef CONFIG_H
#define CONFIG_H

/**
 * @brief 库版本号
 */
#define VERSION "1.0.0"

#include "ssd_types.h" /* BOOL */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 最大 2 的幂次方值
 *
 * 用于环形缓冲区容量计算，确保容量始终为 2 的幂。
 * 64 位架构下为 2^63，32 位架构下为 2^31。
 */
#ifdef ARCH_64
#    define MAX_POW_TWO (((size_t)1) << 63)
#else
#    define MAX_POW_TWO (((size_t)1) << 31)
#endif /* ARCH_64 */

/**
 * @brief 内联函数宏
 *
 * 根据编译器类型定义不同的内联关键字。
 * - MSVC: 使用 __inline
 * - GCC/Clang: 使用 inline
 */
#if defined(_MSC_VER)

#    define INLINE       __inline
#    define FORCE_INLINE __forceinline

#else

#    define INLINE       inline
#    define FORCE_INLINE inline __attribute__((always_inline))

#endif /* _MSC_VER */

/**
 * @brief 最大元素数量
 *
 * 表示容器可容纳的最大元素个数。
 * 值为 (size_t)-2，即最大的 size_t 值减 1。
 */
#define DS_MAX_ELEMENTS ((size_t)-2)

/**
 * @brief 状态码枚举
 *
 * 所有函数返回的状态码，用于指示操作结果。
 */
typedef enum DS_STATUS_E
{
    DS_OK = 0, /**< 操作成功 */

    DS_ERR_ALLOC            = 1, /**< 内存分配失败 */
    DS_ERR_INVALID_CAPACITY = 2, /**< 无效的容量配置 */
    DS_ERR_INVALID_RANGE    = 3, /**< 无效的范围 */
    DS_ERR_MAX_CAPACITY     = 4, /**< 已达最大容量 */
    DS_ERR_KEY_NOT_FOUND    = 5, /**< 键不存在（用于映射容器） */
    DS_ERR_VALUE_NOT_FOUND  = 6, /**< 值不存在 */
    DS_ERR_OUT_OF_RANGE     = 7, /**< 索引超出范围 */

    DS_ITER_END = 8, /**< 迭代器已到达末尾 */
} ds_status_e;

#endif /* CONFIG_H */
