/*
 * lifetime.c — 掉电保持的终身计数（P3）
 *
 * 设计见 docs/design/common/statistics_design.md §9.4：
 *   · 介质块在 CP Header 的 padding 内；只随 CP 落盘。
 *   · per-instance 与 controller-wide 字段分开；controller-wide 在多 die 的块内复制，
 *     加载取 max。
 *   · 块自带 magic/version/crc32；失效只把计数当 0，不使 CP 记录作废。
 */

#include "lifetime.h"

#include "assertion.h"
#include "crc.h"   /* crc_compute_iso_hdlc */
#include "delay.h" /* delay_time_now_us64 */
#include "log.h"

#include <string.h>

#if SSD_STATS_PERSIST_ENABLE

typedef struct LIFETIME_RAM_INST_S
{
    QWORD read_bytes;
    QWORD write_bytes;
    DWORD ecc_corrected_cmds;
    DWORD unc_events;
    DWORD program_fail;
    DWORD erase_fail;
} lifetime_ram_inst_s;

typedef struct LIFETIME_RAM_GLOBAL_S
{
    QWORD power_on_base_ms; /* 上次 CP 落盘的累计上电时长 */
    DWORD boot_count;
    DWORD pfr_count;
    DWORD host_cmd[LIFETIME_CMD_NUM];
} lifetime_ram_global_s;

static lifetime_ram_inst_s   s_inst[LIFETIME_INST_MAX];
static lifetime_ram_global_s s_global;
static BOOL                  s_boot_started;
static BOOL                  s_pfr_counted;
static QWORD                 s_boot_start_ms;

/**
 * @brief 当前单调毫秒（64 位；不可用时返回 0）
 *
 * @return 毫秒
 */
static QWORD lifetime_now_ms(void)
{
    return delay_time_now_us64() / 1000U;
}

/**
 * @brief QWORD → lo/hi 两个 DWORD（显式小端写入）
 *
 * @param lo 低 32 位
 * @param hi 高 32 位
 * @param v  值
 */
static void lifetime_qword_store(DWORD *lo, DWORD *hi, QWORD v)
{
    *lo = (DWORD)v;
    *hi = (DWORD)(v >> 32U);
}

/**
 * @brief lo/hi 两个 DWORD → QWORD
 *
 * @param lo 低 32 位
 * @param hi 高 32 位
 * @return 值
 */
static QWORD lifetime_qword_load(DWORD lo, DWORD hi)
{
    return ((QWORD)hi << 32U) | (QWORD)lo;
}

/**
 * @brief 计算 lifetime 块的 CRC32（crc32 字段按 0 参与）
 *
 * @param disk 块
 * @return CRC32
 */
static DWORD lifetime_crc(const lifetime_disk_s *disk)
{
    lifetime_disk_s tmp = *disk;

    tmp.crc32 = 0U;
    return crc_compute_iso_hdlc(&tmp, (DWORD)sizeof(tmp));
}

/**
 * @brief 块是否有效（magic / version / CRC）
 *
 * @param disk 块
 * @return true 有效
 */
static BOOL lifetime_disk_valid(const lifetime_disk_s *disk)
{
    return ((disk->magic == LIFETIME_MAGIC) && (disk->version == LIFETIME_VERSION) && (disk->crc32 == lifetime_crc(disk))) ? true : false;
}

/**
 * @brief 从一条 CP Header 的 lifetime 块加载本实例计数
 *
 * @param inst 实例号
 * @param disk 块
 */
