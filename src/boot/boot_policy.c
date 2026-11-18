#include "boot_policy.h"

#include "crc.h"

#include <stddef.h>

static int boot_memory_equal(const BYTE *left, const BYTE *right, size_t size)
{
    size_t index;
    BYTE   difference = 0U;

    /* 固定时间比较，避免哈希比较在第一个差异字节处提前退出。 */
    for (index = 0U; index < size; ++index)
    {
        difference |= (left[index] ^ right[index]);
    }
    return difference == 0U;
}

void boot_image_header_finalize(boot_image_hdr_s *header)
{
    if (header == NULL)
    {
        return;
    }
    header->header_crc32 = 0U;
    header->header_crc32 = crc_compute_iso_hdlc(header, sizeof(*header));
}

static int boot_image_header_crc_is_valid(const boot_image_hdr_s *header)
{
    boot_image_hdr_s copy;
    DWORD            expected;

    copy              = *header;
    expected          = copy.header_crc32;
    copy.header_crc32 = 0U;
    return expected == crc_compute_iso_hdlc(&copy, sizeof(copy));
}

int boot_image_validate(const boot_image_hdr_s *header, const boot_image_constraints_s *constraints, const BYTE actual_payload_hash[BOOT_SHA256_SIZE], boot_signature_verify_fn_t verify_signature, void *verify_context)
{
    DWORD image_end;

    if ((header == NULL) || (constraints == NULL) || (actual_payload_hash == NULL) || (verify_signature == NULL))
    {
        return BOOT_POLICY_INVALID_ARGUMENT;
    }
    if ((header->magic != BOOT_IMAGE_MAGIC) || (header->header_version != BOOT_IMAGE_HEADER_VERSION) || (header->header_size != sizeof(*header)) || !boot_image_header_crc_is_valid(header))
    {
        return BOOT_POLICY_CORRUPT;
    }
    if ((header->image_size == 0U) || (header->image_size > constraints->image_capacity) || (header->chip_id != constraints->chip_id) || (constraints->hardware_revision < header->min_hardware_revision) || (constraints->hardware_revision > header->max_hardware_revision))
    {
        return BOOT_POLICY_INCOMPATIBLE;
    }
    if (header->security_version < constraints->minimum_security_version)
    {
        return BOOT_POLICY_ROLLBACK;
    }

    image_end = header->load_address + header->image_size;
    if ((image_end < header->load_address) || (header->load_address < constraints->executable_start) || (image_end > constraints->executable_end) || (header->entry_address < header->load_address) || (header->entry_address >= image_end))
    {
        return BOOT_POLICY_INCOMPATIBLE;
    }
    if (!boot_memory_equal(header->payload_hash, actual_payload_hash, BOOT_SHA256_SIZE) || (verify_signature(header, verify_context) != 0))
    {
        return BOOT_POLICY_AUTH_FAILED;
    }
    return BOOT_POLICY_OK;
}

void boot_metadata_finalize(boot_metadata_s *metadata)
{
    if (metadata == NULL)
    {
        return;
    }
    metadata->crc32 = 0U;
    metadata->crc32 = crc_compute_iso_hdlc(metadata, sizeof(*metadata));
}

void boot_metadata_initialize(boot_metadata_s *metadata, boot_slot_e active_slot, DWORD active_firmware_version)
{
    if ((metadata == NULL) || (active_slot >= BOOT_SLOT_COUNT))
    {
        return;
    }

    metadata->magic                         = BOOT_METADATA_MAGIC;
    metadata->format_version                = BOOT_METADATA_VERSION;
    metadata->sequence                      = 1U;
    metadata->active_slot                   = active_slot;
    metadata->pending_slot                  = BOOT_SLOT_NONE;
    metadata->retry_remaining               = 0U;
    metadata->successful_mask               = UINT32_C(1) << active_slot;
    metadata->firmware_version[0]           = 0U;
    metadata->firmware_version[1]           = 0U;
    metadata->firmware_version[active_slot] = active_firmware_version;
    boot_metadata_finalize(metadata);
}

int boot_metadata_is_valid(const boot_metadata_s *metadata)
{
    boot_metadata_s copy;
    DWORD           expected;

    if ((metadata == NULL) || (metadata->magic != BOOT_METADATA_MAGIC) || (metadata->format_version != BOOT_METADATA_VERSION) || (metadata->active_slot >= BOOT_SLOT_COUNT) || ((metadata->pending_slot != BOOT_SLOT_NONE) && (metadata->pending_slot >= BOOT_SLOT_COUNT)) || ((metadata->successful_mask & ~UINT32_C(0x3)) != 0U))
    {
        return 0;
    }

    copy       = *metadata;
    expected   = copy.crc32;
    copy.crc32 = 0U;
    return expected == crc_compute_iso_hdlc(&copy, sizeof(copy));
}

static int boot_sequence_is_newer(DWORD candidate, DWORD reference)
{
    /* 有符号差值允许 sequence 在 UINT32_MAX 后自然回绕。 */
    return (int32_t)(candidate - reference) > 0;
}

int boot_metadata_select_latest(const boot_metadata_s copies[2], boot_metadata_s *selected, DWORD *selected_copy)
{
    int   valid0;
    int   valid1;
    DWORD index;

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

    index          = (!valid0 || (valid1 && boot_sequence_is_newer(copies[1].sequence, copies[0].sequence))) ? 1U : 0U;
    *selected      = copies[index];
    *selected_copy = index;
    return BOOT_POLICY_OK;
}

static void boot_metadata_changed(boot_metadata_s *metadata)
{
    ++metadata->sequence;
    boot_metadata_finalize(metadata);
}

int boot_policy_select_slot(boot_metadata_s *metadata, const BYTE slot_bootable[BOOT_SLOT_COUNT], boot_slot_e *selected_slot, int *metadata_changed)
{
    DWORD pending;
    DWORD active;
    DWORD fallback;

    if ((metadata == NULL) || (slot_bootable == NULL) || (selected_slot == NULL) || (metadata_changed == NULL) || !boot_metadata_is_valid(metadata))
    {
        return BOOT_POLICY_INVALID_ARGUMENT;
    }
    *metadata_changed = 0;
    pending           = metadata->pending_slot;
    active            = metadata->active_slot;

    if ((pending != BOOT_SLOT_NONE) && (metadata->retry_remaining > 0U) && (slot_bootable[pending] != 0U))
    {
        /* 跳转前先持久化扣减次数，启动中途掉电也会被计为一次尝试。 */
        --metadata->retry_remaining;
        boot_metadata_changed(metadata);
        *metadata_changed = 1;
        *selected_slot    = (boot_slot_e)pending;
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
        *selected_slot = (boot_slot_e)active;
        return BOOT_POLICY_OK;
    }

    fallback = active ^ 1U;
    if (slot_bootable[fallback] != 0U)
    {
        *selected_slot = (boot_slot_e)fallback;
        return BOOT_POLICY_OK;
    }
    return BOOT_POLICY_NO_BOOTABLE_SLOT;
}

int boot_policy_confirm_success(boot_metadata_s *metadata, boot_slot_e running_slot, DWORD firmware_version)
{
    DWORD slot = running_slot;

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
