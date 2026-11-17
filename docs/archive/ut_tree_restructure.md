# UT 测试树重构评估（2026-11-14）

> 状态：**P0~P4 已实施（2026-11-14）**。对象：`ut_main.c` 的套件树、`tests/unit/*` 文件布局、
> `makefile`/模块 `makefile.mk` 的 UT 源集合。产品源码不在范围内。

## 1. 重构前现状与问题（历史记录）

重构前套件树：

```text
/fc-nand/
  pure/         test_fc_pure.c   11 例  纯 RAM（队列/命令/ONFI 解码）
  data_path/    test_fc_data_path.c   15 例  buffer 级编解码，不碰 NAND
  lifecycle/    test_fc_lifecycle.c   19 例  真 NAND 全链路
  negative/     test_fc_negative.c 11 例  FC 边界负例
/ftl/
  pure/{mem,md,host,io}/   test_ftl_pure.c   57 例  RAM
  media/e2e/               test_ftl_media.c  24 例  真 NAND 正例
  negative/pure/           test_negative_pure.c  41 例  RAM/纯函数
  negative/media/          test_negative_media.c 49 例  真 NAND 注入
/hil/                        test_hil.c       16 例  controller + 真 I/O
/bench/speed/                      test_speed.c      2 例  板端测速
```

主要问题：

1. **FTL 负例混层**：`/ftl/media/negative/` 下有 12 条左右纯 RAM/夹具用例
   （allocator / WL / RR / pending / request 表 / 空位图建链），不需要 NAND，
   却和真 NAND 注入用例同套件。纯用例被迫走 `neg_setup`（topology + 首次 format），
   跑单条也要 format。
2. **命名是历史层号**：FC 的 `test_fc_level1/2/3.c` 与文档里的「Level 1/2/3」是
   历史分层；套件路径已是 `logic/data_path/lifecycle`，文件/数组/文档三套叫法并存。
   与 FTL 的 `pure` 术语不一致（FC 叫 logic，FTL 叫 pure）。
3. **增长后套件过大**：当前约 209 例；K 类评估里 D1/D2/D3 还有 83 条可做用例。
   若全部落地，FTL 负例会到 130+ 条，平铺在一个套件里无法按模块定向跑。
4. **HIL 混契约与 I/O**：16 例里 controller 契约（状态机/init/submit 边界/queue）
   与真 READ/WRITE/FLUSH/TRIM 混在一起；后续加 USB/跨重启会继续膨胀。
5. **speed 在根层**：测速不是断言型用例，和 `fc-nand/ftl/hil` 并列时语义不清。

## 2. 建议目标树

```text
/fc-nand/
  pure/          # 原 logic/：纯 RAM
  data_path/     # buffer 级编解码
  lifecycle/     # 真 NAND 全链路
  negative/      # FC 边界负例
/ftl/
  pure/{mem,md,host,io}/    # 不变
  media/e2e/                # 真 NAND 正例
  negative/
    pure/                   # RAM / 分配器 / WL / RR / 表守卫
    media/
      gc/                   # GC / migrate / WL
      l2p/                  # L2P cache / 加载 / 降级
      meta/                 # metadata / CP / Journal / BBT
      boot/                 # 启动 / 恢复 / 掉电模拟
      if/                   # FTL↔FC 接口（需要 live FTL）
/hil/
  contract/                 # 状态机 / init / submit / queue / is_complete
  io/                       # READ / WRITE / FLUSH / TRIM
  usb/                      # 预留：USB/MSC 端到端
/bench/speed/               # 原 /speed/
```

原则：**套件路径 = 抽象层 + 依赖的真实资源**。`pure` 不碰 NAND；`data_path` 碰
buffer 不碰 NAND；`media` 碰真 NAND；`lifecycle` 是全链路；`hil` 是 controller 级集成。

## 3. 重命名映射（P0~P4 已落地）

