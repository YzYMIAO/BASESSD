#ifndef FC_DATA_PATH_H
#define FC_DATA_PATH_H

#include "fc_types.h"
#include "nand_config.h" /* PLANE_NUM / DWORD */

/*
 * FC 数据路径管线接口。
 *
 * ============== 写路径 (encode) ==============
 *
 * fc_data_path_encode()
 *   全自动管线：Random 加扰 → ECC 编码 → RAID XOR。
 *   FC 内部在 NAND Program 前调用，对 FTL 透明。
 *
 * ============== 读路径 (decode) ==============
 *
 * 读路径分为两个阶段，分别由不同模块驱动：
 *
 * Stage 1 — fc_data_path_process_read()  [FC 内部]
 *   NAND Read 完成后、CQ push 之前，FC 自动调用。
 *   对每个 CW：ECC 解码 → 更新 descriptor->status。
 *     - 所有 CW 都 ECC 成功 → 自动解扰所有 buffer，推 CQ。
 *     - 存在 ECC 失败的 CW → 不解扰任何 buffer，推 CQ。
 *       此时非失败 CW 处于「已 ECC 纠正但仍加扰」状态，
 *       以便 FTL 后续的 RAID XOR 恢复仍在 scrambled domain 进行。
 *
 * Stage 2 — fc_data_path_raid_recover()  [FTL 触发]
 *   FTL 从 CQ 拿到 completion 后，遍历 desc 链发现 status=UNC，
 *   决定走 RAID 恢复时调用。
 *   内部：XOR 重建 → ECC 重试 → 所有 CW 解扰 → 更新 descriptor status。
 *
 * 这种设计保证了：
 *   ① 加解扰对 FTL 完全透明
 *   ② RAID XOR 在正确的 scrambled domain 进行
 *   ③ FTL 对恢复策略有完全控制权（RAID / read retry / BBT 等）
 */

/* ── 写路径（FC 内部） ── */

/**
 * @brief 全自动写管线：META 写入 → CRC → 加扰 → ECC 编码。
 * FTL 提交命令后、dispatch 前由 FC 自动调用（对 FTL 透明）。
 *
 * @param command 待编码的命令；沿其 desc 链逐 CW 执行上述步骤。
 *
 * @return FC_STATUS_OK  整条 desc 链编码完成。
 *         ECC 错误码    ECC 编码失败时透传引擎返回码。
 */
fc_status_e fc_data_path_encode(const fc_command_t *command);

#if CONFIG_FC_RAID
/* ── 跨 Plane Stripe RAID parity 生成（FC 内部） ── */

/**
 * @brief 对 PLANE_NUM-1 条数据命令的对应 CW 数据区做跨命令 XOR，
 * 结果写入 parity 命令的 buffer，然后对 parity buffer 做 ECC 编码。
 *
 * @param data_cmds  数据命令指针数组（非空），长度 = data_count。
 * @param data_count 数据命令数量（= PLANE_NUM - 1，非 0）。
 * @param parity_cmd 已分配 desc+buffer 的 parity 命令，接收 XOR 结果。
 *
 * @return FC_STATUS_OK  parity 生成并编码完成。
 *         ECC 错误码    parity buffer 的 ECC 编码失败时透传引擎返回码。
 *
 * @note FC 内部在 stripe 累积完成后、提交 dispatch 前自动调用。
 *       参数 / desc 链损坏的前置违反一律 ASSERT，不返回错误码。
 */
fc_status_e fc_data_path_generate_stripe_parity(const fc_command_t *data_cmds[], BYTE data_count, const fc_command_t *parity_cmd);
#endif /* CONFIG_FC_RAID */

/* ── 读路径 Stage 1：ECC 解码（FC 内部，CQ push 前） ── */

