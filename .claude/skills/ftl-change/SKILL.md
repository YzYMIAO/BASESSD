---
name: ftl-change
description: 修改 FTL 层代码（src/ftl/、include/ftl/）的流程与检查清单。当用户说「改 FTL」「加个 FTL 功能」「优化 FTL」「v0.xx」时使用。
---

# FTL 改动流程

FTL 层 = 映射 / GC / WL / journal / checkpoint / BBT / 掉电恢复。
本层最贵的缺陷是**不报错的**（CLAUDE.md §3.1）。

---

## 0. 动手前（CLAUDE.md P0 / P1）

1. **先问「主流主控怎么实现这件事」，并找出处**（§〇）。
   出处层：`docs/reference/README.md`、`docs/design/storage_controller_learning.md`、
   `docs/reference/pages/spdk_ftl.html`。⚠ **说不出出处就标「本仓自拟」**，不得写成「行业标准形态」。
   ⚠ 主流本身分岔（**DRAM 型 vs DRAM-less**）—— 说清对齐的是哪一支。
2. **读设计文档**：`docs/design/ftl_design.md` 对应章节的「N.1 最新策略」+「N.2 策略选取」。
   演进史查 `docs/design/ftl_revision_history.md`；历史细节查 `docs/archive/changelog/`。
3. **先 grep 已有 helper**（R9 / C2）—— 位操作 / 地址转换 / 结构体构造 / 相似分支，
   大概率已有等价物（`ftl_gblock_plane/block`、`ftl_lba_page_base`、`pba_same_page`、
   `pba_with_page` 都是这么收敛出来的）。

---

## 1. 动手时

### 不变量必须收敛成唯一闸门（§3.1 / H10 / H13）

- 同一不变量**不允许**由多个模块各自手工维护 —— 任一处漏掉就是**永久漏账**。
- 模块内部状态**只通过访问器**读写（范本：`allocator.h` 的 `allocator_*_count` / `closed_at`
  —— 封装**数组/不变量**的才留；只为兜住坏字段名的谓词已删，见 CLAUDE.md H13）。
- ⚠ `FTL_BLOCK_STATE_FREE` **会说谎**（`allocator_init` 早于元数据目录加载）——
  **选块一律以 Free Pool 数组为准**，不以 state 位为准。

### 断言（P3）

动断言前先读 `docs/design/assertion_guidelines.md`：
- **模块内部不逐函数复检同一入参**；边界 = host 返回码 + FTL↔FC/子模块入口 ASSERT（H2）。
- **判据 9**：后果是**静默介质破坏**的，除 debug `ASSERT` 外**必须**补 release 运行期拒绝。
- **判据 10**：被 ASSERT 守的变量在 `NDEBUG` 下变 unused ⇒ **release 档的编译告警**
  是「这条断言在 release 下没有等价守卫」的唯一提示 —— **别把 release 告警当噪音**。
- **有副作用的调用不能进 `ASSERT`**（`NDEBUG` 下表达式不求值，H14）。

### 坐标与语义

- **同一语义只允许一套坐标**（C9）；跨边界转换只允许在单一漏斗发生。
- **改语义/默认值必须回扫全部调用点**（C10）—— 「值不变但含义变了」编译期判不出来。

---

## 2. 验证（R2）

```bash
make all            # debug，零新告警
make release        # 体积 + 看 release 专属告警（判据 10）
make tests UT=FTL   # FTL 单测
```

⚠ **`make tests` 只证链接，不证正确。** 碰了 **建链 / 位图 / 地址** 三类量的改动
**必须上板**（上板流程见 skill `board-run`）。

⚠ 介质布局版本：改了元数据布局就要抬 `FTL_MEDIA_LAYOUT_VERSION`，
**上板首启会自动格式化** —— 这是预期行为，不是 bug，但要提前告诉用户。

---

## 3. 收尾自查

- [ ] diff 里每段新逻辑：**有没有等价物已经存在**？（R9）
- [ ] 新增的校验/功能：**给出「确实生效过」的证据**了吗？（C8 —— 跑通 ≠ 生效）
- [ ] 新写的断言：**能否失败**？负例在哪？（C7）
- [ ] 周边注释同步了吗？（H6）
- [ ] 体积口径用对了吗？（C6 —— `.text`/`.bss` 是 `arm-none-eabi-size` 输出，不是段尺寸）
- [ ] 验证状态如实标注了吗？（R1 —— 未上板就写「未上板」）
- [ ] **未提交、未推送**，由用户决定（R3）
