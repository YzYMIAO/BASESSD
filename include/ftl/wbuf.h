#ifndef FTL_WBUF_H
#define FTL_WBUF_H

/*
 * wbuf.h — 页打包写缓冲（page-packing write buffer）
 *
 * CW 级映射下子操作常只覆盖半个 pair 窗口（如一次 512B 写只落 plane0 的 CW0），
 * 而器件**按页编程**（参数页 Byte86-89 部分页单位 = 1 CW；Byte110 每 ECC user area
 * 只能编一次）。逐子操作分配页会让每次写都吃掉一整对页 ⇒ 利用率 12.5%~50%。
 *
 * 本缓冲把**同一 pair 窗口**内的多次 host 写攒成一页，窗口填满 / 切到别的窗口 /
 * 显式 flush / 空闲时**一次编程**（两 plane 都有数据即一条 multi-plane）⇒ 写指针
 * 按页推进，页内利用率回到 100%。
 *
 * data[slot] 的 slot = plane*CW_NUM + cw（与 LBA 低 3 位同构，见 ftl_lba_pair_slot），
 * occupied 逐位标记有效槽。owners 记录「哪些组向本页贡献过」——一页可能由多个组拼成，
 * 完成时必须**逐组**递减在飞数（见 pending_group_complete_subop_multi）。
 *
 * ⚠ 掉电语义（无电容）：缓冲里的数据**不落 journal/L2P**——只有 flush 编程成功的
 *   completion 才确认映射（与既有顺序一致）。掉电时这些写组仍 outstanding（从未完成），
 *   丢失即正确契约；不变量「L2P/journal 只引用已编程页」由构造保证。
 *
 * ⚠ 不变量：`occupied` 清空（TRIM 掉光 / 显式复位）时**必须**退掉 `owners` 的在飞数，
 *   否则那些组永不完成（host 写挂死）。该动作收在 `ftl_wbuf_drop` / `ftl_wbuf_reset`
 *   内部，**不**由调用者手工维护（见 CLAUDE.md §3.1）。
 */

#include "ftl_config.h" /* FTL_LBA_PER_PAIR / FTL_IU_BYTES / lba_t / ftl_status_t */

/* 前向声明（ftl_context_t 在 ftl.h 中定义；本文件被 ftl.h include，
 * 用 struct 标签前向引用打破环） */
struct ftl_context;
typedef struct ftl_context ftl_context_t;

/* ── 写缓冲状态（内嵌于 ftl_context_t）── */
typedef struct
{
    uint8_t  data[FTL_LBA_PER_PAIR * FTL_IU_BYTES]; /* 8 CW × 512B = 4096B 页镜像 */
    uint8_t  occupied;                              /* bit i = 槽 i 有数据（i = plane*CW_NUM+cw） */
    uint8_t  open;                                  /* 1 = 正持有某窗口 */
    lba_t    base;                                  /* 持有页的 pair 窗口基址（低 3 位清零） */
    uint32_t owners;                                /* bit gi = 组 gi 贡献过本页（FTL_GROUP_MAX=32） */
} ftl_wbuf_t;

/*
 * ftl_wbuf_has - 该**全局** LBA 是否正被写缓冲持有（未编程）
 *
 * 命中判定：窗口匹配 + 该槽位已置位。读路径用它做**收缩判据**（只判不取）。
 * 取值请用 ftl_wbuf_read。
 *
 * @param ftl  FTL 上下文
 * @param lba  全局 host LBA
 * @return true = 缓冲持有该 LBA（其映射尚未落盘）
 */
bool ftl_wbuf_has(const ftl_context_t *ftl, lba_t lba);

/*
 * ftl_wbuf_read - 命中则把该 LBA 从写缓冲拷出
 *
 * 缓冲里的数据比 L2P 新（该 LBA 的映射尚未落盘）⇒ 命中时必须从缓冲取，
 * 否则会读到陈旧映射。
 *
 * @param ftl  FTL 上下文
 * @param lba  全局 host LBA
 * @param out  输出缓冲（FTL_IU_BYTES 字节）
 * @return true = 命中并已拷出；false = 未命中（out 未写）
 */
bool ftl_wbuf_read(const ftl_context_t *ftl, lba_t lba, void *out);

/*
 * ftl_wbuf_drop - 命中则丢弃该 LBA 在缓冲里的数据（TRIM 用）
 *
 * 该 LBA 从未落盘 ⇒ 无旧映射可失效、也无映射变更需记，只清缓冲槽。
 * 槽清空到 0 时**窗口整页作废**：退掉全部贡献组的在飞数（它们从未真正写盘）并复位窗口。
 *
 * @param ftl  FTL 上下文
 * @param lba  全局 host LBA
 * @return true = 命中并已丢弃（调用者据此跳过 L2P/journal 路径）；false = 未命中
 */
bool ftl_wbuf_drop(ftl_context_t *ftl, lba_t lba);

/*
 * ftl_wbuf_append - 把窗口内的 n 个 CW 攒进缓冲；换窗口 / 满时触发编程
 *
 * ⚠ n 必须已被调用者夹在**窗口边界内**（见 ftl_issue_subop）——跨窗口的 append 会把
 *   别的窗口的数据混进本页。
 *
 * @param ftl   FTL 上下文
 * @param gi    贡献组号（记入 owners）
 * @param lba   本段起始全局 LBA
 * @param buffer 数据源（n × FTL_IU_BYTES）
 * @param n     CW 数
 * @return FTL_OK 已攒入 / FTL_AGAIN / FTL_ERROR（换窗口 flush 失败时透传，此时**不消费**
 *         本子操作 ⇒ 调用者 g->next 不前进，下轮重试）
 */
ftl_status_t ftl_wbuf_append(ftl_context_t *ftl, uint16_t gi, lba_t lba, const void *buffer, uint8_t n);

/*
 * ftl_wbuf_flush - 把当前缓冲页编程到 NAND（一页 = 一次编程，两 plane 都有数据即 multi-plane）
 *
 * @param ftl  FTL 上下文
 * @return FTL_OK 已提交 / FTL_AGAIN 背压（缓冲保留，下次重试）/ FTL_ERROR 建链或搬数据失败
 *         （**没有**「无空间」的 FTL_ERROR——取不到块改走 FTL_AGAIN + 起 GC job）
 */
ftl_status_t ftl_wbuf_flush(ftl_context_t *ftl);

/*
 * ftl_wbuf_maybe_flush_idle - 确定性空闲 flush（ftl_service Phase 2.5 末尾调用）
 *
 * 若无任何**未下发完的写组**的下一个 LBA 落在当前打开窗口内（不会再有 append 进来），
 * 就把缓冲编程掉。
 *
 * @param ftl  FTL 上下文
 */
void ftl_wbuf_maybe_flush_idle(ftl_context_t *ftl);

/*
 * ftl_wbuf_reset - 丢弃缓冲全部内容并复位窗口
 *
 * 供介质被整体擦除的诊断路径使用（缓冲里的 CW 若在擦净后被 flush，会把数据编到刚擦空的
 * 芯片上）。**必须**用它而不是裸 memset——`owners` 不能直接丢：那些组仍在等子操作完成，
 * 直接清零会让它们的在飞数永不归零、host 写挂死。本函数按**写失败**语义收尾。
 *
 * @param ftl  FTL 上下文
 */
void ftl_wbuf_reset(ftl_context_t *ftl);

#endif /* FTL_WBUF_H */
