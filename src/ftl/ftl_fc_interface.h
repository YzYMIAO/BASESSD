#ifndef FTL_FC_INTERFACE_H
#define FTL_FC_INTERFACE_H

/*
 * FTL-FC Interface 层
 * ===================
 *
 * 【desc 链组装】
 *   ① ftl_cmd_build_descriptors(cmd, cw_bitmap) — 按形参 cw_bitmap 建整条 desc 链
 *   ③ ftl_cmd_build_ppa_pair_cw() — 每 plane 各自的 PBA + 显式位图 → 建链（Policy B）
 *   ④ pba_to_cmd() — DWORD 直填 + 整段位图 + 建链（遍历路径 / 同步页原语）
 *      ⑩ ftl_cmd_pba() — FC 命令 → PBA（④ 的逆，供 FTL 模块回读自己下发的地址）
 *
 *   位图规则（①③ 共用，唯一定义在 ftl_cmd_check_cw_bitmap）：
 *     · cw_bitmap 是**字面位图**（bit 置 1 = 要访问该 CW；0 = 空集，无隐式展开）；
 *     · ③④ 两个「整段访问」入口各自显式算 fc_command_plane_span_cw_bitmap(cmd)，
 *       只有 ① 接受调用者给的部分位图（空集 / 越段会被校验函数拒绝）；
 *     · 位图是建链的**输入**，不存进命令结构体（③④ 算好后直接传参）；
 *     · config.cw_count 不参与建链（仍被 FC 的 RAID stripe 读取，见 fc_interface.h）。
 *
 * 【数据搬运】
 *   ⑤ ftl_cmd_write_data() — 用户数据写入 desc 链各 CW 的 USER 区
 *   ⑥ ftl_cmd_read_data() — 从 desc 链各 CW 的 USER 区拷出
 *
 * 【同步等待】
 *   ⑦ ftl_sync_submit_wait() — 提交命令并阻塞等待其完成（ftl_op 机制的客户端）
 *
 * 【页级同步原语】
 *   ⑧ ftl_sync_write_page / ftl_sync_read_page / ftl_sync_read_page_raw /
 *      ftl_sync_erase_block / ftl_sync_erase_block_multi_plane / ftl_sync_read_column /
 *      ftl_sync_write_page_mirror — 差异由 ftl_page_cfg_s（crc/random/ldpc/lba）承载。
 *
 * 【异步页级原语】（提交与完成解耦）
 *   ⑩ ftl_async_write_page / ftl_async_write_page_cw / ftl_async_erase_block /
 *      ftl_async_erase_block_multi_plane /
 *      ftl_async_read_page / ftl_async_set_features — 只建链 + 搬数据，**不提交**；
 *      命令交回调用者，由调用者 ftl_op_submit(cmd, <功能 op>, ctx)。服务函数负责
 *      fc_command_release。
 *      写/擦与 ⑧ 共用同一套 prepare 模板；**读**的数据留在 desc 链上（非 prepare 拷贝）
 *      ⇒ 目标缓冲须活到完成、ftl_cmd_read_data 须早于 release；SET_FEATURES 无 PBA
 *      ⇒ ch/ce/lun 只能显式给（完成派发的实例路由键）。
 *
 * 【命令身份】
 *   ⑨ ftl_cmd_id() — 命令指针 → FC 命令池 ID（读命令对象的 cmd_id，见 fc_interface.h）。
 *
 * 为什么这些在本层：FC 定位为**硬件 FCC 的软件替身**——只提供槽位/描述符/
 * 队列/数据路径，不含策略。而「访问哪些 CW」（掩码枚举）与「数据怎么搬」（拷贝）
 * 在行业里都是**固件侧**的事（硬件 FCC 既不提供「按掩码建链」寄存器，也不提供
 * 「数据拷贝」寄存器——描述符链是固件填的、数据是固件 DMA 填的），故落在本层。
 */

#include "assertion.h"
#include "fc_interface.h" /* 池/队列深度等模块常量 */
#include "ftl_config.h"   /* DWORD, pba_to_paa */
#include "ftl_type.h"
#include "nand_config.h"
#include "onfi_1_0.h"

