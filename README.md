# RP2040CAN-FSD / RP2040CAN-FSD

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`RP2040CAN-FSD` is the original RP2040 + MCP2515 full/reference branch. It keeps the older multi-handler structure and can compile HW3, HW4, or Legacy behavior by changing the compile-time macro.

This branch is useful as a reference implementation because it preserves the wider feature set, but it is not the smallest or most stable branch.

### Project Architecture

This branch is an Arduino-style RP2040 firmware that uses an external MCP2515
CAN controller over SPI. It is the reference branch for the older multi-handler
design rather than the newer ESP32-S3/TWAI fast-path architecture.

| Layer | Component | Role |
|-------|-----------|------|
| CAN bus | MCP2515 over SPI | Single CAN interface at 500 kbps. Receives target frames, applies the selected handler, and transmits edited frames. |
| MCU runtime | RP2040 Arduino sketch | Owns setup/loop, board pin macros, MCP2515 init, interrupt/polling path, and LED/status handling. |
| Handler layer | HW3 / HW4 / Legacy handlers | Compile-time-selected logic for the desired vehicle hardware generation and behavior. |
| Configuration | Compile-time macros | Hardware pins and handler selection are decided at build time; there is no WebUI or persistent runtime config. |

### Target Hardware

- MCU: RP2040 board
- CAN controller: MCP2515 over SPI
- CAN bitrate: 500 kbps
- MCP2515 oscillator: 16 MHz
- Default CAN CS: board macro `PIN_CAN_CS`
- Other board macros used by the sketch: `PIN_CAN_INTERRUPT`, `PIN_CAN_STANDBY`, `PIN_CAN_RESET`, `PIN_LED`

### Build Target

- Default selected target in source: `HW3`
- Alternative source handlers still present: `HW4`, `LEGACY`
- Handler selection is compile-time only through `#define HW3`, `#define HW4`, or `#define LEGACY`.

### HW3 CAN Functions

| CAN ID | Function | Behavior |
|--------|----------|----------|
| `1016` | Follow distance | Reads `data[5] bit 5..7` and updates `speedProfile`. |
| `1021 mux 0` | FSD/profile control | Sets bit `46`, writes `speedProfile` into `data[6] bit 1..2`, then re-sends. |
| `1021 mux 1` | Control/nag bit | Clears bit `19`, then re-sends. |
| `1021 mux 2` | Speed offset | Writes a computed speed-offset raw value. |
| `0x399` | Fused speed limit | Reads `data[1] & 0x1F`; `0` and `31` are invalid; valid raw value is multiplied by `5`. |

### HW3 Follow-Distance Mapping

| Follow distance raw value | Written `speedProfile` | Firmware effect |
|---------------------------|------------------------|-----------------|
| `1` | `2` | More aggressive profile |
| `2` | `1` | Middle/default profile |
| `3` | `0` | Softer profile |
| Other values | unchanged | Keeps last profile |

### HW3 Speed Offset Logic

The source variable names call the value `Mph`, but the fused-speed-limit source is `raw * 5`, matching the usual fused-limit step size. Treat the values below as firmware units from `0x399`; this branch labels them as mph in code.

Default target-speed buckets:

| Fused limit value | Target value | Computed absolute offset before clamp |
|-------------------|--------------|---------------------------------------|
| `< 60` | `60` | `60 - limit` |
| `60..79` | `80` | `80 - limit` |
| `80..99` | `100` | `100 - limit` |
| `100..119` | `120` | `120 - limit` |
| `>= 120` | same as limit | `0` |

Offset rules:

- Computed offset is clamped to `0..25`.
- Wire raw value is `offset * 10`.
- Raw value is clamped to `0..255`.
- Example: limit `50` -> target `60` -> offset `10` -> raw `100`.
- Example: limit `60` -> target `80` -> offset `20` -> raw `200`.
- Example: limit `90` -> target `100` -> offset `10` -> raw `100`.
- If there is no valid fused limit, the branch falls back to the stock offset extracted from `1021 mux 0`.
- Optional fixed raw test exists in source: `FIXED_OFFSET_TEST_RAW = 40`, disabled by default.

### HW4 / Legacy Notes

- `HW4` code path still exists and includes extra HW4-style bit changes, speed profile writing in `1021 mux 2`, and optional approaching-emergency-vehicle behavior.
- `LEGACY` code path still exists and uses older CAN IDs (`69`, `1006`).
- These extra handlers increase code size and behavior surface compared with the HW3-only branches.

### CAN Stability

- CAN runs through MCP2515 `readMessage()` / `sendMessage()`.
- No TWAI alert handling, bus-off recovery, or ESP32 hardware filtering exists because this is not an ESP32/TWAI branch.
- No short TX retry wrapper is implemented around `sendMessage()`.
- This branch is best treated as the full RP2040 reference, not the most optimized runtime.

### When To Use

Use this branch when you want the original RP2040 full-feature reference with HW3 speed offset plus preserved HW4/Legacy code paths.

---

## 中文

### 概述

`RP2040CAN-FSD` 是原始 RP2040 + MCP2515 全功能/参考分支。它保留较老的多处理器结构，可以通过编译期宏选择 HW3、HW4 或 Legacy 行为。