| 旧 | 新 | 说明 |
|---|---|---|
| `test_fc_level1.c` | `test_fc_pure.c` | 文件改名，套件 `/fc-nand/pure/` |
| `test_fc_level2.c` | `test_fc_data_path.c` | 文件改名，套件 `/fc-nand/data_path/` |
| `test_fc_level3.c` | `test_fc_lifecycle.c` | 文件改名，套件 `/fc-nand/lifecycle/` |
| `g_fc_logic_tests` | `g_fc_pure_tests` | 与 FTL `pure` 术语统一 |
| `/fc-nand/logic/` | `/fc-nand/pure/` | 套件路径 |
| `test_negative.c`（media 部分） | `test_negative_media.c` | 真 NAND 注入，38 条 |
| `test_negative.c`（pure 部分） | `test_negative_pure.c` | RAM / 表 / 分配器，16 条，不 format |
| `g_negative_tests` | `g_negative_pure_tests` + 5 个 media 子数组 | pure 1 个；media 按 gc/l2p/meta/boot/if 分 5 个 |
| `/ftl/media/negative/` | `/ftl/negative/media/{gc,l2p,meta,boot,if}/` + `/ftl/negative/pure/` | 负例独立于正例 e2e |
| `/hil/` | `/hil/contract/` + `/hil/io/` | 同文件两数组（9 + 7） |
| `/speed/` | `/bench/speed/` | 套件路径改名 |

P1 未新建 `negative_common.c/h`：拆分后 pure 侧不需要任何介质 helper，
media 侧 helper 全部留在 `test_negative_media.c`，共享面为空，不引入空壳文件。

## 4. 迁移顺序与成本

| 阶段 | 内容 | 风险 | 板端 |
|---|---|---|---|
| P0 | FC level1/2/3 文件与套件改名；`makefile.mk` / `ut_main.c` / 文档同步 | 低（纯改名，语义不变） | 一次冒烟即可 |
| P1 | FTL 负例拆 pure/media；纯用例换轻量 setup（不 format）；不新建 common | 中（用例注册/共享 helper 搬迁） | 建议跑一遍 54 条 |
| P2 | media 负例按 gc/l2p/meta/boot/if 拆子套件（同文件五数组，不动用例体） | 中（只动注册） | 同 P1 |
| P3 | HIL 拆 `contract/` `io/`（同文件两数组，不搬文件） | 低 | HIL 16 条跑一遍 |
| P4 | `/speed/` → `/bench/speed/` | 低 | 否 |

## 5. 注意事项

- **上板状态**：P0~P4 仅完成代码与构建（`make all` / `make release` /
  `make tests UT=FTL` / `UT=FC` / `UT=HIL`，零新告警），**未上板**；重构后必须跑一次
  `/fc-nand/` + `/ftl/negative/` + `/hil/` + `/bench/speed/` 冒烟；
  HIL 拆分后也**未上板**。

- 套件前缀**必须以 `/` 结尾**（munit 只做前缀拼接，见 `ut_main.c` 注释）；
- 移动文件后 `src/*/makefile.mk` 的 `TEST_C_SRCS` 必须同步；对象目录按 UT 分置，无需 clean；
- 修改套件路径会让已保存的 `test ...` 命令失效，文档/脚本一次回扫（P12）；
- 测试源由用户维护（R4）：本轮经用户明确授权后直接改测试源；
- 用例体与断言不因重构变化；重构后按 UT 范围构建零新告警，板端冒烟一次。
- 全量组合 `make tests`（FC+FTL+HIL）当前链接溢出，见 RISK-UT-38。

## 6. 结论

P0~P4 已按用户指示一次性落地：FC 去 Level 号、FTL 负例拆 pure/media 并按模块分
5 个子套件、HIL 拆 contract/io、speed 改 `/bench/speed/`。用例体与断言不变；
后续 D1/D2/D3 用例直接挂到对应子套件，不再集中堆在单一 media 套件下。
