#ifndef FIELD_MASK_H
#define FIELD_MASK_H

/*
 * field_mask.h — 位掩码构造与“任一位置位”判据
 *
 * SSD_FIELD_MASK 构造低 width 位全 1；SSD_FLAGS_ANY 判断 value 中是否有
 * 任一 mask 位被置位。两者只用于无符号整数。
 */

#include "ssd_types.h"

/**
 * @brief 构造低 @p width 位全 1 的掩码。
 *
 * @param width 位宽；0 返回 0，调用者须保证 width < 32。
 * @return 掩码。
 */
#define SSD_FIELD_MASK(width) (((width) == 0U) ? 0UL : ((1UL << (width)) - 1UL))

/**
 * @brief @p value 中是否有任一 @p mask 位置位。
 *
 * @param value 被检查的值
 * @param mask  位掩码
 * @return true = 至少一位置位。
 */
#define SSD_FLAGS_ANY(value, mask) (((value) & (mask)) != 0U)

#endif /* FIELD_MASK_H */
