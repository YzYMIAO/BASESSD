#include "firmware/boot_policy.h"

#include <assert.h>
#include <stdio.h>

static int accept_signature(const boot_image_header_t *header, void *context)
{
    (void)header;
    (void)context;
    return 0;
}

static void test_image_validation(void)
{
    boot_image_header_t      header      = {0};
    boot_image_constraints_t constraints = {
        .chip_id                  = 0x1234U,
        .hardware_revision        = 2U,
        .minimum_security_version = 3U,
        .image_capacity           = 0x20000U,
        .executable_start         = 0x80000000U,
        .executable_end           = 0x80100000U,
    };
    uint8_t digest[BOOT_SHA256_SIZE] = {0x5AU};

    header.magic                 = BOOT_IMAGE_MAGIC;
    header.header_version        = BOOT_IMAGE_HEADER_VERSION;
    header.header_size           = sizeof(header);
    header.image_size            = 0x10000U;
    header.security_version      = 3U;
    header.chip_id               = 0x1234U;
    header.min_hardware_revision = 1U;
    header.max_hardware_revision = 3U;
    header.load_address          = 0x80000000U;
    header.entry_address         = 0x80000100U;
    header.payload_hash[0]       = 0x5AU;
    boot_image_header_finalize(&header);

    assert(boot_image_validate(&header, &constraints, digest, accept_signature, NULL) ==
           BOOT_POLICY_OK);

    header.security_version = 2U;
    boot_image_header_finalize(&header);
    assert(boot_image_validate(&header, &constraints, digest, accept_signature, NULL) ==
           BOOT_POLICY_ROLLBACK);
}

static void test_metadata_copy_selection(void)
{
    boot_metadata_t copies[2];
    boot_metadata_t selected;
    uint32_t        selected_copy;

    boot_metadata_initialize(&copies[0], BOOT_SLOT_A, 10U);
    copies[1]          = copies[0];
    copies[1].sequence = 2U;
    boot_metadata_finalize(&copies[1]);

    assert(boot_metadata_select_latest(copies, &selected, &selected_copy) == BOOT_POLICY_OK);
    assert(selected_copy == 1U);

    /* 模拟第二份记录掉电写坏，选择器必须回到仍有合法 CRC 的第一份。 */
    copies[1].active_slot = 99U;
    assert(boot_metadata_select_latest(copies, &selected, &selected_copy) == BOOT_POLICY_OK);
    assert(selected_copy == 0U);
}

static void test_pending_slot_retry_and_confirm(void)
{
    boot_metadata_t metadata;
    boot_slot_t     selected;
    uint8_t         bootable[BOOT_SLOT_COUNT] = {1U, 1U};
    int             changed;

    boot_metadata_initialize(&metadata, BOOT_SLOT_A, 10U);
    metadata.pending_slot    = BOOT_SLOT_B;
    metadata.retry_remaining = 2U;
    boot_metadata_finalize(&metadata);

    assert(boot_policy_select_slot(&metadata, bootable, &selected, &changed) == BOOT_POLICY_OK);
    assert(selected == BOOT_SLOT_B);
    assert(changed != 0);
    assert(metadata.retry_remaining == 1U);
    assert(boot_metadata_is_valid(&metadata));

    assert(boot_policy_confirm_success(&metadata, BOOT_SLOT_B, 11U) == BOOT_POLICY_OK);
    assert(metadata.active_slot == BOOT_SLOT_B);
    assert(metadata.pending_slot == BOOT_SLOT_NONE);
    assert(metadata.firmware_version[BOOT_SLOT_B] == 11U);
}

static void test_retry_exhaustion_falls_back(void)
{
    boot_metadata_t metadata;
    boot_slot_t     selected;
    uint8_t         bootable[BOOT_SLOT_COUNT] = {1U, 1U};
    int             changed;

    boot_metadata_initialize(&metadata, BOOT_SLOT_A, 10U);
    metadata.pending_slot    = BOOT_SLOT_B;
    metadata.retry_remaining = 0U;
    boot_metadata_finalize(&metadata);

    assert(boot_policy_select_slot(&metadata, bootable, &selected, &changed) == BOOT_POLICY_OK);
    assert(selected == BOOT_SLOT_A);
    assert(changed != 0);
    assert(metadata.pending_slot == BOOT_SLOT_NONE);
}

int main(void)
{
    test_image_validation();
    test_metadata_copy_selection();
    test_pending_slot_retry_and_confirm();
    test_retry_exhaustion_falls_back();
    puts("boot policy tests passed");
    return 0;
}
