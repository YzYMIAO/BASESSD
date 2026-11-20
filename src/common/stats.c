/*
 * stats.c — 跨模块统计的注册表、打印、差分与清零
 *
 * 设计见 docs/design/common/statistics_design.md：
 *   · 值不在本文件；只保存组描述（module/inst/base/names/hists），打印时按 offset 读宿主字段。
 *   · 名字表 / 直方图表是模块自己的 const 数组，字段增删由 offsetof 在编译期校验。
 *   · 唯一一次「写值」是 stats_clear / stats_hist_observe（且只写 counter / 直方图桶）。
 */

#include "stats.h"

#include "assertion.h"
#include "lifetime.h" /* host 字节总数（--delta 吞吐） */
#include "log.h"
#include "platform.h" /* platform_now_ms */

#include <stdio.h> /* snprintf（字段显示名/CSV） */
#include <string.h>

#if SSD_STATS_ENABLE

/* module.name 的最大长度（不含级别/标签前缀）：超长会被日志行缓冲截断。 */
#    define STATS_LINE_NAME_MAX (48U)

/* 组表查找失败（与实例哨兵 STATS_INST_NONE 是不同语义，C11）。 */
#    define STATS_GROUP_INVALID (0xFFU)

/* 直方图桶上界（µs，开区间）：bucket[i] = 延迟 < 本值；全部不满足进 over。
 * 覆盖 L2P load（几十 µs）到 GC/CP job（几十 ms）。H4：固定边界集中定义。 */
/* 文本输出把 9 个桶 + over 排成 2×5 对齐小表；桶数改变必须同步改标签表。 */
#    define STATS_HIST_LABEL_NUM     (STATS_HIST_BUCKET_NUM + 1U) /* 桶 + over */
#    define STATS_HIST_CELLS_PER_ROW (5U)
#    define STATS_HIST_CELL_W        (8U)
#    define STATS_TABLE_NAME_W_MAX   (24U)

_Static_assert(STATS_HIST_LABEL_NUM == 10U, "2x5 histogram layout assumes 9 buckets + over");

/* 与 s_hist_bounds 一一对应，最后一个标签是 over（>=50 ms）。 */
static const char *const s_hist_labels[STATS_HIST_LABEL_NUM] = {
    "<50u",
    "<100u",
    "<200u",
    "<500u",
    "<1m",
    "<2m",
    "<5m",
    "<10m",
    "<50m",
    ">=50m",
};

static const DWORD s_hist_bounds[STATS_HIST_BUCKET_NUM] = {
    50U,    /* b0: < 50 µs */
    100U,   /* b1: < 100 µs */
    200U,   /* b2: < 200 µs */
    500U,   /* b3: < 500 µs */
    1000U,  /* b4: < 1 ms */
    2000U,  /* b5: < 2 ms */
    5000U,  /* b6: < 5 ms */
    10000U, /* b7: < 10 ms */
    50000U  /* b8: < 50 ms；>=50 ms 进 over */
};

static stats_group_s s_groups[STATS_GROUP_MAX];
static BYTE          s_group_count;

/* delta 基线：按注册顺序展平的标量值；首次 delta 建立，clear 重置。 */
static DWORD s_baseline[STATS_ENTRY_MAX];
static BOOL  s_baseline_valid;
static DWORD s_baseline_ms; /* 基线时刻；用于 delta 窗口吞吐 */
#    if SSD_STATS_PERSIST_ENABLE
static QWORD s_baseline_read_bytes;
static QWORD s_baseline_write_bytes;
#    endif

/**
 * @brief 按宽度读一个标量字段
 *
 * @param base 宿主对象
 * @param name 字段描述（offset/width 由模块名字表保证合法）
 * @return 字段值（统一升到 DWORD）
 */
static DWORD stats_value_read(const BYTE *base, const stats_name_s *name)
{
    const BYTE *field = &base[name->offset];
    DWORD       value = 0U;

    // 基址是 BYTE*，对齐由模块字段保证但编译器看不到；用 memcpy 读，
    // 避免 -Wcast-align 与潜在的非对齐访问（宽度是运行期值，拷贝很小）。
    switch (name->width)
    {
        case STATS_U8:
            value = *field;
            break;
        case STATS_U16:
        {
            WORD v;

            memcpy(&v, field, sizeof(v));
            value = v;
            break;
        }
        case STATS_U32:
        {
            DWORD v;

            memcpy(&v, field, sizeof(v));
            value = v;
            break;
        }
        default:
            ASSERT(false);
            break;
    }
    return value;
}

