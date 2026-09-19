#ifndef TEST_FC_NAND_H
#define TEST_FC_NAND_H

#include "fc/fc.h"

/*
 * 仅在 FC_NAND_TEST_ENABLE=1 的 H743 固件测试构建中实现。
 * test_paa 必须来自已预留且已经完成坏块检查的测试 Block；这些接口会擦除、顺序编程并校验整个 Block。
 */
fc_status_t fc_nand_test_read_id(void);
fc_status_t fc_nand_test_erase_block(nand_paa_t test_paa);
fc_status_t fc_nand_test_program_block(nand_paa_t test_paa);
fc_status_t fc_nand_test_read_block(nand_paa_t test_paa);
fc_status_t fc_nand_test_run_all(nand_paa_t test_paa);

#endif /* TEST_FC_NAND_H */