#include <stdint.h>

/* ── host 资源预算：给后台（CP / GC / journal / 元数据）预留 FC 池 ──
 *
 * 一个 4KB host 子操作最多占 FTL_LBA_PER_PAIR（8）个 desc/buffer；FC buffer 池只有
 * 128 ⇒ 16 个在飞 host 写就能吃满。后台（CP 的 region flush / GC 的搬移 / journal 页）
 * 此时拿不到命令与 buffer ⇒ CP 无法写 Header / 块表 ⇒ 掉电后每次开机都走全量重建
 * 。这里让 host 写在池接近满时先背压，保留 4 个 pair 规模
 * 的后台资源。
 *
 * ⚠ **所有 host 路径**（含 cache 读/写与 wbuf）都必须走本检查：host 突发能把命令/
 * desc/buffer 吃成 `cmd=0/buf=0`，使 CP/META/L2P 无法提交 ⇒ confirm park timeout
 * + 已回执写丢失。预算取 4 pair，保证后台始终有提交配额。
 *
 * 判据：host 的**下一次**子操作之后，各池仍至少剩 RESERVED 个槽位。
 * ⚠ 只约束 host 路径；后台路径（CP/GC/journal）直接走各自的 async 原语，不经过本检查。
 * ⚠ host 不使用 HSQ（只有 read recovery 走 HSQ），故 HSQ 不参与本预算。 */
#define FTL_FC_HOST_RESERVED_CMDS (8U)

#define FTL_FC_HOST_RESERVED_DESC (4U * FTL_LBA_PER_PAIR)

#define FTL_FC_HOST_RESERVED_BUFS (4U * FTL_LBA_PER_PAIR)

#define FTL_FC_HOST_RESERVED_SQ (8U)

/* ── CP/metadata 的 FC 资源保底（FTL 侧）──
 *
 * FC 已定型：CRIT 类内部不区分 L2P load/flush 与 CP/metadata，谁先占谁赢。
 * L2P 路径在预检时给 CP/metadata 留出这些槽位，避免把 CP 的最后一个命令/页
 * 资源吃光（现场：CP submit stalled cmd=1 buf=8）。
 * 数值是本仓自拟：2 命令 ≈ 一次 CP/metadata 提交 + 1 在飞余量；
 * 8 desc/buf ≈ 2 个 metadata 页（每页 CW_NUM=4）。 */
#define FTL_FC_CP_CMD_HEADROOM (2U)

#define FTL_FC_CP_DESC_HEADROOM (8U)

#define FTL_FC_CP_BUFS_HEADROOM (8U)

/* 「本页 LBA 未知」哨兵：只传给 FTL_PAGE_CFG_DATA_ANY。
 * 页 LBA 的有效上界 = L2P_REGIONS_PER_INSTANCE × L2P_ENTRIES_PER_REGION（= 2^20），
 * 故 0xFFFFFFFF 不可能是一个真实页 LBA。 */
#define FTL_LBA_UNKNOWN (0xFFFFFFFFUL)

#define FTL_PAGE_CFG_META() (&(const ftl_page_cfg_s){FC_CRC_ENABLE, FC_RANDOM_DISABLE, FC_ECC_MODE_NORMAL, 0U})

#define FTL_PAGE_CFG_DATA(lba) (&(const ftl_page_cfg_s){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_NORMAL, (lba)})

/* 数据页但**页 LBA 未知**（Read Recovery 的邻近页辅助读）：CRC/加扰照常开，但命令的
 * META 填 FC_META_UNKNOWN ⇒ 跳过 FC 侧「页内 META 与本命令 LBA 比对」这一项，只保留
 * plane/cw 三位自洽校验（见 fc_data_path_meta_check）。
 * ⚠ 这类读**不要**用 FTL_PAGE_CFG_DATA(0) 代替：那是"本页 LBA 就是 0"，
 *   比对项会拿 0 去撞页内真实 META ⇒ 恒置 META_ERROR（若忽略错误位会看不出来）。 */