/**
 * @brief 对命令 desc 链上的每个 CW 做 ECC 解码，并按结果更新 descriptor status。
 *
 * 全部 CW 解码成功时解扰全部 buffer；任一 CW 不可纠时不解扰任何 buffer，
 * buffer 停在 scrambled + ECC-corrected 状态，供 FTL 的 RAID 恢复在
 * scrambled domain 继续处理。
 *
 * @param command 已完成 NAND 读取的命令。
 *
 * @return FC_STATUS_OK       所有 CW 无不可纠错误（擦除页与 NOT_SUPPORTED 按成功处理），buffer 已解扰。
 *         FC_STATUS_IO_ERROR 存在不可纠的 CW（未解扰，需 FTL 处理）。
 *
 * @note 整条 CW 的 1 比特占比 ≥ 99% 的擦除页记为 FC_CW_STATUS_EMPTY 而非失败，跳过解扰 / CRC / META 校验。
 *       判据只在**解码路径**成立：RAW 读（LDPC_RAW）跳过 ECC，不能用它判断空/非空。
 *       仅在执行 ECC 解码的路径生效；`FC_ECC_MODE_LDPC_RAW` 跳过 ECC ⇒ 不做该识别，
 *       擦除页按 CRC/META 失败上报（RAW 读的调用方自行判空）。
 */
fc_status_e fc_data_path_process_read(const fc_command_t *command);

/* ── 读路径 Stage 2：RAID 恢复（FTL 触发） ── */

/**
 * @brief RAID XOR 重建 + ECC 重试 + 解扰指定 CW。
 *
 * FTL 在 fc_poll_completion 后遍历 desc 链，对 status=UNC 的 CW
 * 调用此函数。函数内部：
 *   ① 从所有其他 CW + parity 做 RAID XOR 重建
 *   ② ECC 解码重试
 *   ③ 解扰所有 CW
 *   ④ 更新 desc->status = READY
 *
 * @param cmd           带 desc 链的命令指针。
 * @param failed_cw_idx 失败 CW 在 desc 链中的索引（0-based）。
 *
 * @return FC_STATUS_OK       恢复成功，所有 desc 置 READY。
 *         FC_STATUS_IO_ERROR 重建后 ECC 仍失败。
 */
fc_status_e fc_data_path_raid_recover(const fc_command_t *cmd, BYTE failed_cw_idx);

/* ── 单 CW 步骤原语（管线与单测共用） ──
 *
 * 上面几个 fc_data_path_* 是**编排**（沿 desc 链跑完整个 stage），下面这几个是
 * 它们内部的**单个算法步骤**，一律只操作一块 528B 的裸 CW buffer
 * （[META:4][CRC:4][USER:512][ECC:8]），不碰 command / descriptor / 链。
 *
 * 拆出来的目的：让「调了哪些步骤」直接等于「开了哪些功能」。
 * 单测（tests/unit/test_fc_level2.c）拿一块 buffer 调这些原语就能验算法本身，
 * 不必建链、也不必用 config 位去组合出「只开 CRC」这类状态。
 *
 * ⚠ 提交出去的命令**绝不能再手工调**这些原语——FC 自己会跑整条管线
 *   （见 tests/unit/fc_test_common.h「五」），重复编码 = 数据损坏。
 */

/**
 * @brief 把本 CW 的逻辑地址（扇区 LBA）写进 META 区 [0,4)。
 *
 * @param cw   528B CW buffer（[META:4][CRC:4][USER:512][ECC:8]）。
 * @param meta 本 CW 的扇区 LBA。
 *
 * @note 写路径 Stage 0 的唯一 META 写入者；META 保持明文，不参与加扰。
 */
void fc_data_path_meta_write(BYTE *cw, DWORD meta);

/**
 * @brief 对 USER 区 [8,520) 计算 CRC-32 并写入 CRC 字段 [4,8)。
 *
 * @param cw 528B CW buffer；调用前 USER 区须已是明文数据。
 */
void fc_data_path_crc_write(BYTE *cw);

/**
 * @brief 加扰 / 解扰（同一个 XOR 变换，自反），作用于 USER 区 [8,520)。
 *
 * @param cw   528B CW buffer。
 * @param seed 加扰种子；通常由页内明文 META 经 fc_random_seed_from_meta 派生。
 *
 * @note META / CRC 保持明文，读侧才能从页内 META 自举出同一个种子。
 */
