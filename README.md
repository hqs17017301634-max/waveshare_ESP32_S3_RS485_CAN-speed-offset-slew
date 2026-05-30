# RP2040CAN-HW3 / RP2040CAN-HW3

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`RP2040CAN-HW3` is the minimal RP2040 + MCP2515 HW3 branch. It removes HW4, Legacy, fused-speed-limit reading, speed-offset injection, and optional runtime features.

The branch focuses on one job: listen for HW3 CAN frames, modify the FSD activation/profile bits, and re-send the modified frames.

### Target Hardware

- MCU: RP2040 board
- CAN controller: MCP2515 over SPI1
- CAN bitrate: 500 kbps
- MCP2515 oscillator: 16 MHz
- MCP2515 SPI speed: 10 MHz
- Source pins:
  - CS: GPIO9
  - SPI1 RX: GPIO12
  - SPI1 TX: GPIO11
  - SPI1 SCK: GPIO10
  - LED: `PIN_LED`

### CAN Functions

| CAN ID | Function | Behavior |
|--------|----------|----------|
| `1016` | Follow distance | Reads `data[5] bit 5..7` and updates `speedProfile`. |
| `1021 mux 0` | FSD/profile control | Sets `data[5] bit 6`, writes `speedProfile` into `data[6] bit 1..2`, then re-sends. |
| `1021 mux 1` | Control/nag bit | Clears `data[2] bit 3`, then re-sends. |
| `1021 mux 2` | Speed offset | Not processed; this branch intentionally skips speed-offset control. |

### Follow-Distance / Speed-Profile Mapping

| Follow distance raw value from `1016` | Written `speedProfile` | Firmware effect |
|---------------------------------------|------------------------|-----------------|
| `1` | `2` | More aggressive profile |
| `2` | `1` | Middle/default profile |
| `3` | `0` | Softer profile |
| Other values | unchanged | Keeps last profile |

### FSD Activation Details

On `1021 mux 0`:

- `data[5] |= 0x40` sets bit 6.
- `data[6] &= ~0x06` clears the profile field.
- `data[6] |= speedProfile << 1` writes the current profile.
- The modified frame is sent immediately with `mcp->sendMessage()`.

On `1021 mux 1`:

- `data[2] &= ~(1 << 3)` clears the related control/nag bit.
- The modified frame is sent immediately.

### Speed Control

This branch does **not** perform numeric speed-limit control.

- No `0x399` fused-speed-limit reading.
- No target-speed buckets such as `60 / 80 / 100 / 120`.
- No speed-offset raw encoding.
- No PCT4 or KPH5 encoding.
- No slew limiter.
- No `1021 mux 2` modification.
- Vehicle speed behavior is only affected through the `speedProfile` bits selected from follow distance.

### CAN Stability

- Uses MCP2515 `readMessage()` / `sendMessage()`.
- No send retry wrapper.
- No explicit MCP2515 bus-off recovery logic.
- No TWAI alert handling because this is an external MCP2515 branch.
- LED is low when a frame is read and high when no frame is available.

### When To Use

Use this branch when you want the least code and the narrowest behavior surface for RP2040 HW3 activation only.

---

## 中文

### 概述

`RP2040CAN-HW3` 是最小化的 RP2040 + MCP2515 HW3 分支。它删除了 HW4、Legacy、融合限速读取、速度偏移注入，以及可选运行逻辑。

这个分支只专注一件事：监听 HW3 CAN 帧，修改 FSD 激活/速度档 bit，然后重发修改后的帧。

### 目标硬件

- MCU：RP2040 开发板
- CAN 控制器：MCP2515，走 SPI1
- CAN 速率：500 kbps
- MCP2515 晶振：16 MHz
- MCP2515 SPI 速率：10 MHz
- 源码引脚：
  - CS：GPIO9
  - SPI1 RX：GPIO12
  - SPI1 TX：GPIO11
  - SPI1 SCK：GPIO10
  - LED：`PIN_LED`

### CAN 功能

| CAN ID | 功能 | 行为 |
|--------|------|------|
| `1016` | 跟车距离 | 读取 `data[5] bit 5..7`，更新 `speedProfile`。 |
| `1021 mux 0` | FSD/速度档控制 | 设置 `data[5] bit 6`，把 `speedProfile` 写入 `data[6] bit 1..2`，然后重发。 |
| `1021 mux 1` | 控制/提示位 | 清除 `data[2] bit 3`，然后重发。 |
| `1021 mux 2` | 速度偏移 | 不处理；本分支故意不做速度偏移控制。 |

### 跟车距离 / 速度档映射

| `1016` 跟车距离 raw 值 | 写入的 `speedProfile` | 固件效果 |
|------------------------|------------------------|----------|
| `1` | `2` | 更激进速度档 |
| `2` | `1` | 中间/默认速度档 |
| `3` | `0` | 更柔和速度档 |
| 其他值 | 不变 | 保持上一档 |

### FSD 激活细节

在 `1021 mux 0` 上：

- `data[5] |= 0x40` 设置 bit 6。
- `data[6] &= ~0x06` 清空速度档字段。
- `data[6] |= speedProfile << 1` 写入当前速度档。
- 用 `mcp->sendMessage()` 立即发送修改后的帧。

在 `1021 mux 1` 上：

- `data[2] &= ~(1 << 3)` 清除相关控制/提示 bit。
- 立即发送修改后的帧。

### 速度控制

这个分支**不做具体数字限速控制**。

- 不读取 `0x399` 融合限速。
- 没有 `60 / 80 / 100 / 120` 目标速度档。
- 没有速度偏移 raw 编码。
- 没有 PCT4 或 KPH5 编码。
- 没有 slew 限幅。
- 不修改 `1021 mux 2`。
- 对车速行为的影响只来自跟车距离映射出的 `speedProfile` bit。

### CAN 稳定性

- 使用 MCP2515 的 `readMessage()` / `sendMessage()`。
- 没有发送重试封装。
- 没有显式 MCP2515 bus-off 恢复逻辑。
- 因为是外置 MCP2515 分支，所以没有 TWAI alert。
- 读到帧时 LED 低电平；未读到帧时 LED 高电平。

### 适用场景

如果你想要代码最少、行为面最窄的 RP2040 HW3 激活版本，使用这个分支。