#define FTL_PAGE_CFG_DATA_ANY() (&(const ftl_page_cfg_s){FC_CRC_ENABLE, FC_RANDOM_ENABLE, FC_ECC_MODE_NORMAL, FTL_LBA_UNKNOWN})

/* 一个命令实际访问到的**全局块**及其本命令内的最大 ECC 纠正 bit 数。 */
typedef struct FTL_COMMAND_BLOCK_ECC_S
{
    WORD gblock;
    BYTE ecc_max;
} ftl_cmd_block_ecc_s;

/* ── 页级同步原语 ──
 *
 * 各原语的差异由 ftl_page_cfg_s 承载（crc / random / ldpc / lba）；
 * 配置是编译期常量，故用复合字面量宏调用：
 *
 *   元数据页（BBT / checkpoint / journal / L2P / metadata / 擦除）：
 *     ftl_sync_write_page(pba, buf, len, FTL_PAGE_CFG_META());
 *   用户数据页（GC / WL 搬移、Read Recovery 重读）：
 *     ftl_sync_write_page(pba, buf, len, FTL_PAGE_CFG_DATA(lba));
 *   裸页（LDPC_RAW，忽略 NAND 成败）：
 *     ftl_sync_read_page_raw(pba, buf);
 *   擦块（20ms 超时，元数据配置）：
 *     ftl_sync_erase_block(pba);
 *
 * 共同契约：返回 FTL_ERROR 时命令**已释放**、buffer 内容未定义；调用者不应再
 * 自行 fc_command_release。共同的命令配置：单 plane、FC_CHAIN_END、LP 页、
 * CW 0..3（擦除为 0，无数据链）。
 *
 * 【成败判据】返回 FTL_OK ⇒ 页内容**可信**；判据是 FC_CMD_STATUS_ERROR_MASK 全集
 * （NAND_FAIL / FAIL_CACHE / TIMEOUT / DATA_ERROR / META_ERROR 任一置位即 FTL_ERROR；
 * UNC/CRC/RAID 合并为 DATA_ERROR），与 fc_interface.h 的链级判据同口径。两个例外：ftl_sync_read_page_raw
 * 与 ftl_sync_read_column 刻意**不看**这些位（坏页的原始位与 spare 正是它们的
 * 返回物），成败由调用者按内容自判。
 */

typedef struct FTL_PAGE_CONFIGURATION_S
{
    BYTE  crc;    /* FC_CRC_ENABLE / FC_CRC_DISABLE */
    BYTE  random; /* FC_RANDOM_ENABLE / FC_RANDOM_DISABLE */
    BYTE  ldpc;   /* FC_ECC_MODE_NORMAL / FC_ECC_MODE_LDPC_RAW；0 = normal，不是“BCH 模式” */
    DWORD lba;    /* META 源：**页** LBA（建链时 << CW_WIDTH 成扇区 LBA）；元数据页传 0 */
} ftl_page_cfg_s;

/* 命令租约：当前只租 command slot；desc/buffer 仍由建链路径按同一 class 取用，
 * 全资源 lease 尚未收口。 */
typedef struct FTL_CMD_LEASE_S
{
    fc_cmd_s      *cmd;
    fc_cmd_class_e cls; /* 建链时 desc/buffer 跟随的 class */
} ftl_cmd_lease_s;

/**
 * @brief host 是否还有余量提交 n 个子操作。
 *
 * 逐池检查：受理本批 n 个子操作后，命令 / desc / buffer / SQ 仍各留
 * FTL_FC_HOST_RESERVED_* 个槽位。
 *
 * @param n 本批子操作数（命令 / SQ 各占 n 个槽，desc / buffer 各占 n × FTL_LBA_PER_PAIR 个）
 * @return true 余量足够；false 应背压 host 写
 */