void fc_data_path_scramble(BYTE *cw, DWORD seed);

/**
 * @brief 对 520B 数据区计算 8B ECC parity 并写入 [520,528)。
 *
 * @param cw 528B CW buffer；USER 区须已完成写入与加扰。
 *
 * @return FC_STATUS_OK       编码完成（引擎返回 NOT_SUPPORTED 时也归一为 OK）。
 *         其他 fc_status_e   ECC 引擎返回的错误码。
 */
fc_status_e fc_data_path_ecc_encode(BYTE *cw);

/**
 * @brief 对单个 CW 做 ECC 解码，可就地纠正数据区与 parity 尾区。
 *
 * @param cw       528B CW buffer。
 * @param ecc_bits 出参，非 NULL 时返回纠正的 bit 数，失败时为 0；不需要时传 NULL。
 *
 * @return FC_STATUS_OK            解码成功（0 个或多个 bit 被纠正）。
 *         FC_STATUS_NOT_SUPPORTED ECC 引擎未实现，未做任何纠正。
 *         FC_STATUS_IO_ERROR      不可纠。
 */
fc_status_e fc_data_path_ecc_decode(BYTE *cw, BYTE *ecc_bits);

/**
 * @brief CRC 校验：重算 USER 区 CRC 并与 CRC 字段 [4,8) 中存储值比对。
 *
 * @param cw 528B CW buffer（USER 区须为明文，即解扰之后）。
 *
 * @return true = 存储值与重算值相等；false = 不相等。
 */
bool fc_data_path_crc_check(const BYTE *cw);

/**
 * @brief META 读回校验，包含两项：
 * ① 低 3 位自洽，等于 paa 对应的 plane/cw；
 * ② command_meta 非 FC_META_UNKNOWN 时，read_meta 须等于 lba_to_meta(command_meta, plane, cw)。
 *
 * @param cw           528B CW buffer。
 * @param command_meta 本命令的 META；FC_META_UNKNOWN 时跳过第 ② 项。
 * @param paa          本 CW 的 NAND 物理地址（plane / cw 的来源）。
 *
 * @return 0U                      两项均通过。
 *         FC_CW_STATUS_MATA_ERROR 任一不符。
 */
DWORD fc_data_path_meta_check(const BYTE *cw, DWORD command_meta, DWORD paa);

/* ══════════════════════════════════════════════════════════
 * 加扰引擎
 * ══════════════════════════════════════════════════════════ */

/*
 * 加扰目标：让输出**统计独立**（P(bit=1 | 全部历史) = 1/2），与随机数无法区分。
 * ⚠ 不是「0/1 各占一半」的均衡（`0101…` 也均衡却最不随机），更不是「与上一位相反」。
 *   理论见 docs/design/fc_nand_design.md §5.4。
 *
 * NAND 加扰算法选择（编译期配置）
 *
 * FC_RANDOM_ALGO_LFSR  (默认)
 *   16 级线性反馈移位寄存器，多项式 x¹⁶ + x¹⁴ + x¹³ + x¹¹ + 1（最大长度，周期 65535）。
 *   主控加扰最主流的方案，硬件实现只需几个寄存器 + 异或门。
 *   ⚠ 本多项式是本工程的选择，**不是 ONFI 规定**——ONFI 不规定加扰器。
 *
 * FC_RANDOM_ALGO_XORSHIFT
 *   XOR-shift 32 三步递推：seed ^= seed<<13; seed ^= seed>>17; seed ^= seed<<5
 *   周期 2³²-1，硬件开销极小。
 *
 * FC_RANDOM_ALGO_SIMPLE_XOR
 *   固定伪随机模式重复，低端方案。不需要种子递推，每 CW 用固定 pattern 异或。
 */
#define FC_RANDOM_ALGO_LFSR       (0U)
#define FC_RANDOM_ALGO_XORSHIFT   (1U)
#define FC_RANDOM_ALGO_SIMPLE_XOR (2U)

