#ifndef SSD_FIRMWARE_BOOT_POLICY_H
#define SSD_FIRMWARE_BOOT_POLICY_H

#include <stddef.h>
#include <stdint.h>

/* 磁盘上的多字节字段按小端保存；结构版本变化时必须同步升级打包工具。 */
#define BOOT_IMAGE_MAGIC           UINT32_C(0x53444657) /* "SDFW" */
#define BOOT_IMAGE_HEADER_VERSION  UINT32_C(1)
#define BOOT_METADATA_MAGIC        UINT32_C(0x5344424D) /* "SDBM" */
#define BOOT_METADATA_VERSION      UINT32_C(1)
#define BOOT_SHA256_SIZE           32U
#define BOOT_SIGNATURE_SIZE        64U
#define BOOT_SLOT_COUNT            2U
#define BOOT_SLOT_NONE             UINT32_MAX

typedef enum
{
    BOOT_SLOT_A = 0,
    BOOT_SLOT_B = 1
} boot_slot_t;

typedef enum
{
    BOOT_POLICY_OK                 = 0,
    BOOT_POLICY_INVALID_ARGUMENT   = -1,
    BOOT_POLICY_CORRUPT            = -2,
    BOOT_POLICY_INCOMPATIBLE       = -3,
    BOOT_POLICY_ROLLBACK           = -4,
    BOOT_POLICY_AUTH_FAILED        = -5,
    BOOT_POLICY_NO_BOOTABLE_SLOT   = -6
} boot_policy_status_t;

/*
 * 固件镜像头。payload_hash 覆盖紧跟镜像头的完整 payload；signature 的
 * 具体算法由产品安全方案决定，参考接口不把 ECDSA/Ed25519 固化在策略层。
 */
typedef struct
{
    uint32_t magic;
    uint32_t header_version;
    uint32_t header_size;
    uint32_t image_size;
    uint32_t firmware_version;
    uint32_t security_version;
    uint32_t chip_id;
    uint32_t min_hardware_revision;
    uint32_t max_hardware_revision;
    uint32_t load_address;
    uint32_t entry_address;
    uint32_t flags;
    uint8_t  payload_hash[BOOT_SHA256_SIZE];
    uint8_t  signature[BOOT_SIGNATURE_SIZE];
    uint32_t header_crc32;
} boot_image_header_t;

/*
 * A/B 启动记录应在 Flash 中保留至少两份。sequence 用于掉电后选择最新
 * 完整记录；pending_slot 只有在新固件完成自检后才会转为 active_slot。
 */
typedef struct
{
    uint32_t magic;
    uint32_t format_version;
    uint32_t sequence;
    uint32_t active_slot;
    uint32_t pending_slot;
    uint32_t retry_remaining;
    uint32_t successful_mask;
    uint32_t firmware_version[BOOT_SLOT_COUNT];
    uint32_t crc32;
} boot_metadata_t;

typedef struct
{
    uint32_t chip_id;
    uint32_t hardware_revision;
    uint32_t minimum_security_version;
    uint32_t image_capacity;
    uint32_t executable_start;
    uint32_t executable_end; /* 半开区间 [start, end) */
} boot_image_constraints_t;

/* 返回 0 表示签名可信；非 0 表示验证失败。 */
typedef int (*boot_signature_verify_fn)(const boot_image_header_t *header, void *context);

uint32_t boot_crc32(const void *data, size_t size);

void boot_image_header_finalize(boot_image_header_t *header);
int  boot_image_validate(const boot_image_header_t      *header,
                         const boot_image_constraints_t *constraints,
                         const uint8_t                    actual_payload_hash[BOOT_SHA256_SIZE],
                         boot_signature_verify_fn         verify_signature,
                         void                            *verify_context);

void boot_metadata_initialize(boot_metadata_t *metadata, boot_slot_t active_slot,
                              uint32_t active_firmware_version);
void boot_metadata_finalize(boot_metadata_t *metadata);
int  boot_metadata_is_valid(const boot_metadata_t *metadata);
int  boot_metadata_select_latest(const boot_metadata_t copies[2], boot_metadata_t *selected,
                                 uint32_t *selected_copy);

/*
 * 根据镜像验证结果选择本次槽位。函数会先扣减 pending 槽的尝试次数，调用者
 * 必须在启动镜像前将更新后的 metadata 原子地写入另一份元数据副本。
 */
int boot_policy_select_slot(boot_metadata_t *metadata, const uint8_t slot_bootable[BOOT_SLOT_COUNT],
                            boot_slot_t *selected_slot, int *metadata_changed);

/* 主固件完成最低可用自检后调用，并将返回的记录写入 Flash。 */
int boot_policy_confirm_success(boot_metadata_t *metadata, boot_slot_t running_slot,
                                uint32_t firmware_version);

#endif /* SSD_FIRMWARE_BOOT_POLICY_H */
