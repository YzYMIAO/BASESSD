# _sbrk 堆栈相撞 HardFault 调试记录 — 根因确认与修复

> **调试日期**: 2026-10-04
> **涉及模块**: `src/platform/stm32h743/syscalls.c`（新增）、`linker/stm32h743xx_flash.ld`（只读确认）、`tests/unit/test_rebuild.c`（触发点，未改动）
> **触发测试**: 板端 UART CLI `test` → `ut_run_all()`，跑到 `rebuild` 套件时 HardFault
> **芯片**: STM32H743II + MT29F4G08ABADAWP NAND
> **现状**: `_sbrk` 守卫已落地并**编译验证通过**（`make tests` 零告警零错误；map 确认 `_sbrk` 解析到 `syscalls.o`，`libnosys.a(sbrk.o)` 未被拉入）；**上板验证未做**。rebuild 套件夹具超容问题**未解决**，见 §5

---

## 1. 现象

GDB 停在 HardFault，回溯：

```
#0  HardFault_Handler () at .../stm32h7xx_it.c:75
#1  <signal handler called>
#2  memset (m=0x20012928, c=0, n=<optimized out>) at .../newlib/libc/string/memset.c:76
#3  0x00000000 in ?? ()
```

串口最后刷出的行是 `lifecycleblock_random_data` —— **这是误导**。munit 在**跑 setup 之前**就打印测试名，所以最后一行属于上一个套件，实际崩在下一个套件 `rebuild` 的 `setup` 里。

## 2. 根因

### 2.1 DTCM 布局：堆和栈在同一块内存里相向生长

`linker/stm32h743xx_flash.ld`：

```
DTCM: 0x20000000 .. 0x20020000（128 KB）

._user_heap_stack (NOLOAD) :      /* :142-150 */
{
    . = ALIGN(8);
    PROVIDE(end = .);             /* ← 堆起点，= 0x20000000 */
    . += _Min_Heap_Size;          /* 0x200 */
    . += _Min_Stack_Size;         /* 0x800 */
} > DTCM

_estack = ORIGIN(DTCM) + LENGTH(DTCM);   /* :36 → 0x20020000，栈顶 */
```

`.dtcm_data` 是 0 字节，所以 `end` 落在 DTCM 原点：

| 区域 | 地址 | 生长方向 |
|------|------|----------|
| 堆 | `end` = 0x20000000 起 | 向**上** |
| 栈 | `_estack` = 0x20020000 起 | 向**下** |

**两者之间没有任何守卫区。**

### 2.2 libnosys 的 `_sbrk` 不做上界检查

构建用 `-specs=nosys.specs`，`_sbrk` 来自 libgloss **libnosys**（不是 libgloss 主版本，后者有 `heap_end + incr > stack_ptr` 检查）。反汇编 `_sbrk`（0x0800312c）确认：只有 `heap_end += incr`，**无任何越界判断**。

### 2.3 触发链

`tests/unit/test_rebuild.c`：

```c
typedef struct {
    ftl_block_t      blocks[FTL_BLOCK_TOTAL];              /* 4096 × 56 B = 224 KB */
    uint16_t         free_blocks[FTL_BLOCK_TOTAL];         /* 4096 ×  2 B =   8 KB */
    super_block_t    closed_sb_list[SUPER_BLOCK_LIST_MAX]; /* 1024 × 12 B =  12 KB */
    ftl_allocator_t  alloc;
    metadata_dir_cache_t md_dir;
} rebuild_fixture_t;                                       /* ≈ 245 KB */

static void *rebuild_setup(...)
{
    rebuild_fixture_t *fx = (rebuild_fixture_t *)malloc(sizeof(*fx));  /* :277 → 245 KB */
    if (fx == NULL) { return NULL; }
    fixture_init(fx);          /* ← fixture_init 内 memset(fx, 0, sizeof(*fx)) */
    return fx;
}
```

1. `malloc(250848)` → `_sbrk` 返回旧堆顶 **0x20012928**（正是 bt 里的 `m`），把 `heap_end` 推到 **≈0x200516xx** —— 越过 `_estack` 0x20020000，且**越出 DTCM 末尾**。
2. `fixture_init` 执行 `memset(0x20012928, 0, 245 KB)`，一路写穿 DTCM 顶端 → BusFault → HardFault。

bt 三个实参全部对上：`m=0x20012928`（`_sbrk` 返回的旧堆顶）、`c=0`、`n` 被优化掉。

### 2.4 `-O0` 是无关变量