static inline BOOL ftl_fc_host_budget_ok_n(DWORD n)
{
    fc_resource_stats_s st;

    fc_get_resource_stats(&st);

    const DWORD cw = n * FTL_LBA_PER_PAIR;

    return (st.command_used + n + FTL_FC_HOST_RESERVED_CMDS <= FC_COMMAND_POOL_DEPTH) && (st.descriptor_used + cw + FTL_FC_HOST_RESERVED_DESC <= FC_DESCRIPTOR_POOL_DEPTH) && (st.buffer_used + cw + FTL_FC_HOST_RESERVED_BUFS <= FC_BUFFER_POOL_DEPTH) && (st.sq_used + n + FTL_FC_HOST_RESERVED_SQ <= FC_SQ_DEPTH);
}

/**
 * @brief host 是否还有余量提交一个子操作。查询失败时不阻塞前台（宁可少一次预留）。
 *
 * @return true 余量足够；false 应背压 host 写
 */
static inline BOOL ftl_fc_host_budget_ok(void)
{
    return ftl_fc_host_budget_ok_n(1U);
}

/* ── desc 链组装共用小工具 ──
 *   plane 段规则用 fc_interface.h 的 fc_command_plane_span（唯一一份定义，
 *   FC 的越段校验与 FTL 的掩码展开共用）。 */

/**
 * @brief 命令指针 → FC 命令池 ID。
 *
 * 命令池 ID 保存在 fc_cmd_s.cmd_id（见 fc_interface.h）。本层调用者手里的 cmd
 * 恒来自 ftl_cmd_alloc_class()（经 ftl_cmd_prepare_alloc），故 ID 必然有效；池外指针（栈上的 fc_cmd_s）得到
 * FC_CMD_ID_INVALID——那是编程错误，调用方（pending_add / sync_wait）会因查不到
 * 而失败，不会静默串命令。
 *
 * @param cmd 命令指针（本层调用者恒来自 ftl_cmd_alloc_class()（经 ftl_cmd_prepare_alloc））
 * @return FC 命令池 ID；池外指针返回 FC_CMD_ID_INVALID
 */
static inline WORD ftl_cmd_id(const fc_cmd_s *cmd)
{
    ASSERT(cmd != NULL);
    return cmd->cmd_id;
}

/**
 * @brief 该操作是否需要 desc 链（即是否携带数据）
 *
 * NO_DATA 操作（ERASE / RESET / LOCK 等）无数据负载；SET/GET_FEATURES 用内联
 * param.value + feature_addr。两者都不建链（desc_id 保持 INVALID）。
 * 与 fc_command_validate 的接受条件对齐：只有 has_data 且非 FEATURES 的操作
 * 才要求 desc_id 有效。
 *
 * @param cmd 已填好 config.operation 的命令
 * @return true 需要 desc 链；false 无数据负载（ERASE / RESET / LOCK / FEATURES 等）
 */
static inline BOOL ftl_cmd_needs_desc_chain(const fc_cmd_s *cmd)
{
    const BYTE op = cmd->config.operation;

    return (nand_operation_has_data(op) != 0U) && (op != OP_SET_FEATURES) && (op != OP_GET_FEATURES);
}

/* ── plane 数 → 命令头字段（读/写各一）──
 *
 * 页读/写命令的 operation 与 config.multi_plane 都由「本子操作覆盖几个 plane」决定。
 * 两者**成对**设置，故收敛在这里，调用点只写一行：
 *
 *     cmd->config.operation   = ftl_plane_write_op(nplanes);
 *     cmd->config.multi_plane = ftl_plane_multi_field(nplanes);
 *
 * multi_plane 字段携带**真实 plane 数**（1 → 0 = 单 plane 语义；N>1 → N）——下游
 * fc_command_plane_span（plane 段）与 fc.c 的 all_planes 掩码都按它算，**不假设 2**。
 * 本器件（MT29F4G08，ONFI 1.0）的 opcode 表里只有「单 plane」与「多 plane」两种页读/
 * 写码（onfi_1_0.c 的 g_nand_multi_plane_*_uops 按 2 plane 定尺）；4/6 plane 器件须
 * **先在 opcode 表加码**，再在此处扩展——这里泛化的是**形状**（plane 数不写死、界取自
 * PLANE_NUM），不是凭空造出不存在的 opcode。 */

