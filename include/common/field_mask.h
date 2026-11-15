#ifndef FIELD_MASK_H
#define FIELD_MASK_H

#include "ssd_types.h"

/**
 * @brief 构造低 @p width 位全 1 的掩码。
 *
 * @param width 位宽；0 返回 0，调用者须保证 width < 32。
 * @return 掩码。
 */
#define SSD_FIELD_MASK(width) (((width) == 0U) ? 0UL : ((1UL << (width)) - 1UL))

#endif /* FIELD_MASK_H */