`config.mk:143` 调试构建为 `-O0 -g3 -ggdb3`。曾怀疑是 `-O0` 帧膨胀撑爆栈，**已排除**：

- `debug.size` 显示 `.dtcm_data` 0 字节、`._user_heap_stack` 2560 B @ 0x20000000 → 静态占用为 0，栈可用空间约 127 KB。
- `-O0` 的帧增长远不足以吃掉 127 KB。

**245 KB 的 malloc 在 `-O2` 下同样会撞穿 DTCM** —— 之前测试跑到 rebuild 才崩是必然，与优化等级无关。

## 3. 修复：给 `_sbrk` 加边界检查

新增 `src/platform/stm32h743/syscalls.c`（该目录 `makefile.mk:13` 用 `wildcard *.c` 收集，自动进构建）。目标文件里的强符号优先于归档成员，libnosys 的 `_sbrk` 被丢弃。

```c
#define SBRK_STACK_GUARD (0x400U)   /* 堆顶与实时 MSP 之间保留 1 KB */

void *_sbrk(ptrdiff_t incr)
{
    extern char end;                /* 链接脚本 PROVIDE(end = .) → 0x20000000 */

    static char *heap_end;
    char        *prev_heap_end;
    char        *limit;

    if (heap_end == NULL)
    {
        heap_end = &end;
    }

    prev_heap_end = heap_end;
    limit         = (char *)__get_MSP() - SBRK_STACK_GUARD;

    if ((heap_end + incr > limit) || (heap_end + incr < &end))
    {
        return (void *)-1;          /* _sbrk_r 据此置 errno = ENOMEM */
    }

    heap_end += incr;
    return (void *)prev_heap_end;
}
```

### 设计要点

| 点 | 理由 |
|----|------|
| 上界用**实时 MSP** 而非 `_estack` | 上限随栈的实时深度自动下移；写死 `_estack - N` 挡不住栈已经压下来的情况 |
| 保留 `SBRK_STACK_GUARD` = 1 KB | 放行那一刻可能已贴着栈顶，本函数 + `_sbrk_r` + `malloc` 还要落帧 |
| `incr = 0` 必须成功 | newlib nano-mallocr 首次 `malloc` 会用 `_sbrk_r(ptr, 0)` 记录 `__malloc_sbrk_start`；`0` 走 `heap_end + 0 <= limit` 正常返回 |
| 下界 `&end` | `free` 收缩堆时传负 `incr`，不能让它退到堆起点以下 |
| 失败**不改** `heap_end` | 后续调用看到一致的堆顶 |
| 形参 `ptrdiff_t` | 与 newlib `_sbrk_r` 的实参类型一致（ARM 上同为 32 位） |

## 4. 验证

- [ ] `make` 零警告零错误
- [ ] `make tests` 零警告零错误
- [ ] `make format-check` 通过（clang-format-15）
- [ ] `arm-none-eabi-nm build/firmware/debug/bin/debug.elf | grep _sbrk` —— 地址应指向 `platform/stm32h743` 的目标文件，不再是 libnosys
- [ ] 上板：`test` 跑到 rebuild 时**不再 HardFault**（预期变为 setup 失败，见 §5）

## 5. 遗留：rebuild 套件夹具放不下（未解决）

`rebuild_fixture_t` ≈ **245 KB**，而堆 = DTCM = **128 KB**（栈还共用这块）。**245 KB > 128 KB，物理上装不下**，与 `_sbrk` 有无守卫无关。

加守卫后 `malloc` 返回 NULL → `rebuild_setup` 返回 NULL → **是否干净失败取决于 vendored munit 对「setup 返回 NULL」的处理**：若它把 NULL 当 fixture 传给测试体，测试体里的 `fx->...` 会再次空指针解引用 → 换地址再崩。**此项待确认。**

真正的修法（`test_rebuild.c` 属测试源文件，按项目规则由作者自建）：

1. 夹具改 `static` 并放到 AXI_RAM（384 KB，够）—— 改动最小，推荐。
2. 测试构建下缩小 `FTL_BLOCK_TOTAL` —— 编译期常量，影响面大。
3. 把 rebuild 套件从板端 munit 摘掉，只在纯 RAM 环境跑。

## 6. 附带发现（未修）

`src/fc/fc.c` `fc_fill_cw_meta_paa()` 直接调用未做占用检查的 `fc_buffer_id_to_real_buffer_internal()`，而对外薄壳 `fc_buffer_id_to_real_buffer()` 是有 `fc_buffer_is_occupied()` 检查的。属于既有的非对称隐患，与本次故障无关。
