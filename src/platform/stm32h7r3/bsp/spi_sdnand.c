

#include "spi_sdnand.h"
#include "delay.h"
#include "usart.h"

/* SD NAND命令定义 */
#define SDNAND_CMD0     ((uint8_t)0)
#define SDNAND_CMD8     ((uint8_t)8)
#define SDNAND_CMD9     ((uint8_t)9)
#define SDNAND_CMD12    ((uint8_t)12)
#define SDNAND_CMD16    ((uint8_t)16)
#define SDNAND_CMD17    ((uint8_t)17)
#define SDNAND_CMD18    ((uint8_t)18)
#define SDNAND_CMD24    ((uint8_t)24)
#define SDNAND_CMD25    ((uint8_t)25)
#define SDNAND_CMD55    ((uint8_t)55)
#define SDNAND_CMD58    ((uint8_t)58)
#define SDNAND_ACMD23   ((uint8_t)23)
#define SDNAND_ACMD41   ((uint8_t)41)

/* SPI mode initialization arguments and response bits. */
#define SDNAND_ACMD41_HCS               0x40000000UL
#define SDNAND_R1_IDLE_STATE             0x01U
#define SDNAND_R1_ILLEGAL_COMMAND        0x04U
#define SDNAND_OCR_CCS                   0x40U
#define SDNAND_CMD0_CRC_BYTE             0x95U
#define SDNAND_CMD8_CRC_BYTE             0x87U
#define SDNAND_CMD8_ARGUMENT             0x000001AAUL
#define SDNAND_DATA_TOKEN_SINGLE         0xFEU
#define SDNAND_DATA_TOKEN_MULTI_WRITE    0xFCU
#define SDNAND_DATA_TOKEN_STOP_WRITE     0xFDU

/* SD NAND信息定义 */
sdnand_info_t sdnand_info = {0};

/* SPI句柄定义 */
static SPI_HandleTypeDef spi_handle = {0};

/* SD NAND SPI速度模式定义 */
typedef enum {
    SDNAND_SPI_SPEED_LOW = 0,
    SDNAND_SPI_SPEED_HIGH,
    SDNAND_SPI_SPEED_DUMMY,
} sdnand_spi_speed_t;

/**
 * @brief   初始化SPI接口
 * @param   speed: SD NAND SPI速度模式（参考sdnand_spi_speed_t定义）
 * @retval  初始化结果
 * @arg     0: 初始化成功
 * @arg     1: 初始化失败
 */
static uint8_t sdnand_spi_init(sdnand_spi_speed_t speed)
{
    __IO uint8_t *ptr;
    uint32_t i;
    
    if (speed >= SDNAND_SPI_SPEED_DUMMY)
    {
        return 1;
    }
    
    ptr = (__IO uint8_t *)&spi_handle;
    for (i = 0; i < sizeof(SPI_HandleTypeDef); i++)
    {
        *ptr++ = 0x00;
    }
    
    /* 反初始化SPI */
    spi_handle.Instance = SDNAND_SPI;
    if (HAL_SPI_DeInit(&spi_handle) != HAL_OK)
    {
        return 1;
    }
    
    /* 初始化SPI */
    spi_handle.Instance = SDNAND_SPI;
    spi_handle.Init.Mode = SPI_MODE_MASTER;
    spi_handle.Init.Direction = SPI_DIRECTION_2LINES;
    spi_handle.Init.DataSize = SPI_DATASIZE_8BIT;
    spi_handle.Init.CLKPolarity = SPI_POLARITY_HIGH;
    spi_handle.Init.CLKPhase = SPI_PHASE_2EDGE;
    spi_handle.Init.NSS = SPI_NSS_SOFT;
    if (speed == SDNAND_SPI_SPEED_LOW)
    {
        spi_handle.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
    }
    else
    {
        spi_handle.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    }
    spi_handle.Init.FirstBit = SPI_FIRSTBIT_MSB;
    spi_handle.Init.TIMode = SPI_TIMODE_DISABLE;
    spi_handle.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    spi_handle.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    spi_handle.Init.NSSPolarity = SPI_NSS_POLARITY_LOW;
    spi_handle.Init.FifoThreshold = SPI_FIFO_THRESHOLD_01DATA;
    spi_handle.Init.MasterSSIdleness = SPI_MASTER_SS_IDLENESS_00CYCLE;
    spi_handle.Init.MasterInterDataIdleness = SPI_MASTER_INTERDATA_IDLENESS_00CYCLE;
    spi_handle.Init.MasterReceiverAutoSusp = SPI_MASTER_RX_AUTOSUSP_DISABLE;
    spi_handle.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;
    spi_handle.Init.IOSwap = SPI_IO_SWAP_DISABLE;
    spi_handle.Init.ReadyMasterManagement = SPI_RDY_MASTER_MANAGEMENT_INTERNALLY;
    spi_handle.Init.ReadyPolarity = SPI_RDY_POLARITY_HIGH;
    if (HAL_SPI_Init(&spi_handle) != HAL_OK)
    {
        return 1;
    }
    
    return 0;
}