/**
 * @brief plane 数 → config.multi_plane 字段。
 * 纯打包：nplanes 的值域由调用点紧邻的 ftl_plane_*_op 断言（同一个 nplanes）。
 *
 * @param nplanes 本子操作覆盖的 plane 数
 * @return 1 → 0（单 plane 语义）；N > 1 → N
 */
static inline BYTE ftl_plane_multi_field(BYTE nplanes)
{
    return ((nplanes > 1U) ? nplanes : 0U);
}

/**
 * @brief plane 数 → 读 operation 码。界取 PLANE_NUM，与 fc_command_plane_span 的上界同源。
 *
 * @param nplanes 本子操作覆盖的 plane 数
 * @return 单 plane → OP_NORMAL_READ_PAGE_WITH_DATA_OUT；多 plane → OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT
 */
static inline BYTE ftl_plane_read_op(BYTE nplanes)
{
    return ((nplanes > 1U) ? OP_READ_PAGE_MULTI_PLANE_WITH_DATA_OUT : OP_NORMAL_READ_PAGE_WITH_DATA_OUT);
}

/**
 * @brief plane 数 → 写 operation 码。界同上。
 *
 * @param nplanes 本子操作覆盖的 plane 数
 * @return 单 plane → OP_NORMAL_PROGRAM_PAGE；多 plane → OP_PROGRAM_PAGE_MULTI_PLANE
 */
static inline BYTE ftl_plane_write_op(BYTE nplanes)
{
    return ((nplanes > 1U) ? OP_PROGRAM_PAGE_MULTI_PLANE : OP_NORMAL_PROGRAM_PAGE);
}

/**
 * @brief cache program 批次第 j 页（共 n 页）使用的 op：
 * 首页 = FIRST（只查 FAIL）、中间页 = CACHE（查 FAIL|FAILC）、末页 = END（10h）。
 *
 * @param j 批次内的页下标（0 起）
 * @param n 批次总页数
 * @return OP_TWO_PLANE_PROGRAM_CACHE_FIRST / _CACHE / _END
 */
static inline BYTE ftl_cache_prog_op(BYTE j, BYTE n)
{
    if (j == 0U)
    {
        return OP_TWO_PLANE_PROGRAM_CACHE_FIRST;
    }
    if ((BYTE)(j + 1U) < n)
    {
        return OP_TWO_PLANE_PROGRAM_CACHE;
    }
    return OP_TWO_PLANE_PROGRAM_CACHE_END;
}

/* ── 地址直填：DWORD → FC 命令（PBA→PAA 的唯一关卡之一）── */

/**
 * @brief FC 命令 → PBA（pba_to_cmd 的逆）
 *
 * FTL 模块要回读自己下发的命令地址时用它，**不必**直接摸 cmd->paa——PAA 是 FC/器件域
 * 的表示。ch/ce 取 cmd->ch / cmd->ce，其余字段由 paa_to_pba() 反重排。
 *
 * ⚠ 本函数与 pba_to_cmd() 是 FTL 层仅有的两处 PBA↔PAA 转换点。
 *
 * @param cmd 命令（ch/ce 取 route，其余字段由 paa 反解）
 * @return 该命令的行地址 PBA
 */
static inline DWORD ftl_cmd_pba(const fc_cmd_s *cmd)
{
    return paa_to_pba(cmd->ch, cmd->ce, cmd->paa);
}

/* ── desc 链组装 ── */

/**
 * @brief 按形参 cw_bitmap 组装整条 desc 链。
 *
 * @note cw_bitmap 是 flat 绝对编号（bit = plane*CW_NUM + cw）的**字面位图**：
 *       0 = 空集（不是「全部」）；链序 = plane-major、plane 内 CW 递增。
 * @note 所有 plane 共用 `cmd->paa` 的行地址（同页语义）；两 plane 不同块
 *       （Policy B）请用 `ftl_cmd_build_ppa_pair_cw`。
 */
