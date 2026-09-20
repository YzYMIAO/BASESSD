#ifndef SSD_CRASH_H
#define SSD_CRASH_H

#include <stdint.h>

#define SSD_CRASH_TEXT_CAPACITY 96U

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t line;
    uint32_t checksum;
    char expression[SSD_CRASH_TEXT_CAPACITY];
    char file[SSD_CRASH_TEXT_CAPACITY];
} ssd_crash_record_t;

void ssd_crash_record_assert(const char *expression, const char *file, uint32_t line);
const ssd_crash_record_t *ssd_crash_record_get(void);
void ssd_crash_record_clear(void);

#endif