/**
 * @brief   HAL库SPI初始化回调函数
 * @param   hspi: SPI句柄指针
 * @retval  无
 */
void HAL_SPI_MspInit(SPI_HandleTypeDef* hspi)
{
    RCC_PeriphCLKInitTypeDef rcc_periph_clk_init_struct = {0};
    GPIO_InitTypeDef gpio_init_struct = {0};
    
    if (hspi->Instance == SDNAND_SPI)
    {
        /* 配置时钟源 */
        rcc_periph_clk_init_struct.PeriphClockSelection = RCC_PERIPHCLK_SPI45;
        rcc_periph_clk_init_struct.Xspi1ClockSelection = RCC_SPI45CLKSOURCE_PLL2Q;
        HAL_RCCEx_PeriphCLKConfig(&rcc_periph_clk_init_struct);
        
        /* 使能时钟 */
        SDNAND_SPI_CLK_ENABLE();
        SDNAND_SPI_SCK_GPIO_CLK_ENABLE();
        SDNAND_SPI_MOSI_GPIO_CLK_ENABLE();
        SDNAND_SPI_MISO_GPIO_CLK_ENABLE();
        
        /* 初始化通讯引脚 */
        gpio_init_struct.Pin = SDNAND_SPI_SCK_GPIO_PIN;
        gpio_init_struct.Mode = GPIO_MODE_AF_PP;
        gpio_init_struct.Pull = GPIO_PULLUP;
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
        gpio_init_struct.Alternate = SDNAND_SPI_SCK_GPIO_AF;
        HAL_GPIO_Init(SDNAND_SPI_SCK_GPIO_PORT, &gpio_init_struct);
        gpio_init_struct.Pin = SDNAND_SPI_MOSI_GPIO_PIN;
        gpio_init_struct.Mode = GPIO_MODE_AF_PP;
        gpio_init_struct.Pull = GPIO_PULLUP;
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
        gpio_init_struct.Alternate = SDNAND_SPI_MOSI_GPIO_AF;
        HAL_GPIO_Init(SDNAND_SPI_MOSI_GPIO_PORT, &gpio_init_struct);
        gpio_init_struct.Pin = SDNAND_SPI_MISO_GPIO_PIN;
        gpio_init_struct.Mode = GPIO_MODE_AF_PP;
        gpio_init_struct.Pull = GPIO_PULLUP;
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
        gpio_init_struct.Alternate = SDNAND_SPI_MISO_GPIO_AF;
        HAL_GPIO_Init(SDNAND_SPI_MISO_GPIO_PORT, &gpio_init_struct);
    }
}

/**
 * @brief   HAL库SPI反初始化回调函数
 * @param   hxspi: SPI句柄指针
 * @retval  无
 */
void HAL_SPI_MspDeInit(SPI_HandleTypeDef* hspi)
{
    if (hspi->Instance == SDNAND_SPI)
    {
        /* 禁用时钟 */
        SDNAND_SPI_CLK_DISABLE();
        
        /* 反初始化通讯引脚 */
        HAL_GPIO_DeInit(SDNAND_SPI_SCK_GPIO_PORT, SDNAND_SPI_SCK_GPIO_PIN);
        HAL_GPIO_DeInit(SDNAND_SPI_MOSI_GPIO_PORT, SDNAND_SPI_MOSI_GPIO_PIN);
        HAL_GPIO_DeInit(SDNAND_SPI_MISO_GPIO_PORT, SDNAND_SPI_MISO_GPIO_PIN);
    }
}