#ifndef FC_RANDOM_ALGO
#    define FC_RANDOM_ALGO FC_RANDOM_ALGO_LFSR
#endif

/**
 * @brief 加扰种子：**本 CW 的逻辑地址**，即该 CW 的 META 字段（扇区 LBA）。
 *
 * 为什么是逻辑地址而不是物理地址：GC 搬页后逻辑地址不变 ⇒ 密文不变，
 * 不需要「解扰再按新物理地址重加扰」。这是存储主控的通行做法
 * （见 docs/archive/changelog/fc_scrambler_lba_seed_v24.md 的出处）。
 *
 * 粒度 = 被寻址逻辑单元：本工程一个 CW（512B 用户数据）= 一个扇区 = 一个逻辑单元，
 * 故每条 CW 用自己那条扇区 LBA 当种子。
 *
 * ⚠ 调用方（fc_data_path.c）必须传**该 CW 的扇区 LBA**（desc->meta / 从页内读回的
 * 明文 META），而不是 command->meta（那是本命令的页级基值）。
 *
 * @param cw_meta 本 CW 的 META 字段（扇区 LBA）
 * @return        非零的 32 位加扰种子
 */
DWORD fc_random_seed_from_meta(DWORD cw_meta);

/**
 * @brief 对 data[0..length-1] 进行加扰/解扰。
 * 加扰和解扰是同一操作（XOR 对称）。
 * 算法由 FC_RANDOM_ALGO 编译宏控制。
 *
 * @param data   待变换的数据缓冲（原地修改）
 * @param length 数据字节数
 * @param seed   本 CW 的加扰种子（fc_random_seed_from_meta 的输出）
 * @return       FC_STATUS_OK（当前实现不会失败）
 */
fc_status_e fc_random_transform(BYTE *data, WORD length, DWORD seed);

/**
 * @brief 获取当前使用的加扰算法名称（调试用）。
 *
 * @return 静态字符串，内容由 FC_RANDOM_ALGO 决定
 */
const char *fc_random_algo_name(void);

/* ══════════════════════════════════════════════════════════
 * ECC 引擎（BCH / QC-LDPC 选择）
 * ══════════════════════════════════════════════════════════ */

/*
 * BCH ECC 引擎。
 *
 * 移植自 Linux 内核 lib/bch.c（GPL v2），作者 Ivan Djelic。
 * 将内核依赖替换为标准 C；本工程参数固定，表存于静态结构体，不使用堆。
 *
 * 典型参数（CW = 520B data + 8B parity）：
 *   m=13, t=4  → ecc_bytes=7（64bit parity 够用）
 *   m=8,  t=8  → ecc_bytes=8（正好填满 8B）
 */

typedef struct fc_bch_control fc_bch_control_t;

/**
 * @brief 初始化 BCH 引擎。
 * @param m         Galois 域阶数 (5..15)
 * @param t         纠错能力 (bit)
 * @param prim_poly 本原多项式 (0=使用默认)
 * @param swap_bits 是否交换数据字节内的 bit 顺序
 * @return BCH 控制句柄，失败返回 NULL
 * @note 本工程静态存储只覆盖 m=13 / t=4；其他参数违反前置条件
 *       （debug 下 ASSERT，release 下返回 NULL）。
 */
fc_bch_control_t *fc_bch_init(int m, int t, unsigned int prim_poly, BYTE swap_bits);

/**
 * @brief 释放 BCH 引擎资源。
 * @param bch BCH 控制句柄（可为 NULL）
 */
void fc_bch_free(fc_bch_control_t *bch);

/**
 * @brief BCH 编码：计算 data 的 ECC 校验码。
 * @param bch  BCH 控制句柄
 * @param data 输入数据
 * @param len  数据字节数
 * @param ecc  ECC 缓冲区（ecc_bytes 字节）；为 NULL 时以全零状态计算，结果只留在内部工作区
 * @note 支持增量计算：把上一段输出的 ecc 缓冲区继续作为本段输入传入；首次调用前该缓冲区必须清零。
 * @note bch / data 为空违反前置条件，一律 ASSERT。
 */
