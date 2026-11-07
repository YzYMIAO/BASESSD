#ifndef NAND_PROTOCOL_H
#define NAND_PROTOCOL_H

#include "nand_config.h"
#include "ssd_types.h" /* BYTE/WORD/DWORD/QWORD */

#include <stdint.h>

/*
 * nand_protocol.h — FC ↔ NAND 执行层的共享协议类型
 *
 * 这里只放**裸类型**：结果码、UOP、命令请求/完成、时序参数、PHY 后端
 * handler 表。它不依赖 HAL、不依赖 FC 头，FC 与 NAND 都可以 include。
 *
 * 依赖方向：FC（引擎/ONFI 序列）→ 本头 → NAND（FMC 后端/器件模型）。
 * NAND 不得反向 include FC 头。
 */

/* 引擎执行结果码（自定义错误体系，非 HAL/POSIX） */
typedef enum NAND_RESULT_E
{
    NAND_RESULT_OK               = 0x00U, /* 成功 */
    NAND_RESULT_INVALID_ARGUMENT = 0xE1U, /* 参数/序列非法 */
    NAND_RESULT_TIMEOUT          = 0xE2U, /* busy 超时 */
    NAND_RESULT_NOT_READY        = 0xE3U, /* 外设忙，让出（非错误） */
    NAND_RESULT_OPERATION_FAILED = 0xE4U, /* NAND 操作失败（状态字节 FAIL 位） */
} nand_result_e;

/*
 * UOP 微指令类型。
 *
 * operand 字段在各 UOP 下的含义（关键约定，读代码前必读）：
 *   COMMAND           → 命令字节（如 00h/30h/80h/10h）
 *   COLUMN_0/1        → 不使用（列地址取自 request->column）
 *   ROW_0/1/2         → plane 下标（request->row[operand] 的 0/1）
 *   DATA_IN/OUT       → buffer 偏移（Multi-Plane 第二 plane 偏移量）
 *   DATA_OUT_STATUS   → 不使用（直读 DQ）
 *   JUMP_IF_NOT_READY → 回跳步数（NAND busy 时回跳重新执行 70h 轮询）
 *   CHECK_FAIL        → 不使用（检查上一步读到的状态字节）
 *   DELAY_TIMING      → timing 字段选择（NAND_TIMING_TWB/TWH/TADL）
 *   WAIT_READY        → 不使用（等待 RB#）
 */
typedef enum NAND_UOP_TYPE_E
{
    NAND_UOP_WAIT_READY,        /* 等待 NAND ready（RB# 轮询，超时检测） */
    NAND_UOP_COMMAND,           /* 写命令字节，operand=命令值 */
    NAND_UOP_COLUMN_0,          /* 写列地址低字节 */
    NAND_UOP_COLUMN_1,          /* 写列地址高字节 */
    NAND_UOP_ROW_0,             /* 写行地址低字节（plane=operand） */
    NAND_UOP_ROW_1,             /* 写行地址中间字节 */
    NAND_UOP_ROW_2,             /* 写行地址高字节 */
    NAND_UOP_DATA_IN,           /* 写数据到 NAND（Program） */
    NAND_UOP_DATA_OUT,          /* 从 NAND 读数据 */
    NAND_UOP_DATA_OUT_STATUS,   /* 直读 70h 状态字节 */
    NAND_UOP_JUMP_IF_NOT_READY, /* 条件跳转：busy 则回跳 operand 步 */
    NAND_UOP_CHECK_FAIL,        /* 检查状态字节 FAIL 位 */
    NAND_UOP_CHECK_FAIL_CACHE,  /* 检查 FAIL + FAILC（cache program 当前/上一页） */
    NAND_UOP_DELAY_TIMING,      /* 按当前 op 的 timing 表延时，operand=NAND_TIMING_* */
    NAND_UOP_END,               /* 序列结束 */
    NAND_UOP_COUNT,             /* 枚举数量（数组长度用） */
} nand_uop_type_e;

/* NAND_UOP_DELAY_TIMING 的字段选择器；时序值来自当前 op 的 nand_op_timing_s。 */
typedef enum NAND_TIMING_FIELD_E
{
    NAND_TIMING_TWB = 0U, /* 命令/确认后到 R/B# 拉低 */
    NAND_TIMING_TWH,      /* 70h/78h 后到状态字节有效 */
    NAND_TIMING_TADL,     /* 11h 切换 plane 后的地址锁存延时 */
    NAND_TIMING_FIELD_COUNT,
} nand_timing_field_e;

/* 单条微指令：类型 + 参数 */
typedef struct NAND_UOP_S
{
    nand_uop_type_e type;
    DWORD           operand;
} nand_uop_s;

