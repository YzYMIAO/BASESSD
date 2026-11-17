# BCH ECC 引擎 clean-room 替换（2026-11-17）

## 背景

`src/fc/fc_bch_engine.c` 原文件头自述「移植自 Linux 内核 `lib/bch.c`（GPL v2），作者 Ivan Djelic」。
Linux `lib/bch.c` 为 GPL-2.0-only；本项目根许可证是 Apache-2.0，二者在组合分发上不兼容。
该问题在开源审计中由 `docs/archive/changelog/fc_structural_changes.md` 的 Phase K 记录暴露。

## 改动

- 重写 `src/fc/fc_bch_engine.c` 与 `include/fc/fc_bch_engine.h` 的实现/注释：
  - 固定参数 m=13、t=4、生成根 α^1/α^3/α^5/α^7；
  - 独立实现 GF(2^13) 表、生成多项式、LFSR 编码、Berlekamp–Massey 与根搜索；
  - 公共 API、ECC 52-bit 布局、错误位置语义保持不变；
  - 不包含 Linux `lib/bch.c` 或其它第三方源码。
- `THIRD_PARTY_NOTICES.md` 增加 BCH 引擎归属说明（本项目 clean-room，Apache-2.0）。
- `docs/design/known_open_items.md` 登记 `RISK-REL-06`（待上板）。

## 验证

- **host 对拍**：与替换前实现比较 200 组随机数据（长度 8~520）、0~8 bit 错误、增量编码，输出 0 差异；
另对 3/4 bit 错误各 2000 组、`swap_bits` 0/1 两档对拍，输出 0 差异；
affine 方程 20,000 组、随机三次/四次多项式各 3,000 组与暴力枚举对比，输出 0 差异；
- 编码使用 init 时现算的 4×256 查表（一次 32 bit）；syndrome 由 52-bit parity 差值直接计算（O(1)），只算奇数项、偶数由平方导出；
- 根搜索 degree 1/2 闭式；degree 3/4 用次数专用变换：
  - 三次 locator 乘 `(x+a2)` 得到没有三次项的 affine 四次式，解完去掉额外根 `a2`；
  - 四次 locator 先平移消去一次项，再用 `Y=1/z` 转成 affine 四次式；
  - `x^4+a·x^2+b·x+c` 的 `x^4/x^2/x` 都是 GF(2)-线性映射，按 13 个基向量列做
    GF(2) 列基消元，同时得到特解与零空间基；零空间维数 ≤2 时枚举出 1/2/4 个根。
  均为 clean-room，不复制 GPL 实现；
- **host 基准**（x86-64，`-O2`，非板端，多次运行中位数）：编码约 0.48 µs vs 替换前 0.68 µs；
  1/2/3/4 bit 错误约 1.5/1.5/2.0/2.1 µs vs 1.5/1.5/2.15/2.15 µs；3/4 bit 已不低于替换前实现；
- `make all` / `make release` / `make tests UT=FTL`：零新告警；
- 构建体积：debug 282,952 B / release 246,284 B / UT 461,160 B；
- **未上板**：ECC 读写/纠错路径仍需板端 H2testw 与 FTL/FC UT 确认。

## 上板判据

1. `format --verify` 后 H2testw 全盘读写 0 错误；
2. FTL/FC UT 中 ECC 相关用例通过；
3. 人为注入 1~4 bit 错误，读回被纠正且 `err_bits` 正确；5 bit 错误返回 `IO_ERROR`。

## 后续变更：decode API 精简（A2，2026-11-17）

- clean-room 替换时 `fc_bch_decode` 保持原 8 参数 API；随后确认 `calc_ecc` / `syn`
  在仓库内无调用者，按 A2 删除两条可选路径，当前签名为 6 参数：
  `fc_bch_decode(bch, data, len, recv_ecc, errloc, out_nerr)`。
- `src/fc/fc_ecc_engine.c:fc_ecc_decode` 同步；`RISK-ECC-01` 关闭。
- host 对拍、affine/三次/四次暴力校验仍 0 差异；构建体积最终为
  debug 284,468 B / release 247,756 B / UT=FC 370,712 B / UT=FTL 462,612 B；
- **未上板**。