/**
 * @brief   SD NAND SPI发送并接收1字节数据
 * @param   txdata: 发送数据
 * @retval  接收数据
 */
static uint8_t sdnand_spi_read_write_byte(uint8_t txdata)
{
    uint8_t rxdata;
    
    HAL_SPI_TransmitReceive(&spi_handle, &txdata, &rxdata, 1, HAL_MAX_DELAY);
    
    return rxdata;
}

static void sdnand_read_response_bytes(uint8_t *response, uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; index++)
        response[index] = sdnand_spi_read_write_byte(0xFFU);
}

/**
 * @brief   等待SD NAND就绪
 * @param   无
 * @retval  等待结果
 * @arg     0: SD NAND就绪
 * @arg     1: 等待超时
 */
static uint8_t sdnand_wait_ready(void)
{
    uint32_t t;
    
    t = 0xFFFFFF;
    while (t--)
    {
        if (sdnand_spi_read_write_byte(0xFF) == 0xFF)
        {
            return 0;
        }
    }
    
    return 1;
}

/**
 * @brief   初始化片选信号
 * @param   无
 * @retval  无
 */
static void sdnand_chip_select_init(void)
{
    GPIO_InitTypeDef gpio_init_struct = {0};
    
    /* 使能时钟 */
    SDNAND_CS_GPIO_CLK_ENABLE();
    
    /* 配置引脚默认输出 */
    SDNAND_CS(1);
    
    /* 初始化引脚 */
    gpio_init_struct.Pin = SDNAND_CS_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(SDNAND_CS_GPIO_PORT, &gpio_init_struct);
}

/**
 * @brief   取消片选信号
 * @param   无
 * @retval  无
 */
static void sdnand_chip_select_disable(void)
{
    SDNAND_CS(1);
    sdnand_spi_read_write_byte(0xFF);
}

/**
 * @brief   使能片选信号
 * @param   无
 * @retval  使能结果
 * @arg     0: 使能成功
 * @arg     1: 使能失败
 */
static uint8_t sdnand_chip_select_enable(void)
{
    SDNAND_CS(0);
    if (sdnand_wait_ready() == 0)
    {
        return 0;
    }
    
    sdnand_chip_select_disable();
    return 1;
}

/**
 * @brief   SD NAND接收数据
 * @param   data: 数据缓冲区指针
 * @param   length: 数据长度
 * @retval  数据接收结果
 * @arg     0: 接收成功
 * @arg     1: 接收失败
 */
static uint8_t sdnand_recv_data(uint8_t *data, uint16_t length)
{
    uint16_t t;
    
    /* 等待SD NAND响应 */
    t = UINT16_MAX;
    while (t--)
    {
        if (sdnand_spi_read_write_byte(0xFF) == SDNAND_DATA_TOKEN_SINGLE)
        {
            break;
        }
    }
    if (t == 0)
    {
        return 1;
    }
    
    if (length == 0)
    {
        return 0;
    }
    
    if (data == NULL)
    {
        return 1;
    }
    
    /* 接收数据 */
    while (length--)
    {
        *data = sdnand_spi_read_write_byte(0xFF);
        data++;
    }
    
    /* 接收CRC */
    sdnand_spi_read_write_byte(0xFF);
    sdnand_spi_read_write_byte(0xFF);
    
    return 0;
}

/**
 * @brief   SD NAND发送块数据
 * @param   arg: 参数
 * @param   data: 数据缓冲区指针
 * @retval  块数据发送结果
 * @arg     0: 发送成功
 * @arg     1: 发送失败
 */
static uint8_t sdnand_send_block_data(uint8_t token, const uint8_t *data)
{
    uint32_t data_index;
    
    /* 校验SD NAND就绪 */
    if (sdnand_wait_ready() != 0)
    {
        return 1;
    }
    
    /* 发送参数 */
    sdnand_spi_read_write_byte(token);

    if (token != SDNAND_DATA_TOKEN_STOP_WRITE)
    {
        if (data == NULL)
            return 1;
        for (data_index = 0; data_index < sdnand_info.logic_block_size; data_index++)
        {
            sdnand_spi_read_write_byte(data[data_index]);
        }
        
        /* 发送CRC */
        sdnand_spi_read_write_byte(0xFF);
        sdnand_spi_read_write_byte(0xFF);
        
        if ((sdnand_spi_read_write_byte(0xFF) & 0x1F) != 0x05)
        {
            return 1;
        }
    }
    
    return 0;
}

