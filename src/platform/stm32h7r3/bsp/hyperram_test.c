#include "hyperram.h"

#include <stddef.h>

#define HYPERRAM_WORD_SIZE 4U
#define HYPERRAM_TEST_PATTERN_COUNT 4U

/* Different bit distributions expose stuck data bits and timing/address errors. */
static const uint32_t g_hyperram_test_patterns[HYPERRAM_TEST_PATTERN_COUNT] = {
    0x00000000U,
    0xFFFFFFFFU,
    0xAAAAAAAAU,
    0x55555555U,
};

static void hyperram_test_reset_report(hyperram_test_report_t *report)
{
    if (report != NULL)
    {
        report->tested_bytes = 0U;
        report->failed_offset = 0U;
        report->expected_value = 0U;
        report->actual_value = 0U;
    }
}

static void hyperram_test_record_failure(hyperram_test_report_t *report, uint32_t offset,
                                         uint32_t expected, uint32_t actual)
{
    if (report != NULL)
    {
        report->failed_offset = offset;
        report->expected_value = expected;
        report->actual_value = actual;
    }
}

static void hyperram_test_flush_cache(uintptr_t address, uint32_t length)
{
    uintptr_t start = address & ~(uintptr_t)(HYPERRAM_CACHE_LINE_SIZE - 1U);
    uintptr_t end = (address + length + HYPERRAM_CACHE_LINE_SIZE - 1U) &
                    ~(uintptr_t)(HYPERRAM_CACHE_LINE_SIZE - 1U);

    /*
     * CPU writes initially reside in D-Cache. Clean pushes them to HyperRAM;
     * invalidation before readback prevents a false pass from cache hits.
     */
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    SCB_InvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
    __DSB();
    __ISB();
}

static hyperram_test_status_t hyperram_test_validate_range(uint32_t offset, uint32_t length)
{
    uint32_t size = hyperram_get_size();

    if (hyperram_is_ready() == 0U)
        return HYPERRAM_TEST_NOT_READY;

    if ((length == 0U) || ((offset & (HYPERRAM_WORD_SIZE - 1U)) != 0U) ||
        ((length & (HYPERRAM_WORD_SIZE - 1U)) != 0U))
        return HYPERRAM_TEST_INVALID_ARGUMENT;

    if ((offset >= size) || (length > (size - offset)))
        return HYPERRAM_TEST_OUT_OF_RANGE;

    return HYPERRAM_TEST_OK;
}

hyperram_test_status_t hyperram_test_memory(uint32_t offset, uint32_t length,
                                            hyperram_test_report_t *report)
{
    volatile uint32_t *memory;
    uint32_t pattern_index;
    uint32_t word_index;
    uint32_t word_count = length / HYPERRAM_WORD_SIZE;
    hyperram_test_status_t status;

    hyperram_test_reset_report(report);
    status = hyperram_test_validate_range(offset, length);
    if (status != HYPERRAM_TEST_OK)
        return status;

    memory = (volatile uint32_t *)(uintptr_t)(HYPERRAM_BASE_ADDR + offset);
    for (pattern_index = 0U; pattern_index < HYPERRAM_TEST_PATTERN_COUNT; pattern_index++)
    {
        uint32_t pattern = g_hyperram_test_patterns[pattern_index];

        for (word_index = 0U; word_index < word_count; word_index++)
            memory[word_index] = pattern ^ (word_index * 0x9E3779B9U);

        hyperram_test_flush_cache((uintptr_t)memory, length);

        for (word_index = 0U; word_index < word_count; word_index++)
        {
            uint32_t expected = pattern ^ (word_index * 0x9E3779B9U);
            uint32_t actual = memory[word_index];

            if (actual != expected)
            {
                hyperram_test_record_failure(report, offset + (word_index * HYPERRAM_WORD_SIZE),
                                             expected, actual);
                return HYPERRAM_TEST_DATA_MISMATCH;
            }
        }
    }

    if (report != NULL)
        report->tested_bytes = length;
    return HYPERRAM_TEST_OK;
}

hyperram_test_status_t hyperram_test_address_lines(uint32_t stride,
                                                   hyperram_test_report_t *report)
{
    uint32_t offset;
    uint32_t size = hyperram_get_size();

    hyperram_test_reset_report(report);
    if (hyperram_is_ready() == 0U)
        return HYPERRAM_TEST_NOT_READY;

    if ((stride < HYPERRAM_CACHE_LINE_SIZE) || ((stride & (stride - 1U)) != 0U))
        return HYPERRAM_TEST_INVALID_ARGUMENT;

    for (offset = 0U; offset < size; offset += stride)
        *(volatile uint32_t *)(uintptr_t)(HYPERRAM_BASE_ADDR + offset) = 0xA5A50000U ^ offset;

    /* Only touch cache lines written by this sparse test. */
    for (offset = 0U; offset < size; offset += stride)
        hyperram_test_flush_cache((uintptr_t)(HYPERRAM_BASE_ADDR + offset), HYPERRAM_WORD_SIZE);

    for (offset = 0U; offset < size; offset += stride)
    {
        uint32_t expected = 0xA5A50000U ^ offset;
        uint32_t actual = *(volatile uint32_t *)(uintptr_t)(HYPERRAM_BASE_ADDR + offset);

        if (actual != expected)
        {
            hyperram_test_record_failure(report, offset, expected, actual);
            return HYPERRAM_TEST_DATA_MISMATCH;
        }
    }

    if (report != NULL)
        report->tested_bytes = ((size + stride - 1U) / stride) * HYPERRAM_WORD_SIZE;
    return HYPERRAM_TEST_OK;
}