void fc_bch_encode(fc_bch_control_t *bch, const BYTE *data, unsigned int len, BYTE *ecc);

/**
 * @brief BCH 解码：检测并定位 data+recv_ecc 中的错误。
 * @param bch      BCH 控制句柄
 * @param data     数据区（len 字节）
 * @param len      数据字节数
 * @param recv_ecc 接收到的 ECC；为 NULL 时不与预期校验比对
 * @param calc_ecc 预先算好的 ECC；为 NULL 时现场编码 data 得到预期校验（此时 recv_ecc 必须非空）
 * @param syn      预先算好的综合式（2t 项）；非 NULL 时直接用于 Berlekamp–Massey
 * @param errloc   输出错误位置数组（至少 t 个元素）
 * @param out_nerr 输出纠正的错误数（0..t）
 * @return true  解码完成（可能纠正了 0..t 个错误）
 *         false 不可纠
 * @note 入参前置违反（len 超码长 / data、recv_ecc 为空）一律 ASSERT
 *       （见 assertion_guidelines.md §十五）。
 * @note errloc[i] < 8*len 为数据区错误，errloc[i] >= 8*len 为 ECC 区错误；
 *       纠正方式 data[errloc/8] ^= (1 << (errloc%8))。
 */
bool fc_bch_decode(fc_bch_control_t *bch, const BYTE *data, unsigned int len, const BYTE *recv_ecc, const BYTE *calc_ecc, const unsigned int *syn, unsigned int *errloc, unsigned int *out_nerr);

/**
 * @brief 辅助函数：获取 BCH 引擎的 ECC 字节数。
 * @param bch BCH 控制句柄
 * @return ECC 字节数
 */
unsigned int fc_bch_ecc_bytes(const fc_bch_control_t *bch);

/**
 * @brief 辅助函数：获取纠错能力 t。
 * @param bch BCH 控制句柄
 * @return 纠错能力（bit 数）
 */
unsigned int fc_bch_t(const fc_bch_control_t *bch);

/*
 * ECC 引擎统一接口。
 *
 * fc_ldpc_encode/fc_ldpc_decode 作为统一 ECC 入口，
 * 底层算法由编译宏选择：
 *
 *   FC_ECC_ALGO_BCH  (0) — BCH(m=13, t=4)（默认，全功能）
 *   FC_ECC_ALGO_LDPC (1) — QC-LDPC 硬判决 Bit-flipping（实验性）
 *
 * CW 布局：[META:4][CRC:4][USER:512][PARITY:8] = 528B
 *
 * 运行时 ldpc 位域：
 *   ldpc=0 → BCH
 *   ldpc=1 → LDPC hard
 *   ldpc=2 → LDPC soft（stub）
 *   ldpc=3 → LDPC raw（stub）
 */
#define FC_ECC_ALGO_BCH  (0U)
#define FC_ECC_ALGO_LDPC (1U)

#ifndef FC_ECC_ALGO
#    define FC_ECC_ALGO FC_ECC_ALGO_BCH
#endif

/**
 * @brief 活动 ECC 引擎名（诊断/开机横幅用）
 *
 * 与实现文件里的 fc_bch_init(13, 4, …) 就地一致。
 *
 * @return 指向静态字符串的引擎名（"BCH(13,4)" 或 "LDPC"），调用者不负责释放。
 */
const char *fc_ecc_algo_name(void);

typedef struct
{
    WORD codeword_bytes;
    WORD parity_bytes;
    BYTE max_iterations;
} fc_ldpc_config_t;

