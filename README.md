# ESP32-FSD / ESP32-FSD

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`ESP32-FSD` is the enhanced ESP32-S3 TWAI branch for Waveshare ESP32-S3-RS485-CAN. It keeps a CAN-only HW3 firmware and adds speed-limit-based speed offset control, PCT4 encoding, slew limiting, and stronger TWAI reliability handling.

### Target Hardware

- Board: Waveshare ESP32-S3-RS485-CAN
- Framework: Arduino via PlatformIO
- CAN controller: ESP32-S3 built-in TWAI
- CAN speed: 500 kbps
- Flash target: 16 MB
- Partition table: `huge_app.csv`
- Default pins:
  - TWAI TX: GPIO15
  - TWAI RX: GPIO16
  - LED: GPIO14
- Queue sizes:
  - RX queue: 64
  - TX queue: 16

### Main Features

- HW3 only.
- CAN-only firmware: no WiFi, Bluetooth, OTA, Web UI, or dashboard runtime.
- Reads follow distance from CAN ID `1016` and maps it to `speedProfile`.
- Handles AP/FSD control frame CAN ID `1021`.
- Reads fused speed limit from CAN ID `0x399`.
- Injects speed offset into `1021 mux 2`.
- Uses PCT4 speed-offset encoding: `raw = percent * 4`.
- Downward slew limiting: default `15%/s`.
- Uses original/stock speed-offset raw value when no valid fused speed limit is available.

### FSD / CAN Behavior

- `1016`: follow distance -> `speedProfile`.
- `1021 mux 0`: sets the activation bit and writes `speedProfile`.
- `1021 mux 1`: clears the related control/nag bit.
- `1021 mux 2`: writes the PCT4 speed offset after slew limiting.
- `0x399`: reads fused speed limit from `data[1] & 0x1F`, valid values are multiplied by 5 kph.

### Speed Offset Logic

Default target-speed buckets:

| Fused limit | Target speed |
|-------------|--------------|
| `< 60 kph` | `60 kph` |
| `60..79 kph` | `80 kph` |
| `80..99 kph` | `100 kph` |
| `100..119 kph` | `120 kph` |
| `>= 120 kph` | no boost |

Offset rules:

- Offset = `targetSpeedKph - fusedSpeedLimitKph`.
- Absolute offset pre-clamp: `25 kph`.
- PCT4 cap: `50%`.
- Raw encoding: `raw = percent * 4`.
- Downward raw changes are limited to `15%/s`, rising changes pass through immediately.

### CAN Reliability Features

- TWAI alert handling.
- Bus-off detection and automatic recovery.
- Re-starts TWAI after bus recovery.
- TX failure short retry: one retry after a 1 ms delay.
- TX wait timeout: 5 ms.
- RX wait timeout: 1 ms.
- DLC guards for all parsed frames.
- Rejects extended frames, remote frames, and over-length frames.
- Hardware acceptance filter: coarse filter for IDs near `0x399 / 1016 / 1021`.
- Software exact filter: only `0x399`, `1016`, and `1021` are processed.

### Build & Flash

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

Erase then flash:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM15 erase_flash
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload --upload-port COM15
```

### Full BIN

This branch can produce a full merged image for flashing from offset `0x0`. The generated release files are usually stored under `release/ESP32-FSD/` when created locally.

### When To Use

Use this branch when you want the most capable ESP32-S3 version: HW3 FSD activation, speed-limit-aware offset control, PCT4 compatibility, slew limiting, and TWAI stability protections.

---

## 中文

### 概述

`ESP32-FSD` 是面向 Waveshare ESP32-S3-RS485-CAN 的增强版 ESP32-S3 TWAI 分支。它保留纯 CAN 的 HW3 固件，并加入基于限速的速度偏移、PCT4 编码、slew 限幅，以及更强的 TWAI 稳定性处理。

### 目标硬件

- 开发板：Waveshare ESP32-S3-RS485-CAN
- 框架：PlatformIO 下的 Arduino
- CAN 控制器：ESP32-S3 内置 TWAI
- CAN 速率：500 kbps
- Flash 目标：16 MB
- 分区表：`huge_app.csv`
- 默认引脚：
  - TWAI TX：GPIO15
  - TWAI RX：GPIO16
  - LED：GPIO14
- 队列大小：
  - RX 队列：64
  - TX 队列：16

### 主要功能

- 仅支持 HW3。
- 纯 CAN 固件：没有 WiFi、蓝牙、OTA、Web UI 或 Dashboard 运行逻辑。
- 从 CAN ID `1016` 读取跟车距离，并映射为 `speedProfile`。
- 处理 AP/FSD 控制帧 CAN ID `1021`。
- 从 CAN ID `0x399` 读取融合限速。
- 向 `1021 mux 2` 注入速度偏移。
- 使用 PCT4 速度偏移编码：`raw = 百分比 * 4`。
- 下降 slew 限幅：默认 `15%/秒`。
- 无有效融合限速时，使用原车/stock 速度偏移 raw 值。

### FSD / CAN 行为

- `1016`：跟车距离 -> `speedProfile`。
- `1021 mux 0`：设置激活位，并写入 `speedProfile`。
- `1021 mux 1`：清除相关控制/提示位。
- `1021 mux 2`：写入经过 slew 限幅后的 PCT4 速度偏移。
- `0x399`：从 `data[1] & 0x1F` 读取融合限速，有效值乘以 5 kph。

### 速度偏移逻辑

默认目标速度档：

| 融合限速 | 目标速度 |
|----------|----------|
| `< 60 kph` | `60 kph` |
| `60..79 kph` | `80 kph` |
| `80..99 kph` | `100 kph` |
| `100..119 kph` | `120 kph` |
| `>= 120 kph` | 不加速 |

偏移规则：

- 偏移 = `targetSpeedKph - fusedSpeedLimitKph`。
- 绝对偏移预夹紧：`25 kph`。
- PCT4 百分比上限：`50%`。
- raw 编码：`raw = 百分比 * 4`。
- raw 下降按 `15%/秒` 限幅，上升立即放行。

### CAN 稳定性功能

- TWAI alert 处理。
- Bus-off 检测和自动恢复。
- Bus 恢复后自动重新启动 TWAI。
- TX 失败短重试：延迟 1 ms 后重试 1 次。
- TX 等待超时：5 ms。
- RX 等待超时：1 ms。
- 所有解析帧都有 DLC 保护。
- 拒绝扩展帧、远程帧和超长帧。
- 硬件验收过滤：对 `0x399 / 1016 / 1021` 附近 ID 做粗过滤。
- 软件精确过滤：只处理 `0x399`、`1016`、`1021`。

### 编译与烧录

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

擦除后烧录：

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM15 erase_flash
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload --upload-port COM15
```

### 全量 BIN

本分支可生成从 `0x0` 写入的合并全量镜像。本地生成后通常放在 `release/ESP32-FSD/` 目录。

### 适用场景

如果你需要功能最完整的 ESP32-S3 版本：HW3 FSD 激活、限速感知速度偏移、PCT4 兼容、slew 限幅和 TWAI 稳定性保护，使用这个分支。