void lifetime_init(BYTE inst, const lifetime_disk_s *disk)
{
    ASSERT(disk != NULL);
    ASSERT(inst < LIFETIME_INST_MAX);

    if ((disk == NULL) || (inst >= LIFETIME_INST_MAX))
    {
        return;
    }

    if (lifetime_disk_valid(disk) == false)
    {
        // 旧介质 / 首次升级 / 块损坏：本实例计数从 0 开始；CP 记录本身仍然有效。
        LOGW(LOG_TAG_STAT "lifetime: no valid block on inst=%hhu; counters start at 0\r\n", inst);
        memset(&s_inst[inst], 0, sizeof(s_inst[inst]));
        return;
    }

    s_inst[inst].read_bytes         = lifetime_qword_load(disk->read_bytes_lo, disk->read_bytes_hi);
    s_inst[inst].write_bytes        = lifetime_qword_load(disk->write_bytes_lo, disk->write_bytes_hi);
    s_inst[inst].ecc_corrected_cmds = disk->ecc_corrected_cmds;
    s_inst[inst].unc_events         = disk->unc_events;
    s_inst[inst].program_fail       = disk->program_fail;
    s_inst[inst].erase_fail         = disk->erase_fail;

    // controller-wide 字段按 max 合并：多 die 的 CP 记录可能略有先后。
    const QWORD power = lifetime_qword_load(disk->power_on_ms_lo, disk->power_on_ms_hi);

    if (power > s_global.power_on_base_ms)
    {
        s_global.power_on_base_ms = power;
    }
    if (disk->boot_count > s_global.boot_count)
    {
        s_global.boot_count = disk->boot_count;
    }
    if (disk->pfr_count > s_global.pfr_count)
    {
        s_global.pfr_count = disk->pfr_count;
    }
    for (BYTE i = 0U; i < LIFETIME_CMD_NUM; i++)
    {
        if (disk->host_cmd[i] > s_global.host_cmd[i])
        {
            s_global.host_cmd[i] = disk->host_cmd[i];
        }
    }
}

/**
 * @brief 本 boot 的 once 初始化：启动上电计时、boot_count++、PFR 计数
 *
 * @param pfr true = 本次开机走了掉电恢复
 */
void lifetime_boot(BOOL pfr)
{
    if (s_boot_started == false)
    {
        s_boot_started  = true;
        s_boot_start_ms = lifetime_now_ms();
        s_global.boot_count++;
    }

    if ((pfr != false) && (s_pfr_counted == false))
    {
        s_pfr_counted = true;
        s_global.pfr_count++;
    }
}

/**
 * @brief 生成本实例要写入 CP Header 的 lifetime 块（含 CRC）
 *
 * @param inst 实例号
 * @param out  输出块
 */
