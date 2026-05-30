# ESP32-HW3 / ESP32-HW3

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`ESP32-HW3` is the ESP32-S3 TWAI port of the minimal HW3 activation sketch. It keeps the small HW3-only behavior of `RP2040CAN-HW3`, but replaces MCP2515/SPI with the ESP32-S3 built-in TWAI CAN controller and adds basic CAN stability handling.

### Target Hardware

- Board: Waveshare ESP32-S3-RS485-CAN
- Framework: Arduino via PlatformIO
- CAN controller: ESP32-S3 built-in TWAI
- CAN speed: 500 kbps
- Default pins:
  - TWAI TX: GPIO15
  - TWAI RX: GPIO16
- Queue sizes:
  - RX queue: 64
  - TX queue: 16

### Main Features

- HW3 only.
- No WiFi, Bluetooth, OTA, Web UI, or dashboard logic.
- Reads follow distance from CAN ID `1016` and maps it to `speedProfile`.
- Handles AP/FSD control frame CAN ID `1021`.
- For `1021 mux 0`:
  - Enables the FSD/speed-profile control bit.
  - Writes `speedProfile` into the control frame.
  - Re-transmits the modified frame.
- For `1021 mux 1`:
  - Clears the control/nag bit.
  - Re-transmits the modified frame.
- Does not handle `1021 mux 2` speed offset.

### CAN Stability Features

- TWAI hardware acceptance filtering for the required IDs.
- TWAI alert handling.
- Bus-off recovery.
- RX queue / FIFO overrun handling.
- DLC and standard-frame checks.

### Build & Flash

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

### When To Use

Use this branch when you want ESP32-S3 hardware with the simplest HW3 activation behavior and no speed-offset feature.

---

## 中文

### 概述

`ESP32-HW3` 是最小 HW3 激活草图的 ESP32-S3 TWAI 移植版。它保留了 `RP2040CAN-HW3` 的极简 HW3 行为，但把 MCP2515/SPI 换成 ESP32-S3 内置 TWAI CAN 控制器，并加入基础 CAN 稳定性处理。

### 目标硬件

- 开发板：Waveshare ESP32-S3-RS485-CAN
- 框架：PlatformIO 下的 Arduino
- CAN 控制器：ESP32-S3 内置 TWAI
- CAN 速率：500 kbps
- 默认引脚：
  - TWAI TX：GPIO15
  - TWAI RX：GPIO16
- 队列大小：
  - RX 队列：64
  - TX 队列：16

### 主要功能

- 仅支持 HW3。
- 没有 WiFi、蓝牙、OTA、Web UI 或 Dashboard 逻辑。
- 从 CAN ID `1016` 读取跟车距离，并映射为 `speedProfile`。
- 处理 AP/FSD 控制帧 CAN ID `1021`。
- 对 `1021 mux 0`：
  - 启用 FSD/速度档控制位。
  - 把 `speedProfile` 写入控制帧。
  - 重新发送修改后的帧。
- 对 `1021 mux 1`：
  - 清除控制/提示相关位。
  - 重新发送修改后的帧。
- 不处理 `1021 mux 2` 速度偏移。

### CAN 稳定性功能

- 对必要 ID 做 TWAI 硬件验收过滤。
- TWAI alert 处理。
- Bus-off 自动恢复。
- RX 队列 / FIFO 溢出处理。
- DLC 和标准帧检查。

### 编译与烧录

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

### 适用场景

如果你想使用 ESP32-S3 硬件，但只需要最简单的 HW3 激活功能，不需要速度偏移，使用这个分支。