/**
 * @brief ECC 编码单个 CW：在 data 尾部写入 8B parity
 *
 * data_length 恒为 FC_BUFFER_DATA_SIZE（520，CW 缓冲 528B 减去 8B parity 尾区）。
 *
 * @param config      ECC 配置（当前实现未读取，可为 NULL）
 * @param data        CW 缓冲；数据区 [0, data_length) 为输入，parity 尾区
 *                    [data_length, data_length+8) 为输出
 * @param data_length 数据区字节数，∈ [8, FC_BUFFER_DATA_SIZE]
 * @return FC_STATUS_OK —— 编码成功
 *         FC_STATUS_NOT_SUPPORTED —— ECC 引擎初始化失败
 * @pre data 非空、data_length ∈ [8, FC_BUFFER_DATA_SIZE]（违反一律 ASSERT）。
 */
fc_status_e fc_ldpc_encode(const fc_ldpc_config_t *config, BYTE *data, WORD data_length);

/**
 * @brief 解码单个 CW（data + parity 连续缓冲），成功后就地纠正数据区
 *
 * BCH 分支可选输出纠正的 bit 数；LDPC 分支不写 err_bits（入口处已置 0）。
 *
 * @param config      ECC 配置（当前实现未读取，可为 NULL）
 * @param data        CW 缓冲；数据区 [0, data_length) 与 parity 尾区
 *                    [data_length, data_length+8)
 * @param data_length 数据区字节数，∈ [8, FC_BUFFER_DATA_SIZE]
 * @param err_bits    可选输出，非 NULL 时返回本 CW 纠正的错误 bit 数（BCH t=4 → 0..4），
 *                    失败时为 0；无需该信息时传 NULL
 * @return FC_STATUS_OK —— 解码成功（BCH 分支已就地纠正）
 *         FC_STATUS_IO_ERROR —— 不可纠
 *         FC_STATUS_NOT_SUPPORTED —— ECC 引擎初始化失败
 *         FC_STATUS_INVALID_ARGUMENT —— LDPC 分支引擎未初始化
 * @pre data 非空、data_length ∈ [8, FC_BUFFER_DATA_SIZE]（违反一律 ASSERT）。
 */
fc_status_e fc_ldpc_decode(const fc_ldpc_config_t *config, BYTE *data, WORD data_length, BYTE *err_bits);

/* ══════════════════════════════════════════════════════════
 * Stripe RAID 引擎
 * ══════════════════════════════════════════════════════════ */

/*
 * ============================================================================
 * RAID XOR 引擎 — 编译开关
 *
 * 当前 CONFIG_FC_RAID=0（默认不启用），所有 RAID 功能代码不编译。
 *
 * 何时开启：硬件功能验证闭环后，需要启用跨 Plane XOR 数据保护时。
 * 打开方法：在 include/fc/fc.h 中 #define CONFIG_FC_RAID 1
 *
 * 开启后需要实现：
 *   1) 写路径：fc_poll_internal() 中 raid=ENCODE 分支分配 parity 命令、
 *      调用 generate_stripe_parity()、以 Multi-Plane PROGRAM 提交
 *   2) 读路径：FTL 提交 Multi-Plane READ 读取双 Plane 数据后，
 *      ECC 失败时利用 parity CW 做 XOR 重建恢复
 *   3) 读恢复：fc_data_path_raid_recover() 适配跨命令 buffer 收集
 *
 * 设计文档：docs/design/fc_nand_design.md §5.1
 * ============================================================================
 */

#if CONFIG_FC_RAID

/*
 * RAID 操作的三个基础指令：
 *   START  → 初始化累加器 = 第一个 CW 的数据
 *   CONTINUE → 累加器 ^= 当前 CW
 *   END    → 累加器 = XOR 结果，写入 parity buffer
 */

/* 每个 LUN 的 RAID 上下文 */
typedef struct
{
    bool  active;                          /* true = RAID 序列进行中 */
    BYTE  acc_buf[FC_BUFFER_POOL_CW_SIZE]; /* XOR 累加器缓冲（一个 CW 大小） */
    BYTE  processed;                       /* 已累加的 CW 数（含 START） */
    BYTE  total;                           /* 期望累加的总 CW 数 */
    DWORD start_paa;                       /* 起始 PAA（调试用） */
} fc_raid_context_t;

