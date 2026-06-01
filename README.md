# ESP32-FSD / ESP32-FSD

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`ESP32-FSD` is the enhanced ESP32-S3 TWAI branch for Waveshare ESP32-S3-RS485-CAN. It is CAN-only and HW3-only, but includes the full practical feature set currently considered useful: FSD activation, follow-distance speed profile control, fused-speed-limit based speed offset, PCT4 encoding, downward slew limiting, and TWAI reliability handling.

No WiFi, Bluetooth, OTA, Web UI, or dashboard runtime is initialized.

### Target Hardware

- Board: Waveshare ESP32-S3-RS485-CAN
- Framework: Arduino via PlatformIO
- CAN controller: ESP32-S3 built-in TWAI
- CAN bitrate: 500 kbps
- Flash target: 16 MB
- Partition table: `huge_app.csv`
- Default TWAI TX: GPIO15
- Default TWAI RX: GPIO16
- LED: GPIO14
- RX queue length: 64
- TX queue length: 16

### CAN Functions

| CAN ID | Function | Behavior |
|--------|----------|----------|
| `1016` | Follow distance | Reads `data[5] bit 5..7` and updates `speedProfile`. |
| `1021 mux 0` | FSD/profile control | Sets bit `46`, writes `speedProfile` into `data[6] bit 1..2`, then transmits. |
| `1021 mux 1` | Control/nag bit | Clears bit `19`, then transmits. |
| `1021 mux 2` | Speed offset | Writes PCT4 speed-offset raw value after slew limiting. |
| `0x399` | Fused speed limit | Reads `data[1] & 0x1F`; `0` and `31` are invalid; valid raw value is multiplied by `5 kph`. |

### Follow-Distance / Speed-Profile Mapping

| Follow distance raw value from `1016` | Written `speedProfile` | Firmware effect |
|---------------------------------------|------------------------|-----------------|
| `1` | `2` | More aggressive profile |
| `2` | `1` | Middle/default profile |
| `3` | `0` | Softer profile |
| Other values | unchanged | Keeps last profile |

### FSD Activation Details

On `1021 mux 0`:

- Sets bit `46` to enable the HW3 FSD/profile control path.
- Writes current `speedProfile` into `data[6] bit 1..2`.
- Sends the modified frame through TWAI.

On `1021 mux 1`:

- Clears bit `19`.
- Sends the modified frame through TWAI.

On `1021 mux 2`:

- Uses the computed PCT4 speed offset if a valid fused speed limit is available.
- If no valid fused speed limit is available, preserves the original stock speed-offset raw value from the frame.
- Applies downward slew limiting before writing the raw value.

### Speed Limit Reading

`0x399` fused speed limit parsing:

- Source field: `data[1] & 0x1F`.
- Invalid raw values: `0` and `31`.
- Effective limit: `raw * 5 kph`.
- Valid effective range from this parser: `5..150 kph`.

### Target-Speed Table

| Fused speed limit | Target speed | Maximum desired boost before clamp |
|-------------------|--------------|------------------------------------|
| `< 50 kph` | capped `50%` | raw `200` |
| `50..59 kph` | `60 kph` | `60 - limit` |
| `60..69 kph` | `80 kph` | `80 - limit` |
| `70..79 kph` | `85 kph` | `85 - limit` |
| `80..89 kph` | `90 kph` | `90 - limit` |
| `90..99 kph` | `100 kph` | `100 - limit` |
| `100..119 kph` | `120 kph` | `120 - limit` |
| `120..139 kph` | `140 kph` | `140 - limit` |
| `>= 140 kph` | same as limit | `0 kph` |

### Speed Offset Rules

- Desired offset: `targetSpeedKph - fusedSpeedLimitKph`.
- Absolute offset pre-clamp: `0..25 kph`.
- PCT4 percentage cap: `50%`.
- PCT4 raw formula: `raw = round(offsetKph / fusedSpeedLimitKph * 100) * 4`.
- Final raw range used by the algorithm: `0..200`.
- Raw is written into `1021 mux 2` using `data[0] bit 6..7` and `data[1] bit 0..5`.

Examples with the default table:

| Fused limit | Target | Offset after clamp | Percent | PCT4 raw |
|-------------|--------|--------------------|---------|----------|
| `30 kph` | `60 kph` | `25 kph` | capped to `50%` | `200` |
| `45 kph` | capped `50%` | n/a | `50%` | `200` |
| `50 kph` | `60 kph` | `10 kph` | `20%` | `80` |
| `55 kph` | `60 kph` | `5 kph` | `9%` | `36` |
| `60 kph` | `80 kph` | `20 kph` | `33%` | `132` |
| `70 kph` | `85 kph` | `15 kph` | `21%` | `84` |
| `75 kph` | `85 kph` | `10 kph` | `13%` | `52` |
| `80 kph` | `90 kph` | `10 kph` | `13%` | `52` |
| `90 kph` | `100 kph` | `10 kph` | `11%` | `44` |
| `100 kph` | `120 kph` | `20 kph` | `20%` | `80` |
| `120 kph` | `140 kph` | `20 kph` | `17%` | `68` |
| `140 kph+` | same as limit | `0 kph` | `0%` | `0` |

