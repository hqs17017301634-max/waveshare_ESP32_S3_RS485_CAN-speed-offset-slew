# ESP32-HW3 / ESP32-HW3

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`ESP32-HW3` is the ESP32-S3 TWAI port of the minimal HW3 activation firmware. It keeps the narrow behavior of `RP2040CAN-HW3`, but replaces the external MCP2515/SPI path with the ESP32-S3 built-in TWAI CAN controller.

This is the simplest ESP32 branch: HW3 activation/profile control only, no speed-limit offset feature.

### Project Architecture

This branch is a single-bus Arduino / PlatformIO ESP32-S3 firmware. The main
entry point is `ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`; no WebUI, Bluetooth, OTA,
dashboard, recorder, or secondary CAN runtime is started.

| Layer | Component | Role |
|-------|-----------|------|
| CAN A / CAN1 | ESP32-S3 built-in TWAI | The only CAN interface. It receives HW3 FSD/AP-control frames, modifies the activation/profile fields, and transmits the edited frame. |
| Main loop | Arduino `loop()` | Services TWAI alerts, receives bounded CAN frames, validates ID/DLC, and runs the minimal HW3 handler. |
| Runtime logic | Minimal HW3 frame handler | Keeps follow-distance/profile state and applies only the required HW3 activation/profile bit edits. |
| Configuration | Compile-time constants | No WebUI or persistent runtime config; behavior is fixed by this branch's source and PlatformIO build flags. |

### Target Hardware

- Board: Waveshare ESP32-S3-RS485-CAN
- Framework: Arduino via PlatformIO
- CAN controller: ESP32-S3 built-in TWAI
- CAN bitrate: 500 kbps
- Default TWAI TX: GPIO15
- Default TWAI RX: GPIO16
- RX queue length: 64
- TX queue length: 16

### CAN Functions

| CAN ID | Function | Behavior |
|--------|----------|----------|
| `1016` | Follow distance | Reads `data[5] bit 5..7` and updates `speedProfile`. |
| `1021 mux 0` | FSD/profile control | Sets `data[5] bit 6`, writes `speedProfile` into `data[6] bit 1..2`, then transmits. |
| `1021 mux 1` | Control/nag bit | Clears `data[2] bit 3`, then transmits. |
| `1021 mux 2` | Speed offset | Not processed. |

### Follow-Distance / Speed-Profile Mapping

| Follow distance raw value from `1016` | Written `speedProfile` | Firmware effect |
|---------------------------------------|------------------------|-----------------|
| `1` | `2` | More aggressive profile |
| `2` | `1` | Middle/default profile |
| `3` | `0` | Softer profile |
| Other values | unchanged | Keeps last profile |

### FSD Activation Details

On `1021 mux 0`:

- `data[5] bit 6` is set.
- `data[6] bit 1..2` is replaced with the current `speedProfile`.
- The modified frame is sent through TWAI.

On `1021 mux 1`:

- `data[2] bit 3` is cleared.
- The modified frame is sent through TWAI.

### Speed Control

This branch intentionally has no numeric speed-offset feature.

- No `0x399` fused-speed-limit reading.
- No target speed table.
- No PCT4 or KPH5 encoding.
- No slew limiter.
- No `1021 mux 2` speed-offset injection.
- The only speed-related behavior is the HW3 `speedProfile` field selected by follow distance.

### CAN Stability Features

- Uses ESP32-S3 built-in TWAI at 500 kbps.
- Hardware acceptance filter covers the required IDs `1016` and `1021`.
- Rejects extended frames, remote frames, and frames shorter than 7 bytes before parsing.
- TWAI alerts enabled:
  - bus-off
  - bus recovered
  - RX queue full
  - RX FIFO overrun
  - TX failed
- Bus-off recovery is initiated automatically.
- TWAI is restarted after bus recovery.
- RX queue is cleared on queue/FIFO overrun.
- TX wait timeout: `1 ms`.
- RX wait timeout: `10 ms`.

### What Is Not Included

- No WiFi, Bluetooth, OTA, Web UI, or dashboard runtime.
- No speed-offset control.
- No short TX retry loop in this branch.
- No fused-limit monitor.

### Build & Flash

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

### When To Use

Use this branch when you want ESP32-S3 hardware, built-in TWAI CAN, and the simplest HW3 activation behavior without speed offset.

---

## 中文

### 概述