/*
 * RAID 上下文池，每个 LUN 独立。
 * 影子缓冲用于 HSQ 打断时保存/恢复。
 * 在 fc_data_path.c 中定义为全局静态数组。
 */

/* ── 上下文管理 ── */

/**
 * @brief 初始化指定 LUN 的 RAID 上下文，清零上下文与影子缓冲。
 *
 * @param lun LUN 编号（调用者保证 < LUN_NUM）
 */
void fc_raid_init_ctx(BYTE lun);

/* ── RAID 序列 ── */

/**
 * @brief 开始新的 RAID XOR 序列。
 * 将 cw_buffer 拷贝到 acc_buf 作为累加器初始值。
 *
 * @param lun       LUN 编号
 * @param cw_buffer 第一个 CW 的数据（PAA 对应的 buffer）
 * @param total     本次序列期望的 CW 总数（含第一个）
 * @return FC_STATUS_OK           序列已启动
 *         FC_STATUS_NOT_READY    该 LUN 已有 RAID 序列进行中
 *
 * @note 入参非法（lun 越界 / cw_buffer 为 NULL / total 为 0）由断言拦截。
 */
fc_status_e fc_raid_start(BYTE lun, const BYTE *cw_buffer, BYTE total);

/**
 * @brief 累加一个 CW 到 RAID 累加器。acc_buf ^= cw_buffer。
 *
 * @param lun       LUN 编号
 * @param cw_buffer 当前 CW 的数据
 * @return FC_STATUS_OK                已累加
 *         FC_STATUS_INVALID_ARGUMENT  RAID 序列未开始
 *
 * @note 入参非法（lun 越界 / cw_buffer 为 NULL）由断言拦截；
 *       超出 total 的累加属调用者计数 bug，由断言拦截。
 */
fc_status_e fc_raid_accumulate(BYTE lun, const BYTE *cw_buffer);

/**
 * @brief 结束 RAID 序列，将累加器结果写入 parity_buffer。
 * 写完后清 active 标志。
 *
 * @param lun           LUN 编号
 * @param parity_buffer 输出：XOR 结果写入此 buffer
 * @return FC_STATUS_OK                累加结果已写入 parity_buffer
 *         FC_STATUS_INVALID_ARGUMENT  RAID 序列未开始
 *
 * @note 入参非法（lun 越界 / parity_buffer 为 NULL）由断言拦截。
 */
fc_status_e fc_raid_end(BYTE lun, BYTE *parity_buffer);

/* ── 中断保护 ── */

/**
 * @brief 保存 RAID 上下文到影子缓冲（HSQ 打断前调用）。
 *
 * @param lun LUN 编号；越界时直接忽略
 */
void fc_raid_save(BYTE lun);

/**
 * @brief 从影子缓冲恢复 RAID 上下文（HSQ 完成后调回）。
 *
 * @param lun LUN 编号；越界时直接忽略
 */
void fc_raid_restore(BYTE lun);

/* ── 静态恢复（ECC 失败后使用，不需要 LUN 上下文） ── */

/**
 * @brief 从所有 CW 和 parity XOR 重建 failed_idx 的数据：
 *   recovered = buffers[0] ^ buffers[1] ^ ... ^ buffers[count-1]
 *   跳过 failed_idx。
 *
 * @param recovered   输出：恢复后的 buffer
 * @param buffers     所有 CW buffer 指针数组（含 parity）
 * @param count       buffers 总数
 * @param failed_idx  失败的 buffer 索引（0..count-1）
 * @param length      每个 buffer 的长度
 *
 * @note recovered 先清零再累加；buffers[i] 为 NULL 的条目跳过；
 *       length 按 4 字节字对齐处理，尾部不足一字的余数字节不参与。
 */
void fc_raid_recover(BYTE *recovered, BYTE *buffers[], BYTE count, BYTE failed_idx, WORD length);

#endif /* CONFIG_FC_RAID */

#endif /* FC_DATA_PATH_H */