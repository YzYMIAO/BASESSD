#ifndef STATS_H
#define STATS_H

#include "ssd_types.h"

#include <stddef.h> /* offsetof：模块名字表在编译期算字段偏移 */

/*
 * stats — 跨模块统计（模块自持存储 + 名字表 + 中央注册/遍历）
 *
 * 架构见 docs/design/common/statistics_design.md。
 *
 * 关键约定：
 *   · 值仍存在模块自己的结构体里（单写者，H10/C9）；本模块只保存
 *     「module + 实例号 + base + 标量表 + 直方图表」，不复制值。
 *   · 名字表是文件级 const 数组，字段偏移用 offsetof 生成；
 *     字段删除/改名后编译失败，不会留下打印不到的旧名字。
 *   · 打印/清零只在 CLI 上下文调用；不做周期打印。
 *   · SSD_STATS_ENABLE=0 时统计专用对象/表/函数/字段整体不编译；
 *     既有功能字段（如 l2p hit/miss、total_writes）不在此列。见设计文档 §4.7。
 */

#ifndef SSD_STATS_ENABLE
#    define SSD_STATS_ENABLE (1U) /* 0 = 关闭统计（注册表/打印/名字表不进镜像） */
#endif

/* 1 = 名字表保存字段名字符串；0 = 只打印 s<index>（省 Flash，多实例时按 inst+index 定位）。
 * 关闭名字时，`stats <prefix>.<name>` 的字段级过滤不可用，只能按 module/inst 过滤。 */
#ifndef SSD_STATS_NAMES
#    define SSD_STATS_NAMES (1U)
#endif

#if SSD_STATS_NAMES
#    define STATS_NAME_STR(field) #field
#else
#    define STATS_NAME_STR(field) NULL
#endif

/* 统计项种类：counter 可被 --clear 清零；gauge 不清零（可能是活状态）。 */
typedef enum STATS_KIND_E
{
    STATS_COUNTER = 0,
    STATS_GAUGE,
    STATS_KIND_NUM
} stats_kind_e;

/* 支持的字段宽度；只允许无符号 8/16/32 位。 */
typedef enum STATS_WIDTH_E
{
    STATS_U8  = 1,
    STATS_U16 = 2,
    STATS_U32 = 4
} stats_width_e;

/* 一个标量字段的描述：名字 + 相对 base 的偏移 + 宽度 + 种类。 */
typedef struct STATS_NAME_S
{
    const char   *name;   /* 打印名（静态字符串） */
    WORD          offset; /* offsetof(宿主结构体, 字段) */
    stats_width_e width;
    stats_kind_e  kind;
} stats_name_s;

/* 固定桶延迟直方图：bucket[] 是 < 对应上界的累计数，over 是 >= 最大上界。
 * 不变量：sum(bucket) + over == count。max_us 是高水位 gauge。 */
#define STATS_HIST_BUCKET_NUM (9U) /* 上界：50/100/200/500/1m/2m/5m/10m/50m µs；>=50m 进 over */

typedef struct STATS_HIST_S
{
    DWORD bucket[STATS_HIST_BUCKET_NUM];
    DWORD over;
    DWORD count;
    DWORD max_us;
} stats_hist_s;

/* 一个直方图的描述：名字 + 相对 base 的偏移（指向 stats_hist_s）。 */
typedef struct STATS_HIST_NAME_S
{
    const char *name;
    WORD        offset;
} stats_hist_name_s;

/* 一个统计组（一个模块的一个实例）。 */
typedef struct STATS_GROUP_S
{
    const char              *module; /* "ftl.l2p"（静态字符串） */
    BYTE                     inst;   /* 实例号（die/目标号）；STATS_INST_NONE = 无实例维度 */
    BYTE                    *base;   /* 宿主对象地址（模块的静态对象） */
    const stats_name_s      *names;
    BYTE                     count;
    const stats_hist_name_s *hists; /* 直方图表（可为 NULL） */
    BYTE                     hist_count;
    WORD                     entry_base; /* 本组第一项在 delta 基线数组里的扁平下标 */
} stats_group_s;

#define STATS_INST_NONE      (0xFFU)
#define STATS_GROUP_MAX      (32U)  /* 模块数 × 实例数上限（多 die/target 的代码上界） */
#define STATS_ENTRY_MAX      (320U) /* 所有组标量条目总数上限（delta 基线容量） */
#define STATS_HIST_TOTAL_MAX (24U)  /* 所有组直方图总数上限 */

/* 名字表 / 直方图表条目数（编译期常量；注册 count 形参是 BYTE）。
 * 窄化在宏里一次完成，调用点不再逐个写 (BYTE)。 */
#define STATS_ENTRY_COUNT(array) ((BYTE)(sizeof(array) / sizeof((array)[0])))

/* 字段偏移：offsetof 返回 size_t，名字表用 WORD 存（结构体 < 64 KB 的自然上界）。
 * 窄化集中在宏里，模块名字表不再逐项写 (WORD)。
 * ⚠ 偏离 H25：type 形参不能加括号 —— offsetof 展开为 __builtin_offsetof(type, member)，
 *   给类型加括号会让内建宏解析失败。 */