ftl_status_e ftl_cmd_build_descriptors(fc_cmd_s *cmd, DWORD cw_bitmap);

/**
 * @brief 设置命令分配类（构造期上下文；返回原值以便恢复）。
 *
 * @note 额度是文件级构造上下文：调用点必须用返回值恢复；单线程 superloop，无并发保护。
 */
fc_cmd_class_e ftl_cmd_class_set(fc_cmd_class_e cls);

/**
 * @brief 按每 plane 各自的 PBA + 显式位图建 desc 链（Policy B）。
 *
 * @note plane 号取自每个 PBA 自己的 plane 位，不是数组下标。
 * @note `cmd->config.multi_plane` 必须由调用者建链前设好；未置位的 plane 整体跳过
 *       （连 pba_is_valid 都不判）。
 */
ftl_status_e ftl_cmd_build_ppa_pair_cw(fc_cmd_s *cmd, const DWORD pba[PLANE_NUM], DWORD cw_bitmap);

/** @brief 逐 plane 反查命令访问的全局块与 ECC（multi-plane 两块必须分别记账）。 */
BYTE ftl_cmd_block_ecc_collect(const fc_cmd_s *cmd, ftl_cmd_block_ecc_s *out, BYTE max);

/**
 * @brief 把外部数据按 CW 顺序写入 desc 链上各 CW 的 USER 区。
 *
 * @note 遍历界走 desc 链（链长度是 CW 数的唯一真值）；数据耗尽后剩余 CW 填 0xFF。
 *       `length` 超过链容量时多余源数据**被忽略**（调用者应保证匹配）。
 */
ftl_status_e ftl_cmd_write_data(fc_cmd_s *cmd, const void *data, DWORD length);

/**
 * @brief CW 级数据填装（按页内 CW 偏移从源镜像散射取数）。
 *
 * @note 两种布局不能合并：wbuf 的 plane1 数据在 `page_buf + 4*512`，而 mirror 的
 *       两个 plane 本来就该相同；误用会让 plane1 复制 plane0（host 大文件静默损坏）。
 */
ftl_status_e ftl_cmd_write_data_cw(fc_cmd_s *cmd, const BYTE *page_buf, DWORD cw_bitmap, BOOL pair_major);

/** @brief 从 desc 链各 CW 的 USER 区按链序拷出到外部缓冲。 */
ftl_status_e ftl_cmd_read_data(fc_cmd_s *cmd, void *buffer, DWORD length);

/**
 * @brief 提交命令并阻塞等待其完成（同步提交的唯一入口）。
 *
 * @note 只在结构性必须顺序执行的路径上用（开机 / 格式化 / 出厂扫描 / CLI）；主循环
 *       路径一律异步化，同步等待会钉住调用者栈帧并重入完成泵。
 * @note 命令归属由 `*submitted` 决定：失败且未提交 ⇒ 调用者必须 release；已提交但
 *       超时 ⇒ 调用者**不得** release / 读数据（迟到完成走 drain 的 missing-request）。
 */
ftl_status_e ftl_sync_submit_wait(fc_cmd_s *cmd, fc_cmd_status_s *status_out, DWORD timeout_us, BOOL *submitted);

/** @brief 同步写页（OP_NORMAL_PROGRAM_PAGE；data/length 为空时只发命令不搬数据）。 */
ftl_status_e ftl_sync_write_page(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);

/**
 * @brief 同步读页（保留完成状态）。
 *
 * @note 供开机恢复区分「提交层失败」与「完成态 DATA_ERROR」：前者快照不可信，
 *       后者可按 L2P 加载重试 / 降级处理。
 */
ftl_status_e ftl_sync_read_page_status(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg, fc_cmd_status_s *completion_out);

/** @brief 同步读页（数据在完成后拷入 buffer）。 */
ftl_status_e ftl_sync_read_page(DWORD pba, void *buffer, DWORD length, const ftl_page_cfg_s *cfg);

