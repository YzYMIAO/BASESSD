# 第三方组件声明

本项目自身代码采用 Apache-2.0（见根目录 `LICENSE`）。
以下组件由第三方提供，**不适用**根目录许可证，各自遵循其上游条款；本文件只做归属声明，
不改写任何上游许可文本，也不给 vendor 文件更换版权头。

## 1. TinyUSB 0.21.0（MIT）

- 位置：`third_party/tinyusb/`
- 许可证：MIT，版权 `Copyright (c) 2012-2026, Ha Thach (tinyusb.org)`
- 许可原文：`third_party/tinyusb/LICENSE`（随组件保留，逐字未改）

## 2. STM32H743 HAL / CMSIS Device（STMicroelectronics）

- 位置：`third_party/stm32h743/hal/`、`third_party/stm32h743/cmsis-device/`、
  `third_party/stm32h743/startup_stm32h743xx.s`、`third_party/stm32h743/system_stm32h7xx.c`
- 上游文件头声明（`third_party/stm32h743/hal/inc/stm32h7xx_hal.h:10-15`）：

  > Copyright (c) 2017 STMicroelectronics. All rights reserved.
  > This software is licensed under terms that can be found in the LICENSE file
  > in the root directory of this software component.
  > If no LICENSE file comes with this software, it is provided AS-IS.

- **当前事实**：本仓库**没有**随附该 LICENSE 文件，因此按上述声明以 AS-IS 提供。
- **待补**：从取得这份源码的 STM32Cube 包中把许可证原文放到
  `third_party/stm32h743/LICENSE`。补上之前，对外发布说明必须写明“ST 组件许可原文待补”。
  该缺口已登记为 `docs/design/known_risks.md` 的 `RISK-REL-02`。

## 3. CMSIS Core（Apache-2.0）

- 位置：`third_party/stm32h743/cmsis-core/`
- 许可证：Apache-2.0，文件头带 SPDX 标识（如 `cmsis_version.h:10`
  `SPDX-License-Identifier: Apache-2.0`）
- 处理：保留上游 SPDX 头，不修改文件。

## 4. μnit 0.4.1（MIT）

- 位置：`tests/framework/munit.c`、`tests/framework/munit.h`
- 说明：上游 μnit Testing Framework 已针对裸机目标（STM32H743）改造。
- 许可证：MIT，版权 `Copyright (c) 2013-2016 Evan Nemerson <evan@nemerson.com>`
- 许可原文：`tests/framework/COPYING`

## 5. 固件二进制

编译产物包含上述组件，分发 `.elf` / `.bin` 时需同时满足各组件条款；
根目录 Apache-2.0 只覆盖本项目自身代码。
