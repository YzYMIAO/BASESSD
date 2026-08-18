/*
 * ============================================================
 *  NAND 加载器 + TCM 初始化
 *  仅在启动阶段使用，在 ITCM 中执行
 *  配套启动文件：startup_ssd.s
 * ============================================================
 */
#include <common.h>

/* ============================================================
 *  段属性宏（与链接脚本对应）
 * ============================================================ */
#define ITCM_CODE __attribute__((section(".itcm_text")))

/* ============================================================
 *  NAND 控制寄存器（示例地址，根据实际芯片修改）
 * ============================================================ */
#define NAND_CTRL (*(volatile uint32_t *)0x80000000)
#define NAND_ADDR (*(volatile uint32_t *)0x80000004)
#define NAND_DATA (*(volatile uint32_t *)0x80000008)
#define NAND_STAT (*(volatile uint32_t *)0x8000000C)

#define NAND_CMD_READ    0x01
#define NAND_CMD_READ_ID 0x02
#define NAND_STAT_READY  (1 << 0)

/* ============================================================
 *  TCM 控制寄存器（Cortex-M7 示例）
 * ============================================================ */
#define SCB_BASE   0xE000E000
#define SCB_ITCMCR (*(volatile uint32_t *)(SCB_BASE + 0x0F00))
#define SCB_DTCMCR (*(volatile uint32_t *)(SCB_BASE + 0x0F04))

#define SCB_ITCMCR_EN (1 << 0)
#define SCB_DTCMCR_EN (1 << 0)

/* ============================================================
 *  TCM 初始化
 *  在 ITCM 中执行，由 Reset_Handler 调用
 * ============================================================ */
ITCM_CODE void TCM_Init(void)
{
    /* 使能 ITCM */
    SCB_ITCMCR = SCB_ITCMCR_EN;

    /* 使能 DTCM */
    SCB_DTCMCR = SCB_DTCMCR_EN;

    /* 内存屏障 */
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

/* ============================================================
 *  NAND 控制器低层初始化
 *  在 ITCM 中执行，由 Reset_Handler 调用
 * ============================================================ */
ITCM_CODE void NAND_Init_LowLevel(void)
{
    /* 配置 NAND 控制器时钟 */
    /* 配置 NAND 时序参数 */
    /* 使能 NAND 控制器 */

    /* 读取 NAND ID 验证（示例：W25N01GV） */
    NAND_CTRL    = NAND_CMD_READ_ID;
    uint32_t id1 = NAND_DATA;
    uint32_t id2 = NAND_DATA;
    uint32_t id3 = NAND_DATA;
    uint32_t id4 = NAND_DATA;

    /* 验证 ID，失败则死循环 */
    if (id1 != 0xEF || id2 != 0xAA || id3 != 0x21 || id4 != 0x15)
    {
        while (1)
        { /* 死循环等待看门狗复位 */
        }
    }
}

/* ============================================================
 *  从 NAND Flash 加载固件到 DRAM
 *  在 ITCM 中执行，由 Reset_Handler 调用
 *
 *  参数：
 *    dst  - DRAM 目标地址（VMA）
 *    src  - NAND 源地址（LMA）
 *    size - 固件大小（字节）
 *
 *  返回值：
 *    0    - 成功
 *    -1   - 失败
 * ============================================================ */
ITCM_CODE int NAND_Load_Firmware(uint32_t dst, uint32_t src, uint32_t size)
{
    uint32_t *dst_ptr = (uint32_t *)dst;
    uint32_t  words   = size / 4;
    uint32_t  i;

    if (dst_ptr == NULL || size == 0)
    {
        return -1;
    }

    /* 发送读取命令 */
    NAND_CTRL = NAND_CMD_READ;
    NAND_ADDR = src;

    /* 等待 NAND 就绪 */
    while (!(NAND_STAT & NAND_STAT_READY))
    {
        /* 等待 */
    }

    /* 读取数据 */
    for (i = 0; i < words; i++)
    {
        dst_ptr[i] = NAND_DATA;
    }

    return 0;
}