/**
 * @brief 裸页读（LDPC_RAW），每 CW 只拷 USER 512B。
 *
 * @note 刻意不判定 NAND 失败（诊断 / 出厂坏块探针要原始位）。
 * @note **不能用来判空**：RAW 跳过 ECC；判空用 `ftl_page_is_blank` /
 *       `ftl_page_blank_check`（解码路径 + FC 的 CW 状态位）。
 */
ftl_status_e ftl_sync_read_page_raw(DWORD pba, void *buffer);

/**
 * @brief 判整页是否从未编程（擦除页）：普通读一次，看 FC 是否把每个 CW 都判为擦除页。
 *
 * @note 判据在解码路径里成立（ECC 成码字 + 1 比特占比 + parity）；读命令没走通 ⇒
 *       一律返回 false（判「非空」，保守方向）。与 full rebuild / BBT 探针 /
 *       Journal 扫页 / L2P 日志区恢复共用唯一实现。
 */
BOOL ftl_page_is_blank(DWORD pba);

/** @brief 同 `ftl_page_is_blank`，但把「命令没走通」与「已编程」分开报告。 */
ftl_status_e ftl_page_blank_check(DWORD pba, BOOL *blank);

/** @brief 擦块（单 plane，OP_ERASE_BLOCK，plane 取自 PBA）。 */
ftl_status_e ftl_sync_erase_block(DWORD pba);

/** @brief 擦块（多 plane，擦同一 block 号在两个 plane 上的物理块；PBA 的 plane 字段被忽略）。 */
ftl_status_e ftl_sync_erase_block_multi_plane(DWORD pba);

/* ── 异步页级原语（提交与完成解耦）──
 *
 * 与 ftl_sync_* 同一 prepare 模板，差别只有最后一步：**不提交**，把已建链、
 * 已装好数据的命令交回调用者：
 *   ftl_async_* → 调用者 ftl_op_submit(cmd, <功能 op ID>, ctx)
 *               → g_op_callbacks[<op ID>] 服务函数判完成状态 + release
 *
 * ⚠ 数据在 prepare 阶段就已拷进 FC buffer 池 ⇒ 返回后调用者即可复用源缓冲。
 * ⚠ 调用者拿到 cmd 后**必须**提交或释放；泄漏命令槽位 ⇒ 全系统 FTL_AGAIN。
 * ⚠ 失败返回时命令已自行回收，out_cmd 不被写入。 */

/** @brief 异步写页（建链 + 装数据，不提交）。 */
ftl_status_e ftl_async_write_page(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);

/** @brief 用已预留的命令槽建链并装数据（双副本写回；失败时命令已回收）。 */
ftl_status_e ftl_async_write_page_reserved(fc_cmd_s *cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);

/** @brief 异步擦块（单 plane；建链，不提交）。 */
ftl_status_e ftl_async_erase_block(fc_cmd_s **out_cmd, DWORD pba);

/** @brief 异步擦块（多 plane；同 `ftl_async_erase_block`，plane 字段被忽略）。 */
ftl_status_e ftl_async_erase_block_multi_plane(fc_cmd_s **out_cmd, DWORD pba);

/**
 * @brief 元数据镜像写（多 plane 同页同数据，不提交）。
 *
 * @note 只给**固定元数据池**用；两个 plane 的 page 必须都处于擦除态。
 *       `length < PAGE_MAIN_SIZE` 时用 0xFF 补齐到一页。
 */
ftl_status_e ftl_async_write_page_mirror(fc_cmd_s **out_cmd, DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);

/** @brief 镜像写的同步版（建链 + 提交等待 + 判错）。 */
ftl_status_e ftl_sync_write_page_mirror(DWORD pba, const void *data, DWORD length, const ftl_page_cfg_s *cfg);

/**
 * @brief 写页（CW 级，异步）：只把 page_buf 里 cw_bitmap 指定的 CW 编进目标页。
 *
 * @note 数据在本函数内已拷进 FC buffer 池 ⇒ 返回后调用者即可复用 page_buf。
 * @note cw_bitmap 须落在本命令 plane 段内；越段是编程错误 → ASSERT。
 */
