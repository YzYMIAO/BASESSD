#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 本层提供 3 个 PBA 操作 helper + 3 个内部同步 NAND 操作函数。
 *
 * 【PBA helper】（已实现）
 *   ① ftl_cmd_setup_ppa() — PBA→CH/CE/PAA + desc 链配置
 *   ② ftl_cmd_write_data() — 用户数据写入 desc buffer
 *   ③ ftl_cmd_read_data() — 从 desc buffer 读取用户数据
 *
 * 【内部同步 NAND 操作】（新增）
 *   ④ ftl_internal_sync_wait() — 等待指定 command_id 完成
 *   ⑤ ftl_internal_read_page() — 同步读页
 *   ⑥ ftl_internal_write_page() — 同步写页
 *   ⑦ ftl_internal_erase_block() — 同步擦除块
 *
 * 同步操作内部各自 alloc/fc_submit/sync_wait/release，调用者只需
 * 传入参数即可。适用于 L2P region R/W、Journal flush、GC 等内部操作。
 */

#include "fc/fc_command.h"
#include "ftl/ftl_type.h"
#include "nand/nand_config.h"

#include <stdint.h>

/* ── PBA helper（已实现）── */

int ftl_cmd_setup_ppa(fc_command_t *cmd, nand_pba_t pba);
int ftl_cmd_write_data(fc_command_t *cmd, const void *data, uint32_t length);
int ftl_cmd_read_data(fc_command_t *cmd, void *buffer, uint32_t length);

/* ── 内部同步 NAND 操作 ── */

/*
 * ftl_internal_sync_wait - 同步等待指定 command_id 完成
 *
 * 循环调用 fc_service() + fc_poll_completion() 直到指定命令完成。
 * 最多等待 timeout_us 微秒。
 *
 * @param cmd_id      要等待的 command_id
 * @param status_out  输出：completion status
 * @param timeout_us  超时微秒（0=使用默认）
 * @return 0 找到（status_out 有效），-1 超时
 */
int ftl_internal_sync_wait(uint16_t cmd_id, fc_command_status_t *status_out, uint32_t timeout_us);

/*
 * ftl_internal_read_page - 同步读一页 NAND 到 buffer
 *
 * 内部：alloc → 配置 → ftl_cmd_setup_ppa → submit → sync_wait → read_data → release
 *
 * @param pba     物理地址
 * @param buffer  输出 buffer（至少 length 字节）
 * @param length  读取字节数（通常 = PAGE_MAIN_SIZE = 2048）
 * @param crc     FC_CRC_ENABLE / FC_CRC_DISABLE
 * @param random  FC_RANDOM_ENABLE / FC_RANDOM_DISABLE
 * @param module  FC_COMMAND_MODULE_*
 * @param lba     META 值（验证用）
 * @return 0 成功，-1 失败
 */
int ftl_internal_read_page(nand_pba_t pba, void *buffer, uint32_t length, uint8_t crc, uint8_t random, uint8_t module, lba_t lba);

/*
 * ftl_internal_read_page_raw - 同步读全页（含 Spare/OOB 区）
 *
 * 以 LDPC_RAW 模式读取完整的 NAND 页面（PAGE_SIZE = 2112 字节），
 * 不经过 ECC 解码。用于出厂坏块扫描时读取 OOB 标记。
 *
 * 内部读 CW 数据后组装全页 buffer，布局：
 *   [CW0_data:528][CW1_data:528][CW2_data:528][CW3_data:528] = 2112B
 *   其中每个 CW: [META:4][CRC:4][USER:512][PARITY:8]
 *
 * @param pba     物理地址
 * @param buffer  输出 buffer（至少 PAGE_SIZE = 2112 字节）
 * @return 0 成功，-1 失败
 */
int ftl_internal_read_page_raw(nand_pba_t pba, void *buffer);

/*
 * ftl_internal_write_page - 同步写一页数据到 NAND
 *
 * @param pba     物理地址
 * @param data    写入数据（至少 length 字节）
 * @param length  写入字节数
 * @param crc     FC_CRC_ENABLE / FC_CRC_DISABLE
 * @param random  FC_RANDOM_ENABLE / FC_RANDOM_DISABLE
 * @param module  FC_COMMAND_MODULE_*
 * @param lba     META 值
 * @return 0 成功，-1 失败
 */
int ftl_internal_write_page(nand_pba_t pba, const void *data, uint32_t length, uint8_t crc, uint8_t random, uint8_t module, lba_t lba);

/*
 * ftl_internal_erase_block - 同步擦除一个 Block
 *
 * @param block_pba  Block 的 PBA（page/plane/cw 无关）
 * @return 0 成功，-1 失败
 */
int ftl_internal_erase_block(nand_pba_t block_pba);

#endif /* FTL_FC_INTERFACE_H */