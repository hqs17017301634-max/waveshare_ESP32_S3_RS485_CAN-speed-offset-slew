# RP2040CAN-FSD / RP2040CAN-FSD

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`RP2040CAN-FSD` is the original RP2040 + MCP2515 CAN sketch. It keeps the wider FSD code path and supports multiple vehicle handler variants through compile-time selection.

### Target Hardware

- MCU: RP2040 board
- CAN controller: MCP2515 over SPI
- CAN speed: 500 kbps
- Default CAN CS: board macro `PIN_CAN_CS`
- Other CAN pins: `PIN_CAN_INTERRUPT`, `PIN_CAN_STANDBY`, `PIN_CAN_RESET`

### Main Features

- Supports HW3, HW4, and Legacy handlers in the source.
- Default compile target is `HW3`.
- Reads follow-distance frames and maps them to `speedProfile`.
- Handles FSD-related control frames.
- Reads fused speed limit from `0x399`.
- Computes a target speed bucket and writes a speed offset into the control frame.
- Uses MCP2515 `sendMessage()` / `readMessage()` for CAN traffic.

### Current Behavior

- HW3 follow distance source: CAN ID `1016`.
- HW3 FSD control source: CAN ID `1021`.
- Fused speed limit source: CAN ID `0x399`.
- Speed offset encoding: legacy absolute offset style, `offset * 10`.
- Max computed offset clamp: `25`.

### Notes

This branch is useful as the original RP2040 full-feature reference. It is not the most optimized branch: it still contains multi-handler code, older runtime switches, and no TWAI-specific stability features because it targets MCP2515.

---

## 中文

### 概述

`RP2040CAN-FSD` 是原始的 RP2040 + MCP2515 CAN 草图。它保留了较完整的 FSD 代码路径，并通过编译期开关支持多个车型处理器。

### 目标硬件

- MCU：RP2040 开发板
- CAN 控制器：MCP2515，走 SPI
- CAN 速率：500 kbps
- 默认 CAN CS：板级宏 `PIN_CAN_CS`
- 其他 CAN 引脚：`PIN_CAN_INTERRUPT`、`PIN_CAN_STANDBY`、`PIN_CAN_RESET`

### 主要功能

- 源码内包含 HW3、HW4、Legacy 处理逻辑。
- 默认编译目标是 `HW3`。
- 读取跟车距离帧，并映射为 `speedProfile`。
- 处理 FSD 相关控制帧。
- 从 `0x399` 读取融合限速。
- 根据限速计算目标速度档，并把速度偏移写入控制帧。
- 使用 MCP2515 的 `sendMessage()` / `readMessage()` 进行 CAN 收发。

### 当前行为

- HW3 跟车距离来源：CAN ID `1016`。
- HW3 FSD 控制来源：CAN ID `1021`。
- 融合限速来源：CAN ID `0x399`。
- 速度偏移编码：旧版绝对偏移方式，`offset * 10`。
- 计算偏移最大夹紧：`25`。

### 说明

这个分支适合作为 RP2040 全功能原始参考。它不是最精简版本：仍保留多车型处理逻辑、旧运行期开关，并且因为目标是 MCP2515，所以没有 ESP32 TWAI 的稳定性恢复功能。