ftl_status_e ftl_async_write_page_cw(fc_cmd_s **out_cmd, DWORD pba, const BYTE *page_buf, DWORD cw_bitmap, const ftl_page_cfg_s *cfg);

/**
 * @brief 写 pair 页（CW 级，异步，显式 op；cache program 批次用）。
 *
 * @note pair_buf 布局 = plane0 的 PAGE_MAIN_SIZE 字节 + plane1 的 PAGE_MAIN_SIZE
 *       字节；两个 pba 必须同 ch/ce/lun；多 plane 链须覆盖每个 plane。
 */
ftl_status_e ftl_async_write_pair_cmd(fc_cmd_s **out_cmd, const DWORD pba[PLANE_NUM], const BYTE *pair_buf, DWORD cw_bitmap, BYTE operation, const ftl_page_cfg_s *cfg);

/** @brief 申请一个命令租约（构造期 class 额度）。 */
BOOL ftl_cmd_lease_claim(ftl_cmd_lease_s *lease, fc_cmd_class_e cls);

/** @brief 按 class 取一条命令（内部经 command lease；调用者负责 release）。 */
fc_cmd_s *ftl_cmd_alloc_class(fc_cmd_class_e cls);

/** @brief 释放未提交的命令租约。 */
void ftl_cmd_lease_release(ftl_cmd_lease_s *lease);

/** @brief 取走租约里的命令（所有权转交调用者）。 */
fc_cmd_s *ftl_cmd_lease_take(ftl_cmd_lease_s *lease);

/**
 * @brief 读页（异步，建链，不提交）。
 *
 * @note 与写**不对称**：读数据留在命令的 desc 链上，只有服务函数在 release **之前**
 *       调 `ftl_cmd_read_data` 才拿得到 ⇒ 调用者的目标缓冲必须在命令完成之前保持有效。
 *       成败判据也搬到服务函数（同步版见 `ftl_sync_read_page_status`）。
 */
ftl_status_e ftl_async_read_page(fc_cmd_s **out_cmd, DWORD pba, const ftl_page_cfg_s *cfg);

/** @brief 用已租约的命令建链并异步读页（成功时所有权转交调用者）。 */
ftl_status_e ftl_async_read_page_leased(fc_cmd_s **out_cmd, ftl_cmd_lease_s *lease, DWORD pba, const ftl_page_cfg_s *cfg);

/**
 * @brief SET_FEATURES（异步）：无地址、无数据链。
 *
 * @note 本仓 Read Recovery **不使用**该入口：0x89 在本器件是保留地址、SLC 也无 retry 表。
 * @note 本命令不绑 PBA（`cmd->paa` 恒 0）；ch/ce/lun 是**完成派发的实例路由键**，
 *       必须显式传——留 0 在多实例下会把完成派给实例 [0][0][0]。
 */
ftl_status_e ftl_async_set_features(fc_cmd_s **out_cmd, BYTE ch, BYTE ce, BYTE lun, BYTE feature_addr, BYTE mode);

/**
 * @brief 按列读（OP_READ_COLUMN）：回读页内从 `column` 起的 4 个字节。
 *
 * @note 与 `ftl_sync_read_page_raw` 同契约：刻意不判定 NAND 失败（坏块 spare 也要
 *       拿到），命令完成即返回 FTL_OK，成败由调用者按 out[] 内容判。
 */
ftl_status_e ftl_sync_read_column(DWORD pba, WORD column, BYTE out[4]);

/**
 * @brief 地址直填：把 PBA 写进 FC 命令并建 desc 链（PBA→PAA 的唯一关卡之一）。
 *
 * @note 地址填好后必须建 desc 链；无数据操作由建链入口短路。PBA 须通过
 *       `pba_is_valid`（进 FC 的 ch/ce/lun 被当数组下标直接用）。
 */
ftl_status_e pba_to_cmd(fc_cmd_s *cmd, DWORD pba);

#endif /* FTL_FC_INTERFACE_H */