### Slew Limiter

The slew limiter only limits downward raw changes, because sudden loss of offset can feel like sudden deceleration.

- Default downward limit: `5%/s`.
- PCT4 raw rate: `5 * 4 = 20 raw units/s`.
- Rising offset changes pass immediately.
- If fused speed limit suddenly drops and target raw goes lower, the firmware ramps downward instead of jumping directly.

### CAN Reliability Features

- TWAI alerts enabled:
  - bus-off
  - bus recovered
  - recovery in progress
  - error passive
  - bus error
  - TX failed
  - RX queue full
  - RX FIFO overrun
- Bus-off is detected and automatic recovery is initiated.
- TWAI is restarted after recovery.
- TX failure short retry: `1` retry after `1 ms`.
- TX wait timeout: `5 ms`.
- RX wait timeout: `1 ms`.
- RX scan limit: `8` frames per loop pass.
- DLC protection for all parsed frames.
- Rejects extended frames, remote frames, and over-length frames.
- Hardware acceptance filter: coarse 16-ID pass set around `0x399`, `1016`, and `1021`.
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

### Full 16 MB BIN

This branch can generate a full merged 16 MB image for flashing from offset `0x0`. A typical local output path is:

```text
release/ESP32-FSD/ESP32-FSD-waveshare-full-16MB.bin
```

The 16 MB full image is convenient for complete factory-style flashing. Normal PlatformIO upload is still fine for development.

### When To Use

Use this branch when you want the most capable current ESP32-S3 firmware: HW3 FSD activation, speed-limit-aware speed offset, DEV-compatible PCT4 default encoding, downward slew protection, and TWAI stability handling.

---

## 中文

### 概述

`ESP32-FSD` 是面向 Waveshare ESP32-S3-RS485-CAN 的增强版 ESP32-S3 TWAI 分支。它是纯 CAN、仅 HW3 的固件，但包含目前最有实际价值的一套功能：FSD 激活、跟车距离速度档控制、基于融合限速的速度偏移、PCT4 编码、下降 slew 限幅，以及 TWAI 稳定性处理。

不会初始化 WiFi、蓝牙、OTA、Web UI 或 Dashboard 运行逻辑。

### 目标硬件

- 开发板：Waveshare ESP32-S3-RS485-CAN
- 框架：PlatformIO 下的 Arduino
- CAN 控制器：ESP32-S3 内置 TWAI
- CAN 速率：500 kbps
- Flash 目标：16 MB
- 分区表：`huge_app.csv`
- 默认 TWAI TX：GPIO15
- 默认 TWAI RX：GPIO16
- LED：GPIO14
- RX 队列长度：64
- TX 队列长度：16

### CAN 功能

| CAN ID | 功能 | 行为 |
|--------|------|------|
| `1016` | 跟车距离 | 读取 `data[5] bit 5..7`，更新 `speedProfile`。 |
| `1021 mux 0` | FSD/速度档控制 | 设置 bit `46`，把 `speedProfile` 写入 `data[6] bit 1..2`，然后发送。 |
| `1021 mux 1` | 控制/提示位 | 清除 bit `19`，然后发送。 |
| `1021 mux 2` | 速度偏移 | 写入经过 slew 限幅后的 PCT4 速度偏移 raw 值。 |
| `0x399` | 融合限速 | 读取 `data[1] & 0x1F`；`0` 和 `31` 无效；有效 raw 乘以 `5 kph`。 |

### 跟车距离 / 速度档映射

| `1016` 跟车距离 raw 值 | 写入的 `speedProfile` | 固件效果 |
|------------------------|------------------------|----------|
| `1` | `2` | 更激进速度档 |
| `2` | `1` | 中间/默认速度档 |
| `3` | `0` | 更柔和速度档 |
| 其他值 | 不变 | 保持上一档 |

### FSD 激活细节

在 `1021 mux 0` 上：

- 设置 bit `46`，启用 HW3 FSD/速度档控制路径。
- 把当前 `speedProfile` 写入 `data[6] bit 1..2`。
- 通过 TWAI 发送修改后的帧。

在 `1021 mux 1` 上：

- 清除 bit `19`。
- 通过 TWAI 发送修改后的帧。