/**
 * @brief 按宽度写一个标量字段（只用于 counter 清零）
 *
 * @param base  宿主对象
 * @param name  字段描述
 * @param value 写入值（调用者只传 0）
 */
static void stats_value_store(BYTE *base, const stats_name_s *name, DWORD value)
{
    BYTE *field = &base[name->offset];

    switch (name->width)
    {
        case STATS_U8:
            *field = (BYTE)value;
            break;
        case STATS_U16:
        {
            const WORD v = (WORD)value;

            memcpy(field, &v, sizeof(v));
            break;
        }
        case STATS_U32:
            memcpy(field, &value, sizeof(value));
            break;
        default:
            ASSERT(false);
            break;
    }
}

/**
 * @brief 拷贝出一个直方图（base 是 BYTE*，用 memcpy 避免对齐强转）
 *
 * @param base   宿主对象
 * @param offset 直方图相对 base 的偏移
 * @param out    输出
 */
static void stats_hist_read(const BYTE *base, WORD offset, stats_hist_s *out)
{
    memcpy(out, &base[offset], sizeof(*out));
}

/**
 * @brief 一个标量 / 直方图是否匹配 CLI 前缀
 *
 * 支持四种写法：
 *   `module`、`module.name`、`module[inst]`、`module[inst].name`；
 *   module 允许是组名的前缀（`ftl`、`ftl.cp`），name 允许是字段名的前缀
 *   （`ftl.l2p.load`）。
 *
 * @param module 组名
 * @param inst   实例号
 * @param name   字段名（SSD_STATS_NAMES=0 时为 NULL）
 * @param prefix 前缀；NULL = 全部
 * @return true 匹配；false 不匹配
 */
static BOOL stats_entry_matches(const char *module, BYTE inst, const char *name, const char *prefix)
{
    if (prefix == NULL)
    {
        return true;
    }

    const char *bracket = strchr(prefix, '[');
    size_t      mod_len;
    BYTE        want_inst  = STATS_INST_NONE;
    BOOL        has_inst   = false;
    const char *field_part = NULL;

    if (bracket != NULL)
    {
        mod_len = (size_t)(bracket - prefix);

        DWORD       value  = 0U;
        BOOL        digits = false;
        const char *q      = bracket + 1U;

        while ((*q >= '0') && (*q <= '9'))
        {
            value  = (value * 10U) + (DWORD)(*q - '0');
            digits = true;
            q++;
            if (value > 0xFEU)
            {
                return false; /* 防止超长十进制回绕成合法 inst */
            }
        }
        if ((digits == false) || (*q != ']') || (value > 0xFEU))
        {
            return false;
        }
        want_inst = (BYTE)value;
        has_inst  = true;
        q++;
        if (*q == '.')
        {
            field_part = q + 1U;
        }
        else if (*q != '\0')
        {
            return false;
        }
    }
    else
    {
        mod_len = strlen(prefix);
    }

    const size_t module_len = strlen(module);

    // A：前缀的 module 部分不短于组名 ⇒ 必为 module 本身或其前缀，整组命中。
    if ((mod_len <= module_len) && (strncmp(prefix, module, mod_len) == 0))
    {
        if ((has_inst != false) && (want_inst != inst))
        {
            return false;
        }
        if (field_part == NULL)
        {
            return true;
        }
        if (name == NULL)
        {
            return false; /* 未保存字段名：只能按 module/inst 过滤 */
        }
        return (strncmp(name, field_part, strlen(field_part)) == 0) ? true : false;
    }

    // B：无 [inst] 时，允许 `module.name`（module 是前缀，下一字符是 '.'）。
    if ((bracket == NULL) && (mod_len > module_len) && (strncmp(prefix, module, module_len) == 0) && (prefix[module_len] == '.'))
    {
        if (name == NULL)
        {
            return false;
        }
        return (strncmp(name, &prefix[module_len + 1U], strlen(&prefix[module_len + 1U])) == 0) ? true : false;
    }

    return false;
}

/**
 * @brief 取字段的显示名（有名字用名字；SSD_STATS_NAMES=0 时用 s<index>）
 *
 * @param name  字段名（可为 NULL）
 * @param index 组内下标
 * @param out   输出缓冲
 * @param size  缓冲大小
 */
static void stats_field_name(const char *name, BYTE index, char *out, size_t size)
{
    if (name != NULL)
    {
        (void)snprintf(out, size, "%s", name);
    }
    else
    {
        (void)snprintf(out, size, "s%hhu", index);
    }
}

/**
 * @brief 组内是否有匹配前缀的条目（标量或直方图）
 *
 * @param g      组
 * @param prefix 前缀（NULL = 全部）
 * @return true 有匹配项
 */
