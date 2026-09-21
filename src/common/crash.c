#include "common/ssd_crash.h"

#include <string.h>

#define SSD_CRASH_MAGIC   0x43524153U
#define SSD_CRASH_VERSION 1U

static volatile ssd_crash_record_t g_ssd_crash_record __attribute__((section(".bkpsram_data")));

static void ssd_crash_copy(char *destination, const char *source)
{
    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }
    (void)strncpy(destination, source, SSD_CRASH_TEXT_CAPACITY - 1U);
    destination[SSD_CRASH_TEXT_CAPACITY - 1U] = '\0';
}

void ssd_crash_record_assert(const char *expression, const char *file, uint32_t line)
{
    uint32_t checksum;

    g_ssd_crash_record.magic   = 0U;
    g_ssd_crash_record.version = SSD_CRASH_VERSION;
    g_ssd_crash_record.line    = line;
    ssd_crash_copy((char *)g_ssd_crash_record.expression, expression);
    ssd_crash_copy((char *)g_ssd_crash_record.file, file);
    checksum = SSD_CRASH_VERSION ^ line;
    for (uint32_t index = 0U; index < SSD_CRASH_TEXT_CAPACITY; index++)
    {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= (uint8_t)g_ssd_crash_record.expression[index];
        checksum ^= (uint8_t)g_ssd_crash_record.file[index];
    }
    g_ssd_crash_record.checksum = checksum;
    __asm volatile("" ::
                       : "memory");
    g_ssd_crash_record.magic = SSD_CRASH_MAGIC;
}

const ssd_crash_record_t *ssd_crash_record_get(void)
{
    uint32_t checksum = SSD_CRASH_VERSION ^ g_ssd_crash_record.line;

    if ((g_ssd_crash_record.magic != SSD_CRASH_MAGIC) || (g_ssd_crash_record.version != SSD_CRASH_VERSION))
    {
        return NULL;
    }
    for (uint32_t index = 0U; index < SSD_CRASH_TEXT_CAPACITY; index++)
    {
        checksum = (checksum << 5) | (checksum >> 27);
        checksum ^= (uint8_t)g_ssd_crash_record.expression[index];
        checksum ^= (uint8_t)g_ssd_crash_record.file[index];
    }
    return (checksum == g_ssd_crash_record.checksum) ? (const ssd_crash_record_t *)&g_ssd_crash_record : NULL;
}

void ssd_crash_record_clear(void)
{
    g_ssd_crash_record.magic = 0U;
}
