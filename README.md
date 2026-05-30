# RP2040CAN-HW3 / RP2040CAN-HW3

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`RP2040CAN-HW3` is the minimal RP2040 + MCP2515 HW3 branch. It removes HW4, Legacy, speed-limit offset, and extra configuration logic, keeping only the basic HW3 FSD activation path.

### Target Hardware

- MCU: RP2040 board
- CAN controller: MCP2515 over SPI1
- CAN speed: 500 kbps
- MCP2515 oscillator: 16 MHz
- MCP2515 SPI speed: 10 MHz
- Default pins in the sketch:
  - CS: GPIO9
  - SPI1 RX: GPIO12
  - SPI1 TX: GPIO11
  - SPI1 SCK: GPIO10

### Main Features

- HW3 only.
- Reads follow distance from CAN ID `1016` and maps it to `speedProfile`.
- Handles AP/FSD control frame CAN ID `1021`.
- For `1021 mux 0`:
  - Sets `data[5] bit 6`.
  - Writes the selected speed profile into `data[6] bit 1..2`.
  - Re-transmits the modified frame.
- For `1021 mux 1`:
  - Clears `data[2] bit 3`.
  - Re-transmits the modified frame.
- Does not process `1021 mux 2` speed offset.

### What This Branch Does Not Include

- No speed-limit reading from `0x399`.
- No speed offset injection.
- No PCT4 encoding.
- No slew limiter.
- No multi-vehicle HW4/Legacy code.
- No send retry or MCP2515 error recovery logic.

### When To Use

Use this branch when you want the simplest RP2040-based HW3 activation sketch with the least code and the smallest behavior surface.

---

## 中文

### 概述

`RP2040CAN-HW3` 是最小化的 RP2040 + MCP2515 HW3 分支。它删除了 HW4、Legacy、限速偏移和多余配置逻辑，只保留基础 HW3 FSD 激活路径。

### 目标硬件

- MCU：RP2040 开发板
- CAN 控制器：MCP2515，走 SPI1
- CAN 速率：500 kbps
- MCP2515 晶振：16 MHz
- MCP2515 SPI 速率：10 MHz
- 草图内默认引脚：
  - CS：GPIO9
  - SPI1 RX：GPIO12
  - SPI1 TX：GPIO11
  - SPI1 SCK：GPIO10

### 主要功能

- 仅支持 HW3。
- 从 CAN ID `1016` 读取跟车距离，并映射为 `speedProfile`。
- 处理 AP/FSD 控制帧 CAN ID `1021`。
- 对 `1021 mux 0`：
  - 设置 `data[5] bit 6`。
  - 把当前速度档写入 `data[6] bit 1..2`。
  - 重新发送修改后的帧。
- 对 `1021 mux 1`：
  - 清除 `data[2] bit 3`。
  - 重新发送修改后的帧。
- 不处理 `1021 mux 2` 速度偏移。

### 本分支不包含

- 不读取 `0x399` 限速。
- 不注入速度偏移。
- 不使用 PCT4 编码。
- 没有 slew 限幅。
- 没有 HW4/Legacy 多车型代码。
- 没有发送重试或 MCP2515 错误恢复逻辑。

### 适用场景

如果你只需要最简单的 RP2040 HW3 激活草图，追求代码最少、行为最单纯，使用这个分支。