在 `1021 mux 2` 上：

- 如果存在有效融合限速，使用计算出的 PCT4 速度偏移。
- 如果没有有效融合限速，保留帧内原车 stock 速度偏移 raw 值。
- 写入前应用下降 slew 限幅。

### 限速读取

`0x399` 融合限速解析：

- 来源字段：`data[1] & 0x1F`。
- 无效 raw 值：`0` 和 `31`。
- 有效限速：`raw * 5 kph`。
- 此解析器有效范围：`5..150 kph`。

### 目标速度表

| 融合限速 | 目标速度 | 夹紧前最大期望提升 |
|----------|----------|--------------------|
| `< 50 kph` | 夹到 `50%` | raw `200` |
| `50..59 kph` | `60 kph` | `60 - 限速` |
| `60..69 kph` | `80 kph` | `80 - 限速` |
| `70..79 kph` | `85 kph` | `85 - 限速` |
| `80..89 kph` | `90 kph` | `90 - 限速` |
| `90..99 kph` | `100 kph` | `100 - 限速` |
| `100..119 kph` | `120 kph` | `120 - 限速` |
| `120..139 kph` | `140 kph` | `140 - 限速` |
| `>= 140 kph` | 等于限速 | `0 kph` |

### 速度偏移规则

- 期望偏移：`targetSpeedKph - fusedSpeedLimitKph`。
- 绝对偏移预夹紧：`0..25 kph`。
- PCT4 百分比上限：`50%`。
- PCT4 raw 公式：`raw = round(offsetKph / fusedSpeedLimitKph * 100) * 4`。
- 算法最终使用 raw 范围：`0..200`。
- raw 写入 `1021 mux 2` 的 `data[0] bit 6..7` 和 `data[1] bit 0..5`。

默认表下的例子：

| 融合限速 | 目标 | 夹紧后偏移 | 百分比 | PCT4 raw |
|----------|------|------------|--------|----------|
| `30 kph` | `60 kph` | `25 kph` | 夹到 `50%` | `200` |
| `45 kph` | 夹到 `50%` | n/a | `50%` | `200` |
| `50 kph` | `60 kph` | `10 kph` | `20%` | `80` |
| `55 kph` | `60 kph` | `5 kph` | `9%` | `36` |
| `60 kph` | `80 kph` | `20 kph` | `33%` | `132` |
| `70 kph` | `85 kph` | `15 kph` | `21%` | `84` |
| `75 kph` | `85 kph` | `10 kph` | `13%` | `52` |
| `80 kph` | `90 kph` | `10 kph` | `13%` | `52` |
| `90 kph` | `100 kph` | `10 kph` | `11%` | `44` |
| `100 kph` | `120 kph` | `20 kph` | `20%` | `80` |
| `120 kph` | `140 kph` | `20 kph` | `17%` | `68` |
| `140 kph+` | 等于限速 | `0 kph` | `0%` | `0` |

### Slew 限幅

Slew 限幅只限制 raw 下降，因为偏移突然消失可能带来突然减速体感。

- 默认下降限幅：`5%/秒`。
- PCT4 raw 下降速率：`5 * 4 = 20 raw/秒`。
- 偏移上升立即放行。
- 当融合限速突然变化导致目标 raw 降低时，固件会平滑下降，而不是直接跳到低值。

### CAN 稳定性功能

- 启用的 TWAI alerts：
  - bus-off
  - bus recovered
  - recovery in progress
  - error passive
  - bus error
  - TX failed
  - RX queue full
  - RX FIFO overrun
- 检测 bus-off 并自动发起恢复。
- 恢复后自动重新启动 TWAI。
- TX 失败短重试：`1` 次，间隔 `1 ms`。
- TX 等待超时：`5 ms`。
- RX 等待超时：`1 ms`。
- RX 每轮最多扫描：`8` 帧。
- 所有解析帧都有 DLC 保护。
- 拒绝扩展帧、远程帧和超长帧。
- 硬件验收过滤：围绕 `0x399`、`1016`、`1021` 的 16 个 ID 粗过滤集合。
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

### 16 MB 全量 BIN

本分支可以生成从 `0x0` 写入的 16 MB 合并全量镜像。常见本地输出路径：

```text
release/ESP32-FSD/ESP32-FSD-waveshare-full-16MB.bin
```

16 MB 全量包适合完整工厂式烧录；日常开发仍可用普通 PlatformIO upload。

### 适用场景

如果你需要当前功能最完整的 ESP32-S3 固件：HW3 FSD 激活、限速感知速度偏移、DEV 兼容的 PCT4 默认编码、下降 slew 保护，以及 TWAI 稳定性处理，使用这个分支。