/**
 * @brief   SD NAND发送命令
 * @param   cmd: 命令
 * @param   arg: 参数
 * @retval  SD NAND响应
 * @arg     UINT8_MAX: 命令发送失败
 * @arg     其他: SD NAND响应
 */
static uint8_t sdnand_send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t crc_byte;
    uint8_t t;
    uint8_t ret;
    
    /* 使能片选信号 */
    if (cmd != SDNAND_CMD12)
    {
        sdnand_chip_select_disable();
        if (sdnand_chip_select_enable() != 0)
        {
            return UINT8_MAX;
        }
    }
    
    /* CMD0 and CMD8 require a valid CRC before CRC checking is disabled. */
    switch (cmd)
    {
        case SDNAND_CMD0:
        {
            crc_byte = SDNAND_CMD0_CRC_BYTE;
            break;
        }
        case SDNAND_CMD8:
        {
            crc_byte = SDNAND_CMD8_CRC_BYTE;
            break;
        }
        default:
        {
            crc_byte = 0x01U;
            break;
        }
    }
    
    /* 发送命令数据包 */
    sdnand_spi_read_write_byte(0x40 | cmd);
    sdnand_spi_read_write_byte((uint8_t)((arg >> 24) & 0xFF));
    sdnand_spi_read_write_byte((uint8_t)((arg >> 16) & 0xFF));
    sdnand_spi_read_write_byte((uint8_t)((arg >> 8) & 0xFF));
    sdnand_spi_read_write_byte((uint8_t)((arg >> 0) & 0xFF));
    sdnand_spi_read_write_byte(crc_byte);
    
    if (cmd == SDNAND_CMD12)
    {
        sdnand_spi_read_write_byte(0xFF);
    }
    
    /* 获取响应 */
    ret = 0xFFU;
    t = 10U;
    while (t--)
    {
        ret = sdnand_spi_read_write_byte(0xFF);
        if ((ret & 0x80) != 0x80)
        {
            break;
        }
    }
    
    return ret;
}

static uint8_t sdnand_send_app_cmd(uint8_t acmd, uint32_t arg)
{
    uint8_t response = sdnand_send_cmd(SDNAND_CMD55, 0UL);

    if ((response != 0x00U) && (response != SDNAND_R1_IDLE_STATE))
        return response;

    return sdnand_send_cmd(acmd, arg);
}

static uint8_t sdnand_read_ocr(uint8_t ocr[4])
{
    if (sdnand_send_cmd(SDNAND_CMD58, 0UL) != 0x00U)
        return 1U;

    sdnand_read_response_bytes(ocr, 4U);
    sdnand_chip_select_disable();
    return 0U;
}

static uint8_t sdnand_parse_csd(void)
{
    const uint8_t *csd = sdnand_info.csd;

    if ((csd[0] >> 6U) == 1U)
    {
        /* CSD version 2.0: capacity = (C_SIZE + 1) * 512 KiB. */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3FU) << 16U) |
                          ((uint32_t)csd[8] << 8U) | csd[9];

        sdnand_info.block_size = SDNAND_BLOCK_SIZE;
        sdnand_info.logic_block_num = (c_size + 1U) * 1024U;
        sdnand_info.block_num = sdnand_info.logic_block_num;
        sdnand_info.chip_size = sdnand_info.logic_block_num * SDNAND_BLOCK_SIZE;
    }
    else if ((csd[0] >> 6U) == 0U)
    {
        uint32_t read_bl_len = csd[5] & 0x0FU;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03U) << 10U) |
                          ((uint32_t)csd[7] << 2U) | ((csd[8] & 0xC0U) >> 6U);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03U) << 1U) |
                               ((csd[10] & 0x80U) >> 7U);

        if (read_bl_len > 11U)
            return 1U;

        sdnand_info.block_size = 1UL << read_bl_len;
        sdnand_info.block_num = (c_size + 1U) << (c_size_mult + 2U);
        sdnand_info.chip_size = sdnand_info.block_num * sdnand_info.block_size;
        sdnand_info.logic_block_num = sdnand_info.chip_size / SDNAND_BLOCK_SIZE;
    }
    else
    {
        return 1U;
    }

    sdnand_info.logic_block_size = SDNAND_BLOCK_SIZE;
    return (sdnand_info.logic_block_num == 0U) ? 1U : 0U;
}