static BOOL stats_group_has_entry(const stats_group_s *g, const char *prefix)
{
    for (BYTE j = 0U; j < g->count; j++)
    {
        if (stats_entry_matches(g->module, g->inst, g->names[j].name, prefix) != false)
        {
            return true;
        }
    }

    for (BYTE j = 0U; j < g->hist_count; j++)
    {
        if (stats_entry_matches(g->module, g->inst, g->hists[j].name, prefix) != false)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 组内是否有匹配前缀的标量（决定是否打印表头）
 *
 * @param g      组
 * @param prefix 前缀（NULL = 全部）
 * @return true 有匹配标量
 */
static BOOL stats_group_has_scalar(const stats_group_s *g, const char *prefix)
{
    for (BYTE j = 0U; j < g->count; j++)
    {
        if (stats_entry_matches(g->module, g->inst, g->names[j].name, prefix) != false)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 组表格的名字列宽：匹配项的最长显示名（含直方图）
 *
 * 名字列宽按组计算，别组不受影响；上限 STATS_TABLE_NAME_W_MAX 保证行宽
 * （前缀 + 缩进 + 名字 + 数值）不超 UART 日志行预算。
 *
 * @param g      组
 * @param prefix 前缀（NULL = 全部）
 * @return 名字列宽（至少容纳表头 "counter"）
 */
static BYTE stats_group_name_width(const stats_group_s *g, const char *prefix)
{
    char field[STATS_LINE_NAME_MAX];
    BYTE width = (BYTE)strlen("counter");

    for (BYTE j = 0U; j < g->count; j++)
    {
        if (stats_entry_matches(g->module, g->inst, g->names[j].name, prefix) == false)
        {
            continue;
        }
        stats_field_name(g->names[j].name, j, field, sizeof(field));
        const size_t len = strlen(field);

        if (len > STATS_TABLE_NAME_W_MAX)
        {
            width = STATS_TABLE_NAME_W_MAX;
        }
        else if (len > width)
        {
            width = (BYTE)len;
        }
    }

    for (BYTE j = 0U; j < g->hist_count; j++)
    {
        if (stats_entry_matches(g->module, g->inst, g->hists[j].name, prefix) == false)
        {
            continue;
        }
        stats_field_name(g->hists[j].name, j, field, sizeof(field));
        const size_t len = strlen(field);

        if (len > STATS_TABLE_NAME_W_MAX)
        {
            width = STATS_TABLE_NAME_W_MAX;
        }
        else if (len > width)
        {
            width = (BYTE)len;
        }
    }

    return width;
}

/**
 * @brief 打印组头（module 或 module[inst]）
 *
 * @param g 组
 */
static void stats_log_group_header(const stats_group_s *g)
{
    if (g->inst == STATS_INST_NONE)
    {
        LOGI(LOG_TAG_STAT "%s\r\n", g->module);
    }
    else
    {
        LOGI(LOG_TAG_STAT "%s[%hhu]\r\n", g->module, g->inst);
    }
}

/**
 * @brief 打印表格列头（counter / value）
 *
 * @param name_w 名字列宽
 */
static void stats_log_table_header(BYTE name_w)
{
    LOGI(LOG_TAG_STAT "  %-*s  %10s\r\n", name_w, "counter", "value");
}

/**
 * @brief 打印一行标量（名字左对齐、数值右对齐）
 *
 * @param field  显示名
 * @param value  值（delta=true 时是差值）
 * @param delta  true = 差值格式（带 +）
 * @param name_w 名字列宽
 */
static void stats_log_row(const char *field, DWORD value, BOOL delta, BYTE name_w)
{
    if (delta != false)
    {
        LOGI(LOG_TAG_STAT "  %-*s  +%9lu\r\n", name_w, field, value);
    }
    else
    {
        LOGI(LOG_TAG_STAT "  %-*s  %10lu\r\n", name_w, field, value);
    }
}

/**
 * @brief 取直方图第 index 个单元格的值（0..8 = 桶，9 = over）
 *
 * @param hist  直方图
 * @param index 单元格下标（< STATS_HIST_LABEL_NUM）
 * @return 计数
 */
static DWORD stats_hist_cell(const stats_hist_s *hist, BYTE index)
{
    return (index < STATS_HIST_BUCKET_NUM) ? hist->bucket[index] : hist->over;
}

/**
 * @brief 打印一个直方图（text：摘要 + 2×5 对齐桶表；brief：只打摘要）
 *
 * @param field 显示名
 * @param hist  直方图值
 * @param brief true = 省略桶表
 */
static void stats_log_hist_text(const char *field, const stats_hist_s *hist, BOOL brief)
{
    LOGI(LOG_TAG_STAT "  %s  n=%lu  max=%luus  over=%lu\r\n", field, hist->count, hist->max_us, hist->over);

    if (brief != false)
    {
        return;
    }

    // 2×5 紧凑表：每格「标签 值」，格间 ` | `；桶上界见 s_hist_labels。
    for (BYTE row = 0U; row < 2U; row++)
    {
        char       line[96];
        size_t     pos   = 0U;
        const BYTE first = (BYTE)(row * STATS_HIST_CELLS_PER_ROW);
        const BYTE last  = (BYTE)(((first + STATS_HIST_CELLS_PER_ROW) < STATS_HIST_LABEL_NUM) ? (first + STATS_HIST_CELLS_PER_ROW) : STATS_HIST_LABEL_NUM);

        line[0] = '\0';

        for (BYTE i = first; i < last; i++)
        {
            const int n = snprintf(&line[pos], sizeof(line) - pos, "%s%s %lu", (i == first) ? "" : " | ", s_hist_labels[i], stats_hist_cell(hist, i));

            if ((n > 0) && ((size_t)n < (sizeof(line) - pos)))
            {
                pos += (size_t)n;
            }
        }
        LOGI(LOG_TAG_STAT "    %s\r\n", line);
    }
}

/**
 * @brief 打印一个直方图（摘要 + 可选 2×5 桶表）
 *
 * @param h     直方图描述
 * @param index 组内下标
 * @param hist  直方图值
 * @param brief true = 省略桶表
 */
static void stats_log_hist(const stats_hist_name_s *h, BYTE index, const stats_hist_s *hist, BOOL brief)
{
    char field[STATS_LINE_NAME_MAX];

    stats_field_name(h->name, index, field, sizeof(field));
    stats_log_hist_text(field, hist, brief);
}

/**
 * @brief 打印一条标量统计（CSV：module,inst,name,value）
 *
 * @param g     组
 * @param n     字段
 * @param index 组内下标
 * @param value 值
 */
static void stats_log_entry_csv(const stats_group_s *g, const stats_name_s *n, BYTE index, DWORD value)
{
    char field[STATS_LINE_NAME_MAX];

    stats_field_name(n->name, index, field, sizeof(field));

    if (g->inst == STATS_INST_NONE)
    {
        LOGI(LOG_TAG_STAT "%s,,%s,%lu\r\n", g->module, field, value);
    }
    else
    {
        LOGI(LOG_TAG_STAT "%s,%hhu,%s,%lu\r\n", g->module, g->inst, field, value);
    }
}

/**
 * @brief 打印一个直方图（CSV：每个桶/over/count/max 一行）
 *
 * @param g     组
 * @param h     直方图描述
 * @param index 组内下标
 * @param hist  直方图值
 */
static void stats_log_hist_csv(const stats_group_s *g, const stats_hist_name_s *h, BYTE index, const stats_hist_s *hist)
{
    char field[STATS_LINE_NAME_MAX];

    stats_field_name(h->name, index, field, sizeof(field));

    for (BYTE b = 0U; b < STATS_HIST_BUCKET_NUM; b++)
    {
        if (g->inst == STATS_INST_NONE)
        {
            LOGI(LOG_TAG_STAT "%s,,%s.b%hu,%lu\r\n", g->module, field, b, hist->bucket[b]);
        }
        else
        {
            LOGI(LOG_TAG_STAT "%s,%hhu,%s.b%hu,%lu\r\n", g->module, g->inst, field, b, hist->bucket[b]);
        }
    }

    if (g->inst == STATS_INST_NONE)
    {
        LOGI(LOG_TAG_STAT "%s,,%s.over,%lu\r\n", g->module, field, hist->over);
        LOGI(LOG_TAG_STAT "%s,,%s.count,%lu\r\n", g->module, field, hist->count);
        LOGI(LOG_TAG_STAT "%s,,%s.max_us,%lu\r\n", g->module, field, hist->max_us);
    }
    else
    {
        LOGI(LOG_TAG_STAT "%s,%hhu,%s.over,%lu\r\n", g->module, g->inst, field, hist->over);
        LOGI(LOG_TAG_STAT "%s,%hhu,%s.count,%lu\r\n", g->module, g->inst, field, hist->count);
        LOGI(LOG_TAG_STAT "%s,%hhu,%s.max_us,%lu\r\n", g->module, g->inst, field, hist->max_us);
    }
}

/**
 * @brief 生成按 (module, inst) 排序的组下标表（稳定输出）
 *
 * @param order 输出（长度 STATS_GROUP_MAX）
 * @return 排序后的组数
 */
static BYTE stats_group_order(BYTE order[STATS_GROUP_MAX])
{
    for (BYTE i = 0U; i < s_group_count; i++)
    {
        order[i] = i;
    }

    // 插入排序：组数 <= 24，打印期一次，代价可忽略。
    for (BYTE i = 1U; i < s_group_count; i++)
    {
        const BYTE key = order[i];
        BYTE       j   = i;

        while (j > 0U)
        {
            const stats_group_s *a = &s_groups[order[j - 1U]];
            const stats_group_s *b = &s_groups[key];
            const int            c = strcmp(a->module, b->module);

            if ((c < 0) || ((c == 0) && (a->inst <= b->inst)))
            {
                break;
            }
            order[j] = order[j - 1U];
            j--;
        }
        order[j] = key;
    }
    return s_group_count;
}

/**
 * @brief 重新分配各组的 delta 基线扁平下标
 *
 * @return true  总条目数 <= STATS_ENTRY_MAX
 *         false 超出容量（调用者回滚本次注册）
 */
static BOOL stats_reindex(void)
{
    WORD flat = 0U;

    for (BYTE i = 0U; i < s_group_count; i++)
    {
        s_groups[i].entry_base = flat;
        flat                   = (WORD)(flat + s_groups[i].count);
        if (flat > STATS_ENTRY_MAX)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 按 (module, inst) 查找组
 *
 * @param module 组名
 * @param inst   实例号
 * @return 组下标；STATS_GROUP_INVALID = 未找到
 */
static BYTE stats_find_group(const char *module, BYTE inst)
{
    for (BYTE i = 0U; i < s_group_count; i++)
    {
        if ((s_groups[i].inst == inst) && (strcmp(s_groups[i].module, module) == 0))
        {
            return i;
        }
    }
    return STATS_GROUP_INVALID;
}

/**
 * @brief 校验一组标量名字表（注册前一次性检查）
 *
 * @param module 组名
 * @param names  名字表
 * @param count  条目数
 * @return true 合法；false 非法（调用者在 release 记日志后拒绝）
 */
static BOOL stats_names_valid(const char *module, const stats_name_s *names, BYTE count)
{
    for (BYTE i = 0U; i < count; i++)
    {
        const stats_name_s *n = &names[i];

        if ((n->kind >= STATS_KIND_NUM) ||
            ((n->width != STATS_U8) && (n->width != STATS_U16) && (n->width != STATS_U32)))
        {
            return false;
        }
        if (n->name == NULL)
        {
#    if SSD_STATS_NAMES
            return false;
#    else
            continue;
#    endif
        }
        if ((strlen(module) + 1U + strlen(n->name)) >= STATS_LINE_NAME_MAX)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 校验一组直方图名字表
 *
 * @param module 组名
 * @param hists  名字表
 * @param count  条目数
 * @return true 合法；false 非法
 */
static BOOL stats_hist_names_valid(const char *module, const stats_hist_name_s *hists, BYTE count)
{
    for (BYTE i = 0U; i < count; i++)
    {
        const stats_hist_name_s *h = &hists[i];

        if (h->name == NULL)
        {
#    if SSD_STATS_NAMES
            return false;
#    else
            continue;
#    endif
        }
        if ((strlen(module) + 1U + strlen(h->name)) >= STATS_LINE_NAME_MAX)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 当前直方图总数
 *
 * @return 所有组 hist_count 之和
 */
static WORD stats_hist_total(void)
{
    WORD total = 0U;

    for (BYTE i = 0U; i < s_group_count; i++)
    {
        total = (WORD)(total + s_groups[i].hist_count);
    }
    return total;
}

/**
 * @brief 注册（或更新）一个统计组的标量表
 *
 * @param module 组名（静态字符串）
 * @param inst   实例号；STATS_INST_NONE = 无实例维度
 * @param base   宿主对象地址
 * @param names  名字表
 * @param count  条目数
 * @return true 注册/更新成功；false 参数非法或容量不足
 */
BOOL stats_register(const char *module, BYTE inst, void *base, const stats_name_s *names, BYTE count)
{
    ASSERT(module != NULL);
    ASSERT(base != NULL);
    ASSERT(names != NULL);
    ASSERT(count > 0U);

    if ((module == NULL) || (base == NULL) || (names == NULL) || (count == 0U))
    {
        LOGE(LOG_TAG_STAT "register: invalid argument\r\n");
        return false;
    }
    if (stats_names_valid(module, names, count) == false)
    {
        ASSERT(false);
        LOGE(LOG_TAG_STAT "register: bad name table module=%s\r\n", module);
        return false;
    }

    const BYTE existing = stats_find_group(module, inst);

    if (existing != STATS_GROUP_INVALID)
    {
        // 同一 (module, inst) 幂等更新；条目数变化会让 delta 扁平下标错位，
        // 属配置错误（只有重新编译才会变），直接拒绝而不是重排。
        if (s_groups[existing].count != count)
        {
            ASSERT(false);
            LOGE(LOG_TAG_STAT "register: count mismatch module=%s inst=%hhu\r\n", module, inst);
            return false;
        }
        s_groups[existing].base  = base;
        s_groups[existing].names = names;
        return true;
    }

    if (s_group_count >= STATS_GROUP_MAX)
    {
        LOGE(LOG_TAG_STAT "register: group table full (%u) module=%s\r\n", STATS_GROUP_MAX, module);
        return false;
    }

    stats_group_s *g = &s_groups[s_group_count];
    g->module        = module;
    g->inst          = inst;
    g->base          = base;
    g->names         = names;
    g->count         = count;
    g->hists         = NULL;
    g->hist_count    = 0U;
    g->entry_base    = 0U;
    s_group_count++;

    if (stats_reindex() == false)
    {
        s_group_count--;
        (void)stats_reindex();
        LOGE(LOG_TAG_STAT "register: entry table full (%u) module=%s\r\n", STATS_ENTRY_MAX, module);
        return false;
    }
    return true;
}

/**
 * @brief 给已注册的组附加直方图表（必须在 stats_register 之后调用）
 *
 * @param module 组名（必须已注册）
 * @param inst   实例号
 * @param base   同一宿主对象地址
 * @param hists  直方图表（静态数组）
 * @param count  条目数（>0）
 * @return true 注册成功；false 组未注册 / 参数非法 / 直方图总数超限
 */
BOOL stats_register_hist(const char *module, BYTE inst, void *base, const stats_hist_name_s *hists, BYTE count)
{
    ASSERT(module != NULL);
    ASSERT(base != NULL);
    ASSERT(hists != NULL);
    ASSERT(count > 0U);

    if ((module == NULL) || (base == NULL) || (hists == NULL) || (count == 0U))
    {
        LOGE(LOG_TAG_STAT "register_hist: invalid argument\r\n");
        return false;
    }
    if (stats_hist_names_valid(module, hists, count) == false)
    {
        ASSERT(false);
        LOGE(LOG_TAG_STAT "register_hist: bad name table module=%s\r\n", module);
        return false;
    }

    const BYTE index = stats_find_group(module, inst);

    if (index == STATS_GROUP_INVALID)
    {
        ASSERT(false);
        LOGE(LOG_TAG_STAT "register_hist: group missing module=%s inst=%hhu\r\n", module, inst);
        return false;
    }

    stats_group_s *g = &s_groups[index];

    if ((BYTE *)base != g->base)
    {
        ASSERT(false);
        LOGE(LOG_TAG_STAT "register_hist: base mismatch module=%s inst=%hhu\r\n", module, inst);
        return false;
    }

    const WORD old = g->hist_count;
    const WORD sum = (WORD)(stats_hist_total() - old + count);

    if (sum > STATS_HIST_TOTAL_MAX)
    {
        LOGE(LOG_TAG_STAT "register_hist: hist table full (%u) module=%s\r\n", STATS_HIST_TOTAL_MAX, module);
        return false;
    }

    g->base       = base;
    g->hists      = hists;
    g->hist_count = count;
    return true;
}

/**
 * @brief 观测一次延迟（µs），更新桶 / over / count / max_us
 *
 * @param hist     直方图（非空）
 * @param value_us 延迟（µs）
 */
void stats_hist_observe(stats_hist_s *hist, DWORD value_us)
{
    ASSERT(hist != NULL);

    DWORD *slot = &hist->over;

    for (BYTE i = 0U; i < STATS_HIST_BUCKET_NUM; i++)
    {
        if (value_us < s_hist_bounds[i])
        {
            slot = &hist->bucket[i];
            break;
        }
    }

    STATS_INC(*slot);
    STATS_INC(hist->count);
    if (value_us > hist->max_us)
    {
        hist->max_us = value_us;
    }
}

/**
 * @brief 打印已注册的统计（只读）
 *
 * prefix 支持 `module` / `module.name` / `module[inst]` / `module[inst].name`。
 *
 * @param prefix module / module.name / module[inst] / module[inst].name；NULL = 全部
 * @param format text / csv / brief
 * @return true 至少打印一项；false 无匹配
 */
BOOL stats_print(const char *prefix, stats_format_e format)
{
    BYTE       order[STATS_GROUP_MAX];
    BOOL       printed = false;
    const BOOL brief   = (format == STATS_FORMAT_BRIEF) ? true : false;
    const BOOL csv     = (format == STATS_FORMAT_CSV) ? true : false;

    ASSERT(format < STATS_FORMAT_NUM);
    (void)stats_group_order(order);

    for (BYTE k = 0U; k < s_group_count; k++)
    {
        const stats_group_s *g      = &s_groups[order[k]];
        BYTE                 name_w = 0U;

        // 文本格式：每组打一次组头 + 列头；CSV 保持一行一项，不加表头。
        if (csv == false)
        {
            if (stats_group_has_entry(g, prefix) == false)
            {
                continue;
            }
            name_w = stats_group_name_width(g, prefix);
            stats_log_group_header(g);
            if (stats_group_has_scalar(g, prefix) != false)
            {
                stats_log_table_header(name_w);
            }
        }

        for (BYTE j = 0U; j < g->count; j++)
        {
            const stats_name_s *n = &g->names[j];

            if (stats_entry_matches(g->module, g->inst, n->name, prefix) == false)
            {
                continue;
            }
            if (csv != false)
            {
                stats_log_entry_csv(g, n, j, stats_value_read(g->base, n));
            }
            else
            {
                char field[STATS_LINE_NAME_MAX];

                stats_field_name(n->name, j, field, sizeof(field));
                stats_log_row(field, stats_value_read(g->base, n), false, name_w);
            }
            printed = true;
        }

        for (BYTE j = 0U; j < g->hist_count; j++)
        {
            const stats_hist_name_s *h = &g->hists[j];

            if (stats_entry_matches(g->module, g->inst, h->name, prefix) == false)
            {
                continue;
            }
            stats_hist_s hist;

            stats_hist_read(g->base, h->offset, &hist);
            if (csv != false)
            {
                stats_log_hist_csv(g, h, j, &hist);
            }
            else
            {
                stats_log_hist(h, j, &hist, brief);
            }
            printed = true;
        }
    }
    return printed;
}

/**
 * @brief 只列出已注册的组与条目数（不打印值）
 */
void stats_list(void)
{
    BYTE order[STATS_GROUP_MAX];

    (void)stats_group_order(order);
    LOGI(LOG_TAG_STAT "groups=%hu\r\n", s_group_count);

    for (BYTE k = 0U; k < s_group_count; k++)
    {
        const stats_group_s *g = &s_groups[order[k]];

        if (g->inst == STATS_INST_NONE)
        {
            LOGI(LOG_TAG_STAT "%-16s scalars=%hu hists=%hu\r\n", g->module, g->count, g->hist_count);
        }
        else
        {
            LOGI(LOG_TAG_STAT "%-12s[%hhu] scalars=%hu hists=%hu\r\n", g->module, g->inst, g->count, g->hist_count);
        }
    }
}

/**
 * @brief 打印自上次 delta 基线以来的变化（首次调用只建基线）
 *
 * @param prefix module 或 module.name 前缀；NULL = 全部
 * @return true 至少打印一项 / 首次建立基线；false 无匹配
 */
BOOL stats_delta(const char *prefix)
{
    if (s_baseline_valid == false)
    {
        WORD flat = 0U;

        for (BYTE i = 0U; i < s_group_count; i++)
        {
            const stats_group_s *g = &s_groups[i];

            for (BYTE j = 0U; j < g->count; j++)
            {
                s_baseline[flat] = stats_value_read(g->base, &g->names[j]);
                flat++;
            }
        }
        s_baseline_ms = platform_now_ms();
#    if SSD_STATS_PERSIST_ENABLE
        lifetime_get_bytes(&s_baseline_read_bytes, &s_baseline_write_bytes);
#    endif
        s_baseline_valid = true;
        LOGI(LOG_TAG_STAT "baseline stored (%hu entries)\r\n", flat);
        return true;
    }

    BYTE order[STATS_GROUP_MAX];
    BOOL printed = false;

    (void)stats_group_order(order);

    const DWORD now_ms = platform_now_ms();

#    if SSD_STATS_PERSIST_ENABLE
    const DWORD elapsed_ms = ((s_baseline_ms != 0U) && (now_ms >= s_baseline_ms)) ? (now_ms - s_baseline_ms) : 0U;
    QWORD       read_now   = 0U;
    QWORD       write_now  = 0U;

    lifetime_get_bytes(&read_now, &write_now);

    if ((prefix == NULL) && (elapsed_ms != 0U))
    {
        const QWORD read_delta  = read_now - s_baseline_read_bytes;
        const QWORD write_delta = write_now - s_baseline_write_bytes;
        const QWORD denom       = (QWORD)elapsed_ms * 1024U;

        LOGI(LOG_TAG_STAT "window elapsed=%lums host_read=%lluB (%lluKB/s) host_write=%lluB (%lluKB/s)\r\n", elapsed_ms, read_delta, (denom != 0U) ? ((read_delta * 1000U) / denom) : 0U, write_delta, (denom != 0U) ? ((write_delta * 1000U) / denom) : 0U);
    }
    // 带 prefix 的 delta 也要推进吞吐基线，否则下一次全局 delta 会跨过它。
    s_baseline_read_bytes  = read_now;
    s_baseline_write_bytes = write_now;
#    endif
    s_baseline_ms = now_ms;

    for (BYTE k = 0U; k < s_group_count; k++)
    {
        const stats_group_s *g      = &s_groups[order[k]];
        BYTE                 name_w = 0U;

        if (stats_group_has_entry(g, prefix) != false)
        {
            name_w = stats_group_name_width(g, prefix);
            stats_log_group_header(g);
            if (stats_group_has_scalar(g, prefix) != false)
            {
                stats_log_table_header(name_w);
            }
        }

        for (BYTE j = 0U; j < g->count; j++)
        {
            const stats_name_s *n   = &g->names[j];
            const WORD          idx = (WORD)(g->entry_base + j);
            const DWORD         cur = stats_value_read(g->base, n);

            if (stats_entry_matches(g->module, g->inst, n->name, prefix) != false)
            {
                char field[STATS_LINE_NAME_MAX];

                stats_field_name(n->name, j, field, sizeof(field));
                if (n->kind == STATS_COUNTER)
                {
                    stats_log_row(field, cur - s_baseline[idx], true, name_w);
                }
                else
                {
                    stats_log_row(field, cur, false, name_w);
                }
                printed = true;
            }
            // 未匹配的项也更新基线：否则带前缀的 delta 会让其他组出现假差值。
            s_baseline[idx] = cur;
        }

        for (BYTE j = 0U; j < g->hist_count; j++)
        {
            const stats_hist_name_s *h = &g->hists[j];

            if (stats_entry_matches(g->module, g->inst, h->name, prefix) == false)
            {
                continue;
            }
            // 直方图只打当前值；桶差值需要额外基线 RAM，P2 不做。
            stats_hist_s hist;

            stats_hist_read(g->base, h->offset, &hist);
            stats_log_hist(h, j, &hist, false);
            printed = true;
        }
    }
    return printed;
}

/**
 * @brief 清零匹配的 counter 与直方图桶（gauge / max_us 不动），并重置 delta 基线
 *
 * @param prefix module 或 module.name 前缀；NULL = 全部
 */
void stats_clear(const char *prefix)
{
    for (BYTE i = 0U; i < s_group_count; i++)
    {
        stats_group_s *g = &s_groups[i];

        for (BYTE j = 0U; j < g->count; j++)
        {
            const stats_name_s *n = &g->names[j];

            if ((n->kind == STATS_COUNTER) && (stats_entry_matches(g->module, g->inst, n->name, prefix) != false))
            {
                stats_value_store(g->base, n, 0U);
            }
        }

        for (BYTE j = 0U; j < g->hist_count; j++)
        {
            const stats_hist_name_s *h = &g->hists[j];

            if (stats_entry_matches(g->module, g->inst, h->name, prefix) == false)
            {
                continue;
            }

            stats_hist_s hist;

            stats_hist_read(g->base, h->offset, &hist);
            for (BYTE b = 0U; b < STATS_HIST_BUCKET_NUM; b++)
            {
                hist.bucket[b] = 0U;
            }
            hist.over  = 0U;
            hist.count = 0U;
            // max_us 是高水位 gauge，clear 不清。
            memcpy(&g->base[h->offset], &hist, sizeof(hist));
        }
    }

    memset(s_baseline, 0, sizeof(s_baseline));
    s_baseline_valid = false;
    s_baseline_ms    = 0U;
#    if SSD_STATS_PERSIST_ENABLE
    s_baseline_read_bytes  = 0U;
    s_baseline_write_bytes = 0U;
#    endif
}

#endif /* SSD_STATS_ENABLE */
