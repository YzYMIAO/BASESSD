#ifndef MEDIA_LAYOUT_H
#define MEDIA_LAYOUT_H

/*
 * media_layout.h — CW 槽物理布局（FTL / FC 共享的介质契约）
 *
 * CW 槽固定为：
 *
 *   [META:4][CRC:4][USER:512][PARITY:8]   = CW_SIZE(528)
 *
 * 其中 USER 区是主机可见的映射单元（FTL_IU_BYTES），由 FTL 与 FC 共同依赖：
 *   - FC 用它在 desc 链里定位用户数据区、算加扰范围；
 *   - FTL 用它把 LBA 映射到 CW。
 *
 * 因此这两个偏移/长度既不属于 FC 单向 API，也不属于 FTL 私有配置，而是
 * 介质布局契约。放在 common/ 使两侧都不必为了一个常量互相 include。
 *
 * ⚠ 与 CW_SIZE 的自洽由 ftl_config.h 的 _Static_assert 钉住
 *   （USER_SIZE + 2 × USER_OFFSET == CW_SIZE）。
 */

#define MEDIA_CW_USER_OFFSET (8U)   /* USER 区在 CW 槽中的起始偏移 */
#define MEDIA_CW_USER_SIZE   (512U) /* USER 区字节数 = 每 CW 的映射单元 */

#endif /* MEDIA_LAYOUT_H */