`ESP32-HW3` 是最小 HW3 激活固件的 ESP32-S3 TWAI 移植版。它保留 `RP2040CAN-HW3` 的窄行为范围，但把外置 MCP2515/SPI 换成 ESP32-S3 内置 TWAI CAN 控制器。

这是最简单的 ESP32 分支：只做 HW3 激活/速度档控制，不做限速偏移。

### 项目架构

本分支是单 CAN 总线的 Arduino / PlatformIO ESP32-S3 固件。主入口是
`ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`；不会启动 WebUI、蓝牙、OTA、Dashboard、
抓包器或第二路 CAN。

| 层级 | 组件 | 作用 |
|------|------|------|
| CAN A / CAN1 | ESP32-S3 原生 TWAI | 唯一 CAN 接口。接收 HW3 FSD/AP 控制帧，修改激活/速度档字段，再发送修改后的帧。 |
| 主循环 | Arduino `loop()` | 处理 TWAI alert，按预算接收 CAN 帧，校验 ID/DLC，并运行最小 HW3 处理器。 |
| 运行逻辑 | 最小 HW3 帧处理器 | 保存跟车距离/速度档状态，只应用 HW3 激活/速度档所需 bit 修改。 |
| 配置 | 编译期常量 | 没有 WebUI 或持久化运行配置；行为由本分支源码和 PlatformIO build flags 固定。 |

### 目标硬件

- 开发板：Waveshare ESP32-S3-RS485-CAN
- 框架：PlatformIO 下的 Arduino
- CAN 控制器：ESP32-S3 内置 TWAI
- CAN 速率：500 kbps
- 默认 TWAI TX：GPIO15
- 默认 TWAI RX：GPIO16
- RX 队列长度：64
- TX 队列长度：16

### CAN 功能

| CAN ID | 功能 | 行为 |
|--------|------|------|
| `1016` | 跟车距离 | 读取 `data[5] bit 5..7`，更新 `speedProfile`。 |
| `1021 mux 0` | FSD/速度档控制 | 设置 `data[5] bit 6`，把 `speedProfile` 写入 `data[6] bit 1..2`，然后发送。 |
| `1021 mux 1` | 控制/提示位 | 清除 `data[2] bit 3`，然后发送。 |
| `1021 mux 2` | 速度偏移 | 不处理。 |

### 跟车距离 / 速度档映射

| `1016` 跟车距离 raw 值 | 写入的 `speedProfile` | 固件效果 |
|------------------------|------------------------|----------|
| `1` | `2` | 更激进速度档 |
| `2` | `1` | 中间/默认速度档 |
| `3` | `0` | 更柔和速度档 |
| 其他值 | 不变 | 保持上一档 |

### FSD 激活细节

在 `1021 mux 0` 上：

- 设置 `data[5] bit 6`。
- 用当前 `speedProfile` 替换 `data[6] bit 1..2`。
- 通过 TWAI 发送修改后的帧。

在 `1021 mux 1` 上：

- 清除 `data[2] bit 3`。
- 通过 TWAI 发送修改后的帧。

### 速度控制

这个分支故意不包含具体数字速度偏移功能。

- 不读取 `0x399` 融合限速。
- 没有目标速度表。
- 没有 PCT4 或 KPH5 编码。
- 没有 slew 限幅。
- 不向 `1021 mux 2` 注入速度偏移。
- 唯一与速度相关的行为是跟车距离选择的 HW3 `speedProfile` 字段。

### CAN 稳定性功能

- 使用 ESP32-S3 内置 TWAI，500 kbps。
- 硬件验收过滤覆盖需要的 ID：`1016` 和 `1021`。
- 解析前拒绝扩展帧、远程帧和短于 7 字节的帧。
- 启用的 TWAI alerts：
  - bus-off
  - bus recovered
  - RX queue full
  - RX FIFO overrun
  - TX failed
- bus-off 后自动发起恢复。
- bus 恢复后自动重新启动 TWAI。
- RX 队列/FIFO 溢出时清空接收队列。
- TX 等待超时：`1 ms`。
- RX 等待超时：`10 ms`。

### 不包含的内容

- 没有 WiFi、蓝牙、OTA、Web UI 或 Dashboard 运行逻辑。
- 没有速度偏移控制。
- 本分支没有短 TX 重试循环。
- 没有融合限速监控。

### 编译与烧录

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

### 适用场景

如果你想使用 ESP32-S3 硬件、内置 TWAI CAN，并且只需要最简单的 HW3 激活功能，不需要速度偏移，使用这个分支。