#define STATS_OFFSET(type, field) ((WORD)offsetof(type, field))
#define STATS_OFFSET_NESTED(type1, field1, type2, field2) \
    ((WORD)(offsetof(type1, field1) + offsetof(type2, field2)))

/* 编译期宽度校验：名字表写的 width 必须等于字段真实 sizeof（C7）。
 * 字段从 DWORD 改 WORD 时编译失败，而不是打印时读错 2 字节。
 * ⚠ 同 STATS_OFFSET：type 形参是类型名，不能加括号。 */
#define STATS_CHECK_FIELD(type, field, width) \
    _Static_assert(sizeof(((type *)0)->field) == (width), #field " width mismatch")

/* 直方图字段必须正好是 stats_hist_s（防止名字表指向普通标量）。 */
#define STATS_CHECK_HIST_FIELD(type, field) \
    _Static_assert(sizeof(((type *)0)->field) == sizeof(stats_hist_s), #field " hist size mismatch")

/* 打印格式：text = 一行一项；csv = module,inst,name,value；brief = 省略直方图桶行。 */
typedef enum STATS_FORMAT_E
{
    STATS_FORMAT_TEXT = 0,
    STATS_FORMAT_CSV,
    STATS_FORMAT_BRIEF,
    STATS_FORMAT_NUM
} stats_format_e;

#if SSD_STATS_ENABLE
/**
 * @brief 注册（或更新）一个统计组的标量表
 *
 * 同一 (module, inst) 重复注册是幂等更新（UT 反复 init 不会撑满注册表）；
 * 但同组条目数变化属配置错误，直接拒绝。
 *
 * @param module 组名（静态字符串，生命周期 = 程序）
 * @param inst   实例号；STATS_INST_NONE = 无实例维度
 * @param base   宿主对象地址（稳定地址，不得是栈上临时对象）
 * @param names  标量名字表（静态数组）
 * @param count  条目数（>0）
 * @return true  注册/更新成功
 *         false 参数非法 / 组表或条目表满 / 同组条目数不一致
 */
BOOL stats_register(const char *module, BYTE inst, void *base, const stats_name_s *names, BYTE count);

/**
 * @brief 给已注册的组附加直方图表（必须在 stats_register 之后调用）
 *
 * @param module 组名（必须已注册）
 * @param inst   实例号
 * @param base   同一宿主对象地址
 * @param hists  直方图表（静态数组）
 * @param count  条目数（>0）
 * @return true  注册成功
 *         false 组未注册 / 参数非法 / 直方图总数超限
 */
BOOL stats_register_hist(const char *module, BYTE inst, void *base, const stats_hist_name_s *hists, BYTE count);

/**
 * @brief 观测一次延迟（µs），更新桶 / over / count / max_us
 *
 * @param hist     直方图（非空）
 * @param value_us 延迟（µs）
 */
void stats_hist_observe(stats_hist_s *hist, DWORD value_us);

/**
 * @brief 打印已注册的统计（只读）
 *
 * prefix 支持 `module`、`module.name`、`module[inst]`、`module[inst].name`。
 *
 * @param prefix module / module.name / module[inst] / module[inst].name；NULL = 全部
 * @param format 输出格式（text / csv / brief）
 * @return true  至少打印了一项
 *         false 没有匹配项
 */
BOOL stats_print(const char *prefix, stats_format_e format);

/**
 * @brief 只列出已注册的组与条目数（不打印值）
 */
void stats_list(void);

/**
 * @brief 打印自上次 delta 基线以来的变化（首次调用只建基线）
 *
 * 标量 counter 打 `+差值`；gauge 打当前值；直方图打当前值（不做差值）。
 * 未匹配 prefix 的项也会更新基线，因此带前缀调用不会让其他组出现假差值。
 *
 * @param prefix module / module.name / module[inst] / module[inst].name；NULL = 全部
 * @return true  至少打印了一项 / 首次调用建立了基线
 *         false 没有匹配项（首次调用仍会建立全量基线）
 */
BOOL stats_delta(const char *prefix);

/**
 * @brief 清零匹配的 STATS_COUNTER 项与直方图桶（gauge/max_us 不动），并重置 delta 基线
 *
 * @param prefix module / module.name / module[inst] / module[inst].name；NULL = 全部
 *
 * @note 只按名字表的 kind 逐字段清零，绝不 memset 整个 base——
 *       base 可能是控制器主体，整块清零会破坏活状态。
 */
void stats_clear(const char *prefix);
#else
/* 关闭时实参不求值：不得把带副作用的表达式放进这些调用。 */
#    define stats_register(...)      ((void)0)
#    define stats_register_hist(...) ((void)0)
#    define stats_hist_observe(...)  ((void)0)
#    define stats_print(...)         ((void)0)
#    define stats_list()             ((void)0)
#    define stats_delta(...)         ((void)0)
#    define stats_clear(...)         ((void)0)
#endif

/*
 * 计数自增宏。
 * ⚠ 实参必须在关闭档不求值也没有副作用（见上）。
 */
#if SSD_STATS_ENABLE
#    define STATS_INC(field)    ((field)++)
#    define STATS_ADD(field, n) ((field) += (n))
#else
#    define STATS_INC(field)    ((void)0)
#    define STATS_ADD(field, n) ((void)0)
#endif

#endif /* STATS_H */
