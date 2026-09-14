

#ifndef __HYPERRAM_H
#define __HYPERRAM_H

#include "sys.h"

#include <stdint.h>

/* HyperRAM相关定义 */
#define HYPERRAM_BASE_ADDR XSPI2_BASE
#define HYPERRAM_CACHE_LINE_SIZE 32U

/*
 * These tests write their target range. The caller must reserve the selected
 * range for diagnostics; do not test a range containing stacks, DMA buffers,
 * FTL tables, or SSD_CODE_SLOW functions.
 */
typedef enum
{
    HYPERRAM_TEST_OK = 0,
    HYPERRAM_TEST_NOT_READY,
    HYPERRAM_TEST_INVALID_ARGUMENT,
    HYPERRAM_TEST_OUT_OF_RANGE,
    HYPERRAM_TEST_DATA_MISMATCH
} hyperram_test_status_t;

typedef struct
{
    uint32_t tested_bytes;
    uint32_t failed_offset;
    uint32_t expected_value;
    uint32_t actual_value;
} hyperram_test_report_t;

/* 函数声明 */
uint8_t hyperram_init(void);        /* 初始化HyperRAM */
uint32_t hyperram_get_size(void);   /* 获取HyperRAM容量 */
uint8_t hyperram_is_ready(void);

/* Destructive board-level tests. offset and length must be 4-byte aligned. */
hyperram_test_status_t hyperram_test_memory(uint32_t offset, uint32_t length,
                                            hyperram_test_report_t *report);
hyperram_test_status_t hyperram_test_address_lines(uint32_t stride,
                                                   hyperram_test_report_t *report);

#endif
