#ifndef SSD_CONFIG_NAND_GEOMETRY_H
#define SSD_CONFIG_NAND_GEOMETRY_H

/* 小容量配置便于单元测试，后续可以通过构建变量切换更大模型。 */
#define SSD_NAND_CHANNEL_COUNT       2U
#define SSD_NAND_CHIPS_PER_CHANNEL   1U
#define SSD_NAND_DIES_PER_CHIP       1U
#define SSD_NAND_PLANES_PER_DIE      2U
#define SSD_NAND_BLOCKS_PER_PLANE    64U
#define SSD_NAND_PAGES_PER_BLOCK     64U
#define SSD_NAND_PAGE_SIZE           4096U
#define SSD_NAND_OOB_SIZE            128U

#endif /* SSD_CONFIG_NAND_GEOMETRY_H */
