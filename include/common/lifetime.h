#ifndef LIFETIME_H
#define LIFETIME_H

#include "ssd_types.h"

/*
 * lifetime — 掉电保持的终身计数（SMART 类，P3）
 *
 * 设计见 docs/design/common/statistics_design.md §9.4：
 *   · 存储：Checkpoint Header 的 padding 内自描述块（magic/version/crc32），
 *     不升全局介质布局版本；旧固件把这段当 padding 忽略。
 *   · 更新节拍：只随 CP 落盘（T3 10 s / journal 水位 1024 写）⇒ 掉电最多丢
 *     一个 CP 间隔的增量。
 *   · 归属：per-instance 字段写各自 die 的 CP；controller-wide 字段（上电时长 /
 *     启动次数 / host 命令数）在每个 die 的块内复制，加载时取 max。
 *   · SSD_STATS_PERSIST_ENABLE=0 时只有介质结构定义，读写/计数函数整体不编译。
 */

#ifndef SSD_STATS_PERSIST_ENABLE
#    define SSD_STATS_PERSIST_ENABLE (1U) /* 0 = 关闭终身计数 */
#endif

#define LIFETIME_INST_MAX (8U)          /* 与 FTL_DIE_MAX 的代码上界解耦；调用方按实际 die 传 */
#define LIFETIME_MAGIC    (0x4C544D31U) /* "LTM1" */
#define LIFETIME_VERSION  (1U)

/* host 命令类别（common 不依赖 HIL/SCSI 枚举）。 */
typedef enum LIFETIME_CMD_E
{
    LIFETIME_CMD_READ = 0,
    LIFETIME_CMD_WRITE,
    LIFETIME_CMD_FLUSH,
    LIFETIME_CMD_TRIM,
    LIFETIME_CMD_NUM
} lifetime_cmd_e;

/* 介质上的终身计数块（CP Header padding 内）。
 * ⚠ 多字节字段按本平台小端写入；QWORD 拆 lo/hi 两个 DWORD（H27，显式端序）。
 * ⚠ crc32 覆盖本结构在 crc32 字段置 0 时的全部字节。 */
typedef struct LIFETIME_DISK_S
{
    DWORD magic;
    DWORD version;
    DWORD crc32;
    DWORD reserved;

    /* per-instance（每个 die 自己的 CP 记录） */
    DWORD read_bytes_lo;
    DWORD read_bytes_hi;
    DWORD write_bytes_lo;
    DWORD write_bytes_hi;
    DWORD ecc_corrected_cmds; /* 发生过 ECC 纠正的读命令数 */
    DWORD unc_events;         /* 不可纠 ECC 事件数 */
    DWORD program_fail;       /* 页编程失败次数 */
    DWORD erase_fail;         /* 块擦除失败次数 */

    /* controller-wide（每 die 记录复制；加载取 max） */
    DWORD power_on_ms_lo;
    DWORD power_on_ms_hi;
    DWORD boot_count;
    DWORD pfr_count;
    DWORD host_cmd[LIFETIME_CMD_NUM];
} lifetime_disk_s;

_Static_assert((sizeof(lifetime_disk_s) % sizeof(DWORD)) == 0U, "lifetime_disk_s 必须是 DWORD 整数倍");

#if SSD_STATS_PERSIST_ENABLE
/**
 * @brief 从一条 CP Header 的 lifetime 块加载本实例计数
 *
 * 块无效（magic/version/crc 不符）⇒ 本实例计数从 0 开始，CP 记录本身仍然有效。
 * controller-wide 字段按 max 合并（多 die 记录可能略有先后）。
 *
 * @param inst 实例号（紧凑 die 号）
 * @param disk CP Header 内的 lifetime 块
 */
void lifetime_init(BYTE inst, const lifetime_disk_s *disk);

/**
 * @brief 本 boot 的 once 初始化：启动上电计时、boot_count++、PFR 计数
 *
 * 全部实例的 lifetime 块加载完成后调用一次（多 die 下 boot_count 在加载前自增
 * 会被各 die 的旧值 max 合并覆盖）；内部另有 once 守卫，重复调用只计一次。
 *
 * @param pfr true = 本次开机走了掉电恢复（PFR）
 */
void lifetime_boot(BOOL pfr);

/**
 * @brief 生成本实例要写入 CP Header 的 lifetime 块（含 CRC）
 *
 * @param inst 实例号
 * @param out  输出块（非空）
 */
void lifetime_snapshot(BYTE inst, lifetime_disk_s *out);

/**
 * @brief 读当前 host 读/写字节总数（所有实例求和；RAM 值，不强制落盘）
 *
 * @param read_bytes  输出：累计读字节
 * @param write_bytes 输出：累计写字节
 */
void lifetime_get_bytes(QWORD *read_bytes, QWORD *write_bytes);

/** @brief host 成功完成的 I/O 字节数（per-instance） */
void lifetime_note_host(BYTE inst, BOOL write, DWORD bytes);

/** @brief host 命令成功完成计数（controller-wide） */
void lifetime_note_cmd(lifetime_cmd_e cmd);

/** @brief 读命令的 ECC 健康（per-instance）：纠正命令数 + UNC 事件 */
void lifetime_note_read_health(BYTE inst, DWORD ecc_max, BOOL unc);

/** @brief 页编程失败（per-instance） */
void lifetime_note_program_fail(BYTE inst);

/** @brief 块擦除失败（per-instance） */
void lifetime_note_erase_fail(BYTE inst);

/**
 * @brief 打印终身计数（聚合 per-instance + controller-wide）
 *
 * @param inst_count 有效实例数（主循环已知的 die 数）
 */
void lifetime_print(BYTE inst_count);
#else
#    define lifetime_init(...)              ((void)0)
#    define lifetime_boot(...)              ((void)0)
#    define lifetime_snapshot(...)          ((void)0)
#    define lifetime_get_bytes(...)         ((void)0)
#    define lifetime_note_host(...)         ((void)0)
#    define lifetime_note_cmd(...)          ((void)0)
#    define lifetime_note_read_health(...)  ((void)0)
#    define lifetime_note_program_fail(...) ((void)0)
#    define lifetime_note_erase_fail(...)   ((void)0)
#    define lifetime_print(...)             ((void)0)
#endif

#endif /* LIFETIME_H */