static uint32_t sdnand_lba_to_command_address(uint32_t lba)
{
    return (sdnand_info.card_type == SDNAND_CARD_SDHC_OR_SDXC) ?
           lba : (lba * SDNAND_BLOCK_SIZE);
}

/**
 * @brief   初始化SD NAND
 * @param   无
 * @retval  初始化结果
 * @arg     0: 初始化成功
 * @arg     1: 初始化失败
 */
uint8_t sdnand_init(void)
{
    uint8_t index;
    uint16_t t;
    uint8_t resp7[4];
    uint8_t response;
    uint8_t ready;
    uint32_t acmd41_arg;
    
    /* 初始化片选信号 */
    sdnand_chip_select_init();
    
    /* SD NAND初始化前低速模式初始化SPI */
    if (sdnand_spi_init(SDNAND_SPI_SPEED_LOW) != 0U)
        return 1U;
    
    /* 发送至少74个时钟以上电SD */
    for (index = 0; index < ((74 + 7) / 8); index++)
    {
       sdnand_spi_read_write_byte(0xFF);
    }
    
    /* 进入IDLE状态 */
    ready = 0U;
    t = 20U;
    while (t--)
    {
        if (sdnand_send_cmd(SDNAND_CMD0, 0UL) == SDNAND_R1_IDLE_STATE)
        {
            ready = 1U;
            break;
        }
    }
    if (ready == 0U)
    {
        sdnand_chip_select_disable();
        return 1;
    }
    
    /* CMD8 distinguishes legacy SDSC cards from SD v2 cards. */
    response = sdnand_send_cmd(SDNAND_CMD8, SDNAND_CMD8_ARGUMENT);
    if (response == SDNAND_R1_IDLE_STATE)
    {
        sdnand_read_response_bytes(resp7, sizeof(resp7));
        sdnand_chip_select_disable();
        if ((resp7[2] != 0x01U) || (resp7[3] != 0xAAU))
            return 1U;

        sdnand_info.card_type = SDNAND_CARD_SDSC_V2;
        acmd41_arg = SDNAND_ACMD41_HCS;
    }
    else if ((response & SDNAND_R1_ILLEGAL_COMMAND) != 0U)
    {
        sdnand_chip_select_disable();
        sdnand_info.card_type = SDNAND_CARD_SDSC_V1;
        acmd41_arg = 0UL;
    }
    else
    {
        sdnand_chip_select_disable();
        return 1U;
    }

    /* ACMD41 remains in Idle until the card finishes its internal startup. */
    ready = 0U;
    t = 20U;
    while (t--)
    {
        if (sdnand_send_app_cmd(SDNAND_ACMD41, acmd41_arg) == 0x00U)
        {
            ready = 1U;
            break;
        }
        sdnand_chip_select_disable();
        HAL_Delay(10U);
    }
    if (ready == 0U)
    {
        sdnand_chip_select_disable();
        return 1;
    }
    
    if (sdnand_read_ocr(sdnand_info.ocr) != 0U)
        return 1U;

    if ((sdnand_info.card_type == SDNAND_CARD_SDSC_V2) &&
        ((sdnand_info.ocr[0] & SDNAND_OCR_CCS) != 0U))
        sdnand_info.card_type = SDNAND_CARD_SDHC_OR_SDXC;

    /* SD NAND初始化后高速模式初始化SPI */
    if (sdnand_spi_init(SDNAND_SPI_SPEED_HIGH) != 0U)
        return 1U;
    
    /* 设置块大小 */
    if ((sdnand_info.card_type != SDNAND_CARD_SDHC_OR_SDXC) &&
        (sdnand_send_cmd(SDNAND_CMD16, SDNAND_BLOCK_SIZE) != 0x00U))
    {
        sdnand_chip_select_disable();
        return 1;
    }
    sdnand_chip_select_disable();
    
    /* 读取CSD */
    if (sdnand_send_cmd(SDNAND_CMD9, 0UL) != 0x00)
    {
        sdnand_chip_select_disable();
        return 1;
    }
    if (sdnand_recv_data(sdnand_info.csd, sizeof(sdnand_info.csd)) != 0)
    {
        sdnand_chip_select_disable();
        return 1;
    }
    sdnand_chip_select_disable();
    
    return sdnand_parse_csd();
}