void lifetime_snapshot(BYTE inst, lifetime_disk_s *out)
{
    ASSERT(out != NULL);
    ASSERT(inst < LIFETIME_INST_MAX);

    if ((out == NULL) || (inst >= LIFETIME_INST_MAX))
    {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->magic   = LIFETIME_MAGIC;
    out->version = LIFETIME_VERSION;

    lifetime_qword_store(&out->read_bytes_lo, &out->read_bytes_hi, s_inst[inst].read_bytes);
    lifetime_qword_store(&out->write_bytes_lo, &out->write_bytes_hi, s_inst[inst].write_bytes);
    out->ecc_corrected_cmds = s_inst[inst].ecc_corrected_cmds;
    out->unc_events         = s_inst[inst].unc_events;
    out->program_fail       = s_inst[inst].program_fail;
    out->erase_fail         = s_inst[inst].erase_fail;

    QWORD power = s_global.power_on_base_ms;

    if (s_boot_started != false)
    {
        const QWORD now = lifetime_now_ms();

        if (now >= s_boot_start_ms)
        {
            power += now - s_boot_start_ms;
        }
    }
    lifetime_qword_store(&out->power_on_ms_lo, &out->power_on_ms_hi, power);
    out->boot_count = s_global.boot_count;
    out->pfr_count  = s_global.pfr_count;
    for (BYTE i = 0U; i < LIFETIME_CMD_NUM; i++)
    {
        out->host_cmd[i] = s_global.host_cmd[i];
    }

    out->crc32 = lifetime_crc(out);
}

/**
 * @brief 读当前 host 读/写字节总数（所有实例求和）
 *
 * @param read_bytes  输出：累计读字节
 * @param write_bytes 输出：累计写字节
 */
void lifetime_get_bytes(QWORD *read_bytes, QWORD *write_bytes)
{
    ASSERT(read_bytes != NULL);
    ASSERT(write_bytes != NULL);

    QWORD read_total  = 0U;
    QWORD write_total = 0U;

    for (BYTE i = 0U; i < LIFETIME_INST_MAX; i++)
    {
        read_total += s_inst[i].read_bytes;
        write_total += s_inst[i].write_bytes;
    }

    *read_bytes  = read_total;
    *write_bytes = write_total;
}

/** @brief host 成功完成的 I/O 字节数（per-instance） */
void lifetime_note_host(BYTE inst, BOOL write, DWORD bytes)
{
    if (inst >= LIFETIME_INST_MAX)
    {
        return;
    }

    if (write != false)
    {
        s_inst[inst].write_bytes += bytes;
    }
    else
    {
        s_inst[inst].read_bytes += bytes;
    }
}

/** @brief host 命令成功完成计数（controller-wide） */
void lifetime_note_cmd(lifetime_cmd_e cmd)
{
    if (cmd < LIFETIME_CMD_NUM)
    {
        s_global.host_cmd[cmd]++;
    }
}

/** @brief 读命令的 ECC 健康（per-instance） */
void lifetime_note_read_health(BYTE inst, DWORD ecc_max, BOOL unc)
{
    if (inst >= LIFETIME_INST_MAX)
    {
        return;
    }

    if (ecc_max > 0U)
    {
        s_inst[inst].ecc_corrected_cmds++;
    }
    if (unc != false)
    {
        s_inst[inst].unc_events++;
    }
}

/** @brief 页编程失败（per-instance） */
void lifetime_note_program_fail(BYTE inst)
{
    if (inst < LIFETIME_INST_MAX)
    {
        s_inst[inst].program_fail++;
    }
}

/** @brief 块擦除失败（per-instance） */
void lifetime_note_erase_fail(BYTE inst)
{
    if (inst < LIFETIME_INST_MAX)
    {
        s_inst[inst].erase_fail++;
    }
}

/**
 * @brief 打印终身计数（聚合 per-instance + controller-wide）
 *
 * @param inst_count 有效实例数
 */
void lifetime_print(BYTE inst_count)
{
    QWORD read_bytes  = 0U;
    QWORD write_bytes = 0U;
    DWORD ecc_cmds    = 0U;
    DWORD unc         = 0U;
    DWORD prog_fail   = 0U;
    DWORD erase_fail  = 0U;

    for (BYTE i = 0U; (i < inst_count) && (i < LIFETIME_INST_MAX); i++)
    {
        read_bytes += s_inst[i].read_bytes;
        write_bytes += s_inst[i].write_bytes;
        ecc_cmds += s_inst[i].ecc_corrected_cmds;
        unc += s_inst[i].unc_events;
        prog_fail += s_inst[i].program_fail;
        erase_fail += s_inst[i].erase_fail;
    }

    const QWORD now_ms = lifetime_now_ms();
    QWORD       power  = s_global.power_on_base_ms;

    if ((s_boot_started != false) && (now_ms >= s_boot_start_ms))
    {
        power += now_ms - s_boot_start_ms;
    }

    LOGI(LOG_TAG_STAT "smart bytes read=%llu write=%llu\r\n", read_bytes, write_bytes);
    LOGI(LOG_TAG_STAT "smart ecc_cmd=%lu unc=%lu prog_fail=%lu erase_fail=%lu\r\n", ecc_cmds, unc, prog_fail, erase_fail);
    LOGI(LOG_TAG_STAT "smart power_on=%llus boots=%lu pfr=%lu\r\n", power / 1000U, s_global.boot_count, s_global.pfr_count);
    LOGI(LOG_TAG_STAT "smart host_cmd r=%lu w=%lu f=%lu t=%lu\r\n", s_global.host_cmd[LIFETIME_CMD_READ], s_global.host_cmd[LIFETIME_CMD_WRITE], s_global.host_cmd[LIFETIME_CMD_FLUSH], s_global.host_cmd[LIFETIME_CMD_TRIM]);

    for (BYTE i = 0U; (i < inst_count) && (i < LIFETIME_INST_MAX); i++)
    {
        LOGI_IF((s_inst[i].read_bytes != 0U) || (s_inst[i].write_bytes != 0U) || (s_inst[i].ecc_corrected_cmds != 0U) || (s_inst[i].unc_events != 0U) || (s_inst[i].program_fail != 0U) || (s_inst[i].erase_fail != 0U), LOG_TAG_STAT "smart[%hhu] read=%llu write=%llu ecc_cmd=%lu unc=%lu prog_fail=%lu erase_fail=%lu\r\n", i, s_inst[i].read_bytes, s_inst[i].write_bytes, s_inst[i].ecc_corrected_cmds, s_inst[i].unc_events, s_inst[i].program_fail, s_inst[i].erase_fail);
    }
}

#endif /* SSD_STATS_PERSIST_ENABLE */
