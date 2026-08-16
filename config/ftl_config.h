#ifndef SSD_CONFIG_FTL_H
#define SSD_CONFIG_FTL_H

/* 空闲块低于该比例时触发垃圾回收。 */
#define SSD_FTL_GC_LOW_WATERMARK_PERCENT  10U

/* 初期使用页级映射，一个 LBA 对应一个 NAND 页。 */
#define SSD_FTL_LBA_SIZE                  4096U

#endif /* SSD_CONFIG_FTL_H */
