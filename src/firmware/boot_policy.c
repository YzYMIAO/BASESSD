#include "firmware/boot_policy.h"

#include <stddef.h>

static int boot_memory_equal(const uint8_t *left, const uint8_t *right, size_t size)
{
    size_t  index;
    uint8_t difference = 0U;

    /* 固定时间比较，避免哈希比较在第一个差异字节处提前退出。 */
    for (index = 0U; index < size; ++index)
    {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0U;
}

uint32_t boot_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t       crc   = UINT32_MAX;
    size_t         index;
    uint32_t       bit;

    if ((data == NULL) && (size != 0U))
    {
        return 0U;
    }

    /* CRC-32/ISO-HDLC，多项式 0x04C11DB7 的反射形式。 */
    for (index = 0U; index < size; ++index)
    {
        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            uint32_t mask = (uint32_t)(0U - (crc & 1U));
            crc           = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

void boot_image_header_finalize(boot_image_header_t *header)
{
    if (header == NULL)
    {
        return;
    }
    header->header_crc32 = 0U;
    header->header_crc32 = boot_crc32(header, sizeof(*header));
}

static int boot_image_header_crc_is_valid(const boot_image_header_t *header)
{
    boot_image_header_t copy;
    uint32_t            expected;

    copy              = *header;
    expected          = copy.header_crc32;
    copy.header_crc32 = 0U;
    return expected == boot_crc32(&copy, sizeof(copy));
}

int boot_image_validate(const boot_image_header_t      *header,
                        const boot_image_constraints_t *constraints,
                        const uint8_t                   actual_payload_hash[BOOT_SHA256_SIZE],
                        boot_signature_verify_fn        verify_signature,
                        void                           *verify_context)
{
    uint32_t image_end;

    if ((header == NULL) || (constraints == NULL) || (actual_payload_hash == NULL) ||
        (verify_signature == NULL))
    {
        return BOOT_POLICY_INVALID_ARGUMENT;
    }
    if ((header->magic != BOOT_IMAGE_MAGIC) ||
        (header->header_version != BOOT_IMAGE_HEADER_VERSION) ||
        (header->header_size != sizeof(*header)) || !boot_image_header_crc_is_valid(header))
    {
        return BOOT_POLICY_CORRUPT;
    }
    if ((header->image_size == 0U) || (header->image_size > constraints->image_capacity) ||
        (header->chip_id != constraints->chip_id) ||
        (constraints->hardware_revision < header->min_hardware_revision) ||
        (constraints->hardware_revision > header->max_hardware_revision))
    {
        return BOOT_POLICY_INCOMPATIBLE;
    }
    if (header->security_version < constraints->minimum_security_version)
    {
        return BOOT_POLICY_ROLLBACK;
    }

    image_end = header->load_address + header->image_size;
    if ((image_end < header->load_address) ||
        (header->load_address < constraints->executable_start) ||
        (image_end > constraints->executable_end) || (header->entry_address < header->load_address) ||
        (header->entry_address >= image_end))
    {
        return BOOT_POLICY_INCOMPATIBLE;
    }
    if (!boot_memory_equal(header->payload_hash, actual_payload_hash, BOOT_SHA256_SIZE) ||
        (verify_signature(header, verify_context) != 0))
    {
        return BOOT_POLICY_AUTH_FAILED;
    }
    return BOOT_POLICY_OK;
}

void boot_metadata_finalize(boot_metadata_t *metadata)
{
    if (metadata == NULL)
    {
        return;
    }
    metadata->crc32 = 0U;
    metadata->crc32 = boot_crc32(metadata, sizeof(*metadata));
}

void boot_metadata_initialize(boot_metadata_t *metadata, boot_slot_t active_slot, uint32_t active_firmware_version)
{
    if ((metadata == NULL) || ((uint32_t)active_slot >= BOOT_SLOT_COUNT))
    {
        return;
    }

    metadata->magic                                   = BOOT_METADATA_MAGIC;
    metadata->format_version                          = BOOT_METADATA_VERSION;
    metadata->sequence                                = 1U;
    metadata->active_slot                             = (uint32_t)active_slot;
    metadata->pending_slot                            = BOOT_SLOT_NONE;
    metadata->retry_remaining                         = 0U;
    metadata->successful_mask                         = UINT32_C(1) << (uint32_t)active_slot;
    metadata->firmware_version[0]                     = 0U;
    metadata->firmware_version[1]                     = 0U;
    metadata->firmware_version[(uint32_t)active_slot] = active_firmware_version;
    boot_metadata_finalize(metadata);
}

int boot_metadata_is_valid(const boot_metadata_t *metadata)
{
    boot_metadata_t copy;
    uint32_t        expected;

    if ((metadata == NULL) || (metadata->magic != BOOT_METADATA_MAGIC) ||
        (metadata->format_version != BOOT_METADATA_VERSION) ||
        (metadata->active_slot >= BOOT_SLOT_COUNT) ||
        ((metadata->pending_slot != BOOT_SLOT_NONE) &&
         (metadata->pending_slot >= BOOT_SLOT_COUNT)) ||
        ((metadata->successful_mask & ~UINT32_C(0x3)) != 0U))
    {
        return 0;
    }

    copy       = *metadata;
    expected   = copy.crc32;
    copy.crc32 = 0U;
    return expected == boot_crc32(&copy, sizeof(copy));
}

static int boot_sequence_is_newer(uint32_t candidate, uint32_t reference)
{
    /* 有符号差值允许 sequence 在 UINT32_MAX 后自然回绕。 */
    return (int32_t)(candidate - reference) > 0;
}

int boot_metadata_select_latest(const boot_metadata_t copies[2], boot_metadata_t *selected, uint32_t *selected_copy)
{
    int      valid0;
    int      valid1;
    uint32_t index;

    if ((copies == NULL) || (selected == NULL) || (selected_copy == NULL))
    {
        return BOOT_POLICY_INVALID_ARGUMENT;
    }

    valid0 = boot_metadata_is_valid(&copies[0]);
    valid1 = boot_metadata_is_valid(&copies[1]);
    if (!valid0 && !valid1)
    {
        return BOOT_POLICY_CORRUPT;
    }

    index          = (!valid0 || (valid1 && boot_sequence_is_newer(copies[1].sequence, copies[0].sequence)))
                         ? 1U
                         : 0U;
    *selected      = copies[index];
    *selected_copy = index;
    return BOOT_POLICY_OK;
}

static void boot_metadata_changed(boot_metadata_t *metadata)
{
    ++metadata->sequence;
    boot_metadata_finalize(metadata);
}

int boot_policy_select_slot(boot_metadata_t *metadata, const uint8_t slot_bootable[BOOT_SLOT_COUNT], boot_slot_t *selected_slot, int *metadata_changed)
{
    uint32_t pending;
    uint32_t active;
    uint32_t fallback;

    if ((metadata == NULL) || (slot_bootable == NULL) || (selected_slot == NULL) ||
        (metadata_changed == NULL) || !boot_metadata_is_valid(metadata))
    {
        return BOOT_POLICY_INVALID_ARGUMENT;
    }
    *metadata_changed = 0;
    pending           = metadata->pending_slot;
    active            = metadata->active_slot;

    if ((pending != BOOT_SLOT_NONE) && (metadata->retry_remaining > 0U) &&
        (slot_bootable[pending] != 0U))
    {
        /* 跳转前先持久化扣减次数，启动中途掉电也会被计为一次尝试。 */
        --metadata->retry_remaining;
        boot_metadata_changed(metadata);
        *metadata_changed = 1;
        *selected_slot    = (boot_slot_t)pending;
        return BOOT_POLICY_OK;
    }

    if (pending != BOOT_SLOT_NONE)
    {
        metadata->pending_slot    = BOOT_SLOT_NONE;
        metadata->retry_remaining = 0U;
        boot_metadata_changed(metadata);
        *metadata_changed = 1;
    }

    if (slot_bootable[active] != 0U)
    {
        *selected_slot = (boot_slot_t)active;
        return BOOT_POLICY_OK;
    }

    fallback = active ^ 1U;
    if (slot_bootable[fallback] != 0U)
    {
        *selected_slot = (boot_slot_t)fallback;
        return BOOT_POLICY_OK;
    }
    return BOOT_POLICY_NO_BOOTABLE_SLOT;
}

int boot_policy_confirm_success(boot_metadata_t *metadata, boot_slot_t running_slot, uint32_t firmware_version)
{
    uint32_t slot = (uint32_t)running_slot;

    if ((metadata == NULL) || (slot >= BOOT_SLOT_COUNT) || !boot_metadata_is_valid(metadata))
    {
        return BOOT_POLICY_INVALID_ARGUMENT;
    }

    metadata->active_slot     = slot;
    metadata->pending_slot    = BOOT_SLOT_NONE;
    metadata->retry_remaining = 0U;
    metadata->successful_mask |= UINT32_C(1) << slot;
    metadata->firmware_version[slot] = firmware_version;
    boot_metadata_changed(metadata);
    return BOOT_POLICY_OK;
}