这个分支适合作为参考实现，因为功能保留得比较完整；但它不是代码最少、稳定性保护最多的分支。

### 项目架构

本分支是 Arduino 风格的 RP2040 固件，使用外置 MCP2515 通过 SPI 接入 CAN。
它是较早期多 handler 设计的参考分支，不是后续 ESP32-S3/TWAI 快路径架构。

| 层级 | 组件 | 作用 |
|------|------|------|
| CAN 总线 | MCP2515 SPI | 单 CAN 接口，500 kbps。接收目标帧，交给选定 handler 修改，再发送编辑后的帧。 |
| MCU 运行时 | RP2040 Arduino sketch | 负责 setup/loop、板级引脚宏、MCP2515 初始化、中断/轮询路径和 LED/状态处理。 |
| 处理层 | HW3 / HW4 / Legacy handlers | 编译期选择对应车辆硬件代际和功能行为。 |
| 配置 | 编译期宏 | 硬件引脚和 handler 选择在构建时决定；没有 WebUI 或持久化运行配置。 |

### 目标硬件

- MCU：RP2040 开发板
- CAN 控制器：MCP2515，走 SPI
- CAN 速率：500 kbps
- MCP2515 晶振：16 MHz
- 默认 CAN CS：板级宏 `PIN_CAN_CS`
- 草图还使用这些板级宏：`PIN_CAN_INTERRUPT`、`PIN_CAN_STANDBY`、`PIN_CAN_RESET`、`PIN_LED`

### 编译目标

- 源码默认目标：`HW3`
- 源码内仍保留：`HW4`、`LEGACY`
- 只能通过编译期宏 `#define HW3`、`#define HW4` 或 `#define LEGACY` 切换。

### HW3 CAN 功能

| CAN ID | 功能 | 行为 |
|--------|------|------|
| `1016` | 跟车距离 | 读取 `data[5] bit 5..7`，更新 `speedProfile`。 |
| `1021 mux 0` | FSD/速度档控制 | 设置 bit `46`，把 `speedProfile` 写入 `data[6] bit 1..2`，然后重发。 |
| `1021 mux 1` | 控制/提示位 | 清除 bit `19`，然后重发。 |
| `1021 mux 2` | 速度偏移 | 写入计算后的速度偏移 raw 值。 |
| `0x399` | 融合限速 | 读取 `data[1] & 0x1F`；`0` 和 `31` 视为无效；有效 raw 乘以 `5`。 |

### HW3 跟车距离映射

| 跟车距离 raw 值 | 写入的 `speedProfile` | 固件效果 |
|-----------------|------------------------|----------|
| `1` | `2` | 更激进速度档 |
| `2` | `1` | 中间/默认速度档 |
| `3` | `0` | 更柔和速度档 |
| 其他值 | 不变 | 保持上一档 |

### HW3 限速偏移逻辑

源码变量名写的是 `Mph`，但融合限速来源是 `raw * 5`，与常见融合限速步进一致。下面的数值可理解为 `0x399` 的固件单位；本分支原代码把它命名为 mph。

默认目标速度档：

| 融合限速值 | 目标值 | 夹紧前绝对偏移 |
|------------|--------|----------------|
| `< 60` | `60` | `60 - 限速` |
| `60..79` | `80` | `80 - 限速` |
| `80..99` | `100` | `100 - 限速` |
| `100..119` | `120` | `120 - 限速` |
| `>= 120` | 等于限速 | `0` |

偏移规则：

- 计算偏移夹紧到 `0..25`。
- 线上 raw 值为 `offset * 10`。
- raw 值再夹紧到 `0..255`。
- 例：限速 `50` -> 目标 `60` -> 偏移 `10` -> raw `100`。
- 例：限速 `60` -> 目标 `80` -> 偏移 `20` -> raw `200`。
- 例：限速 `90` -> 目标 `100` -> 偏移 `10` -> raw `100`。
- 无有效融合限速时，回退使用 `1021 mux 0` 中解析出的原车 offset。
- 源码内有固定 raw 测试值：`FIXED_OFFSET_TEST_RAW = 40`，默认关闭。

### HW4 / Legacy 说明

- `HW4` 路径仍在，包含 HW4 风格 bit 修改、`1021 mux 2` 速度档写入，以及可选紧急车辆接近相关行为。
- `LEGACY` 路径仍在，使用较老 CAN ID（`69`、`1006`）。
- 这些额外路径让代码量和行为面都比 HW3-only 分支更大。

### CAN 稳定性

- CAN 通过 MCP2515 的 `readMessage()` / `sendMessage()` 收发。
- 因为不是 ESP32/TWAI 分支，所以没有 TWAI alert、bus-off 恢复、ESP32 硬件过滤。
- `sendMessage()` 外层没有短重试逻辑。
- 这个分支更适合作为 RP2040 全功能参考，不是最优化运行版本。

### 适用场景

如果你需要原始 RP2040 全功能参考，包含 HW3 速度偏移，并保留 HW4/Legacy 代码路径，使用这个分支。
