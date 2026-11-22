#ifndef SSD_FIRMWARE_BOOT_POLICY_H
#define SSD_FIRMWARE_BOOT_POLICY_H

/*
 * boot_policy.h — A/B 镜像头与启动元数据契约（当前未接入 main）
 *
 * 定义镜像头 / 启动元数据的版本、slot 与状态码；策略实现见 boot_policy.c。
 * 当前仅作为设计组件保留，未接入启动流程。
 */

#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stddef.h>
#include <stdint.h>

/* 磁盘上的多字节字段按小端保存；结构版本变化时必须同步升级打包工具。 */
#define BOOT_IMAGE_MAGIC          UINT32_C(0x53444657) /* "SDFW" */
#define BOOT_IMAGE_HEADER_VERSION UINT32_C(1)
#define BOOT_METADATA_MAGIC       UINT32_C(0x5344424D) /* "SDBM" */
#define BOOT_METADATA_VERSION     UINT32_C(1)
#define BOOT_SHA256_SIZE          32U
#define BOOT_SIGNATURE_SIZE       64U
#define BOOT_SLOT_COUNT           2U
#define BOOT_SLOT_NONE            UINT32_MAX

typedef enum BOOT_SLOT_E
{
    BOOT_SLOT_A = 0,
    BOOT_SLOT_B = 1
} boot_slot_e;

typedef enum BOOT_STATUS_E
{
    BOOT_POLICY_OK               = 0U,
    BOOT_POLICY_INVALID_ARGUMENT = 1U,
    BOOT_POLICY_CORRUPT          = 2U,
    BOOT_POLICY_INCOMPATIBLE     = 3U,
    BOOT_POLICY_ROLLBACK         = 4U,
    BOOT_POLICY_AUTH_FAILED      = 5U,
    BOOT_POLICY_NO_BOOTABLE_SLOT = 6U
} boot_status_e;

/*
 * 固件镜像头。payload_hash 覆盖紧跟镜像头的完整 payload；signature 的
 * 具体算法由产品安全方案决定，参考接口不把 ECDSA/Ed25519 固化在策略层。
 */
typedef struct BOOT_IMAGE_HEADER_S
{
    DWORD magic;
    DWORD header_version;
    DWORD header_size;
    DWORD image_size;
    DWORD firmware_version;
    DWORD security_version;
    DWORD chip_id;
    DWORD min_hardware_revision;
    DWORD max_hardware_revision;
    DWORD load_address;
    DWORD entry_address;
    DWORD flags;
    BYTE  payload_hash[BOOT_SHA256_SIZE];
    BYTE  signature[BOOT_SIGNATURE_SIZE];
    DWORD header_crc32;
} boot_image_hdr_s;

/*
 * A/B 启动记录应在 Flash 中保留至少两份。sequence 用于掉电后选择最新
 * 完整记录；pending_slot 只有在新固件完成自检后才会转为 active_slot。
 */
typedef struct BOOT_METADATA_S
{
    DWORD magic;
    DWORD format_version;
    DWORD sequence;
    DWORD active_slot;
    DWORD pending_slot;
    DWORD retry_remaining;
    DWORD successful_mask;
    DWORD firmware_version[BOOT_SLOT_COUNT];
    DWORD crc32;
} boot_metadata_s;

typedef struct BOOT_IMAGE_CONSTRAINTS_S
{
    DWORD chip_id;
    DWORD hardware_revision;
    DWORD minimum_security_version;
    DWORD image_capacity;
    DWORD executable_start;
    DWORD executable_end; /* 半开区间 [start, end) */
} boot_image_constraints_s;

/* 返回 0 表示签名可信；非 0 表示验证失败。 */
typedef int (*boot_signature_verify_fn_t)(const boot_image_hdr_s *header, void *context);

void boot_image_header_finalize(boot_image_hdr_s *header);
int  boot_image_validate(const boot_image_hdr_s *header, const boot_image_constraints_s *constraints, const BYTE actual_payload_hash[BOOT_SHA256_SIZE], boot_signature_verify_fn_t verify_signature, void *verify_context);

void boot_metadata_initialize(boot_metadata_s *metadata, boot_slot_e active_slot, DWORD active_firmware_version);
void boot_metadata_finalize(boot_metadata_s *metadata);
int  boot_metadata_is_valid(const boot_metadata_s *metadata);
int  boot_metadata_select_latest(const boot_metadata_s copies[2], boot_metadata_s *selected, DWORD *selected_copy);

/*
 * 根据镜像验证结果选择本次槽位。函数会先扣减 pending 槽的尝试次数，调用者
 * 必须在启动镜像前将更新后的 metadata 原子地写入另一份元数据副本。
 */
int boot_policy_select_slot(boot_metadata_s *metadata, const BYTE slot_bootable[BOOT_SLOT_COUNT], boot_slot_e *selected_slot, int *metadata_changed);

/* 主固件完成最低可用自检后调用，并将返回的记录写入 Flash。 */
int boot_policy_confirm_success(boot_metadata_s *metadata, boot_slot_e running_slot, DWORD firmware_version);

#endif /* SSD_FIRMWARE_BOOT_POLICY_H */