/* 命令请求：列/行/数据。row 按 plane 索引（NAND_PLANE_MAX 深），
 * 单 plane 只用 row[0]，多 plane 填 row[0..plane_count-1]。 */
typedef struct NAND_COMMAND_REQUEST_S
{
    BYTE  operation;
    WORD  column;
    DWORD row[NAND_PLANE_MAX]; /* 按 plane 索引，uop ROW handler 通过 operand 选取 */
    BYTE *buffer;
    WORD  data_length;
} nand_cmd_req_s;

/* 命令完成信息：nand_status = 70h 原始状态字节；
 * execution_status = NAND_RESULT_* 执行结果。两字段语义不同。 */
typedef struct NAND_COMMAND_COMPLETION_S
{
    BYTE          nand_status; /* 70h 原始状态：OP 后的读状态（FAIL/FAILC） */
    BYTE          pre_status;  /* 70h 原始状态：OP 前的读状态（RDY/ARDY/WP） */
    nand_result_e execution_status;
} nand_cmd_comp_s;

/* 操作时序参数：twb/twh/tadl 由 NAND_UOP_DELAY_TIMING 消费；
 * busy_timeout_us 供引擎/FMC 忙等超时使用。 */
typedef struct NAND_OPERATION_TIMING_S
{
    WORD twb_us;
    WORD twh_us;
    WORD tadl_us;
    WORD busy_timeout_us;
} nand_op_timing_s;

/*
 * FMC NAND 时序参数（29 个 WORD，单位 ns）。
 *
 * 放在协议头而不是 nand_fmc.h：器件描述符需要引用它，而 FC 不能 include
 * 含 HAL 类型的 nand_fmc.h。FMC 后端再把本结构换算成 PCC 寄存器值。
 *
 * 器件时序表使用 designated initializer 按字段名赋值；字段顺序仍决定结构体
 * 布局，但初始化不再依赖位置。
 */
typedef struct NAND_FMC_TIMING_S
{
    WORD t_adl_ns;     /* 地址→数据加载延时 */
    WORD t_alh_ns;     /* ALE 保持 */
    WORD t_als_ns;     /* ALE 建立 */
    WORD t_ch_ns;      /* CE 保持 */
    WORD t_clh_ns;     /* CLE 保持 */
    WORD t_cls_ns;     /* CLE 建立 */
    WORD t_cs_ns;      /* CE 建立 */
    WORD t_dh_ns;      /* 数据保持 */
    WORD t_ds_ns;      /* 数据建立 */
    WORD t_wc_ns;      /* 写周期 */
    WORD t_wh_ns;      /* WE 高电平宽度 */
    WORD t_wp_ns;      /* WE 低电平宽度 */
    WORD t_ar_ns;      /* ALE→RE 延时 */
    WORD t_cea_max_ns; /* CE→输出有效（max） */
    WORD t_chz_max_ns; /* CE→输出高阻（max） */
    WORD t_clr_ns;     /* CLE→RE 延时 */
    WORD t_coh_ns;     /* CE 高→输出保持 */
    WORD t_ir_ns;      /* 命令→数据输出间隔 */
    WORD t_rc_ns;      /* 读周期 */
    WORD t_rea_max_ns; /* RE→输出有效（max） */
    WORD t_reh_ns;     /* RE 高电平宽度 */
    WORD t_rhoh_ns;    /* RE 高→输出保持 */
    WORD t_rhw_ns;     /* RE→WE 延时 */
    WORD t_rhz_max_ns; /* RE→输出高阻（max） */
    WORD t_rloh_ns;    /* RE 低→输出保持 */
    WORD t_rp_ns;      /* RE 低电平宽度 */
    WORD t_rr_ns;      /* 读命令→读命令间隔 */
    WORD t_wb_max_ns;  /* WE 高→busy（max） */
    WORD t_whr_ns;     /* 状态→RE 高延时 */
} nand_fmc_timing_s;

/* UOP handler 签名：context 透传给 NAND 后端（FMC 实现） */
typedef nand_result_e (*nand_uop_handler_fn_t)(void *context, const nand_uop_s *uop, const nand_cmd_req_s *request, const nand_op_timing_s *timing, nand_cmd_comp_s *completion);

/* NAND PHY 后端抽象：handler 表 + 透传上下文。
 * 由 NAND 层（nand_fmc.c）提供，FC 的 UOP 引擎只持有指针。 */
typedef struct NAND_PHY_BACKEND_S
{
    const nand_uop_handler_fn_t *handlers;
    void                        *context;
} nand_phy_backend_s;

#endif /* NAND_PROTOCOL_H */