/**
 * @brief   读SD NAND指定数量块的数据
 * @param   data: 数据缓冲区指针
 * @param   block_index: 起始块
 * @param   block_num: 块数量
 * @retval  读取结果
 * @arg     0: 读取成功
 * @arg     1: 读取失败
 */
uint8_t sdnand_read_disk(uint8_t *data, uint32_t block_index, uint32_t block_num)
{
    uint32_t address;
    
    if (block_num == 0)
    {
        return 0;
    }
    
    if (data == NULL)
    {
        return 1;
    }
    
    if ((block_index >= sdnand_info.logic_block_num) ||
        (block_num > (sdnand_info.logic_block_num - block_index)))
    {
        return 1;
    }
    
    /* 计算块地址 */
    address = sdnand_lba_to_command_address(block_index);
    
    /* 单块读 */
    if (block_num == 1)
    {
        if (sdnand_send_cmd(SDNAND_CMD17, address) != 0x00)
        {
            sdnand_chip_select_disable();
            return 1;
        }
        
        if (sdnand_recv_data(data, sdnand_info.logic_block_size) != 0)
        {
            sdnand_chip_select_disable();
            return 1;
        }
    }
    /* 多块读 */
    else
    {
        if (sdnand_send_cmd(SDNAND_CMD18, address) != 0x00)
        {
            sdnand_chip_select_disable();
            return 1;
        }
        
        while (block_num--)
        {
            if (sdnand_recv_data(data, sdnand_info.logic_block_size) != 0)
            {
                sdnand_chip_select_disable();
                return 1;
            }
            data += sdnand_info.logic_block_size;
        }
        
        if (sdnand_send_cmd(SDNAND_CMD12, 0UL) != 0x00U)
        {
            sdnand_chip_select_disable();
            return 1U;
        }
    }
    
    /* 取消片选 */
    sdnand_chip_select_disable();
    
    return 0;
}

/**
 * @brief   写SD NAND指定数量块的数据
 * @param   data: 数据缓冲区指针
 * @param   block_index: 起始块
 * @param   block_num: 块数量
 * @retval  写入结果
 * @arg     0: 写入成功
 * @arg     1: 写入失败
 */
uint8_t sdnand_write_disk(uint8_t *data, uint32_t block_index, uint32_t block_num)
{
    uint32_t address;
    
    if (block_num == 0)
    {
        return 0;
    }
    
    if (data == NULL)
    {
        return 1;
    }
    
    if ((block_index >= sdnand_info.logic_block_num) ||
        (block_num > (sdnand_info.logic_block_num - block_index)))
    {
        return 1;
    }
    
    /* 计算块地址 */
    address = sdnand_lba_to_command_address(block_index);
    
    /* 单块写 */
    if (block_num == 1)
    {
        if (sdnand_send_cmd(SDNAND_CMD24, address) != 0x00)
        {
            sdnand_chip_select_disable();
            return 1;
        }
        
        if (sdnand_send_block_data(SDNAND_DATA_TOKEN_SINGLE, data) != 0)
        {
            sdnand_chip_select_disable();
            return 1;
        }
    }
    /* 多块写 */
    else
    {
        /* ACMD23 is only a performance hint; unsupported cards may reject it. */
        (void)sdnand_send_app_cmd(SDNAND_ACMD23, block_num);
        sdnand_chip_select_disable();
        
        if (sdnand_send_cmd(SDNAND_CMD25, address) != 0x00)
        {
            sdnand_chip_select_disable();
            return 1;
        }
        
        while (block_num--)
        {
            if (sdnand_send_block_data(SDNAND_DATA_TOKEN_MULTI_WRITE, data) != 0)
            {
                sdnand_chip_select_disable();
                return 1;
            }
            data += sdnand_info.logic_block_size;
        }
        if (sdnand_send_block_data(SDNAND_DATA_TOKEN_STOP_WRITE, NULL) != 0)
        {
            sdnand_chip_select_disable();
            return 1;
        }
        
        if (sdnand_wait_ready() != 0U)
        {
            sdnand_chip_select_disable();
            return 1U;
        }
    }
    
    /* 取消片选 */
    sdnand_chip_select_disable();
    
    return 0;
}
