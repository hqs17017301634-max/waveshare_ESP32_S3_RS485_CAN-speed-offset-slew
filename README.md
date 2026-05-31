# ESP32-FSD HW3 Speed Optimization / ESP32-FSD HW3 速度优化

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies CAN messages related to vehicle driver-assistance behavior. It is provided for research and educational use only. You are responsible for legality, safety, validation, and all consequences of using it on real hardware.
> 本固件会修改与车辆驾驶辅助相关的 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全、验证及全部后果。

---

## English

### Overview

`codex/hw3-speed-optimization` is an ESP32-S3 / TWAI / Arduino branch for Waveshare ESP32-S3-RS485-CAN. It is based on `ESP32-FSD` and keeps the same CAN-only HW3 FSD activation path, but adds a more refined speed-offset strategy.

Main changes compared with the base `ESP32-FSD` branch:

- New HW3 target-speed table: `60 / 80 / 90 / 100 / 120 / 140 kph`.
- Downward slew limiter changed from `15%/s` to `5%/s` for smoother deceleration.
- Startup speed fallback: before the first valid fused speed limit arrives, the firmware can apply a capped 60 kph fallback offset.
- Speed algorithm moved into `ESP32S3CAN-FSD/speed_algorithm.h`.
- Native unit tests added for target speed, PCT4, slew, and startup fallback behavior.
- Full BIN release files are included under `release/HW3-speed-optimization/`.

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
| `1021 mux 2` | Speed offset | Writes PCT4 speed-offset raw value after startup fallback / fused-limit calculation / slew limiting. |
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

- Uses startup fallback raw if no valid fused speed limit has ever been received and the startup fallback window is active.
- Uses computed PCT4 speed offset after a valid fused speed limit is available.
- Uses the original stock raw value if there is no valid fused speed limit after startup fallback has ended.
- Applies downward slew limiting only to the normal fused-limit path.

### Speed Limit Reading

`0x399` fused speed limit parsing:

- Source field: `data[1] & 0x1F`.
- Invalid raw values: `0` and `31`.
- Effective limit: `raw * 5 kph`.
- Valid effective range from this parser: `5..150 kph`.
- The first valid `0x399` permanently disables startup fallback.

### Target-Speed Table

| Fused speed limit | Target speed | Desired boost before clamp |
|-------------------|--------------|-----------------------------|
| `< 60 kph` | `60 kph` | `60 - limit` |
| `60..79 kph` | `80 kph` | `80 - limit` |
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

Examples with this optimized table:

| Fused limit | Target | Offset after clamp | Percent | PCT4 raw |
|-------------|--------|--------------------|---------|----------|
| `30 kph` | `60 kph` | `25 kph` | capped to `50%` | `200` |
| `50 kph` | `60 kph` | `10 kph` | `20%` | `80` |
| `60 kph` | `80 kph` | `20 kph` | `33%` | `132` |
| `70 kph` | `80 kph` | `10 kph` | `14%` | `56` |
| `80 kph` | `90 kph` | `10 kph` | `13%` | `52` |
| `90 kph` | `100 kph` | `10 kph` | `11%` | `44` |
| `100 kph` | `120 kph` | `20 kph` | `20%` | `80` |
| `120 kph` | `140 kph` | `20 kph` | `17%` | `68` |
| `140 kph+` | same as limit | `0 kph` | `0%` | `0` |

### Startup 60 kph Fallback

Startup fallback improves the low-speed experience immediately after boot or FSD activation, before map/vision fused speed limit is available.

- Active only before the first valid `0x399` fused speed limit is received.
- Window: `15 seconds`, counted from the first `1021 mux2` fallback use.
- Fallback raw: `200`, equivalent to PCT4 `50%` cap.
- If stock raw is already higher than `200`, stock raw is preserved.
- Once a valid `0x399` is received, fallback is permanently disabled.
- Fallback does not update the normal slew limiter state.

This is not an unconditional forced 60 kph command. It is a capped offset fallback, so very low baselines such as 15 or 30 kph may improve but are not guaranteed to become 60 kph.

### Slew Limiter

The slew limiter only limits downward raw changes, because sudden loss of offset can feel like sudden deceleration.

- Downward limit: `5%/s`.
- PCT4 raw rate: `5 * 4 = 20 raw units/s`.
- Rising offset changes pass immediately.
- Compared with the previous `15%/s`, downward changes are about 3x slower and smoother.

### CAN Reliability Features

- TWAI alerts enabled: bus-off, bus recovered, recovery in progress, error passive, bus error, TX failed, RX queue full, RX FIFO overrun.
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

### Included BIN Files

This branch includes generated full BIN files:

| File | Size | SHA256 |
|------|------|--------|
| `release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full-16MB.bin` | `16777216` | `4070126E0F9C8C873BED9816320EACE1055B33295B6065D0E8D8A6F7C7C8886A` |
| `release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full.bin` | `313344` | `D4BA692975511BCFAA51D1D2321524EC16AA8CB7C95D4F76BAC0BAF2F14C7068` |

Recommended file for full flashing from offset `0x0`:

```text
release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full-16MB.bin
```

### Build, Test & Flash

Build firmware:

```bash
pio run
```

Run native speed-algorithm tests:

```bash
pio test -e native
```

Upload through PlatformIO:

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

Erase then upload:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM15 erase_flash
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload --upload-port COM15
```

Flash included full 16 MB BIN from offset `0x0`:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM15 write_flash 0x0 release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full-16MB.bin
```

### When To Use

Use this branch when you want the ESP32-S3 HW3 FSD firmware with smoother speed-down behavior, startup low-speed fallback, a refined target-speed table, PCT4 compatibility, and TWAI stability protections.

---

## 中文

### 概述

`codex/hw3-speed-optimization` 是面向 Waveshare ESP32-S3-RS485-CAN 的 ESP32-S3 / TWAI / Arduino 分支。它基于 `ESP32-FSD`，保留纯 CAN、仅 HW3 的 FSD 激活路径，并加入更细的速度偏移策略。

相比基础 `ESP32-FSD` 分支，主要变化：

- 新 HW3 目标速度表：`60 / 80 / 90 / 100 / 120 / 140 kph`。
- 下降 slew 从 `15%/秒` 改为 `5%/秒`，降速更柔和。
- 启动速度兜底：第一次有效融合限速到来前，可以使用封顶的 60 kph fallback 偏移。
- 速度算法拆到 `ESP32S3CAN-FSD/speed_algorithm.h`。
- 增加 native 单元测试，覆盖目标速度表、PCT4、slew、启动 fallback。
- 已包含全量 BIN 文件，位于 `release/HW3-speed-optimization/`。

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
| `1021 mux 2` | 速度偏移 | 按启动 fallback / 融合限速计算 / slew 限幅后写入 PCT4 速度偏移 raw。 |
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

- 第一次有效融合限速到来前，如启动 fallback 窗口有效，则使用启动 fallback raw。
- 有效融合限速可用后，使用计算出的 PCT4 速度偏移。
- 启动 fallback 结束后仍无有效融合限速时，保留原车 stock raw。
- 下降 slew 只作用于正常融合限速路径。

### 限速读取

`0x399` 融合限速解析：

- 来源字段：`data[1] & 0x1F`。
- 无效 raw 值：`0` 和 `31`。
- 有效限速：`raw * 5 kph`。
- 此解析器有效范围：`5..150 kph`。
- 第一次收到有效 `0x399` 后，永久关闭启动 fallback。

### 目标速度表

| 融合限速 | 目标速度 | 夹紧前期望提升 |
|----------|----------|----------------|
| `< 60 kph` | `60 kph` | `60 - 限速` |
| `60..79 kph` | `80 kph` | `80 - 限速` |
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

优化表下的例子：

| 融合限速 | 目标 | 夹紧后偏移 | 百分比 | PCT4 raw |
|----------|------|------------|--------|----------|
| `30 kph` | `60 kph` | `25 kph` | 夹到 `50%` | `200` |
| `50 kph` | `60 kph` | `10 kph` | `20%` | `80` |
| `60 kph` | `80 kph` | `20 kph` | `33%` | `132` |
| `70 kph` | `80 kph` | `10 kph` | `14%` | `56` |
| `80 kph` | `90 kph` | `10 kph` | `13%` | `52` |
| `90 kph` | `100 kph` | `10 kph` | `11%` | `44` |
| `100 kph` | `120 kph` | `20 kph` | `20%` | `80` |
| `120 kph` | `140 kph` | `20 kph` | `17%` | `68` |
| `140 kph+` | 等于限速 | `0 kph` | `0%` | `0` |

### 启动 60 kph 兜底

启动 fallback 用于改善刚开机或刚激活 FSD、地图/视觉融合限速还没到来时的低速体验。

- 只在第一次有效 `0x399` 融合限速到来前生效。
- 窗口：`15 秒`，从第一次使用 `1021 mux2` fallback 开始计时。
- fallback raw：`200`，等于 PCT4 `50%` 上限。
- 如果原车 stock raw 已经高于 `200`，保留原车 stock raw。
- 一旦收到有效 `0x399`，fallback 永久关闭。
- fallback 不更新正常 slew 状态。

这不是无条件强制 60 kph，而是封顶偏移兜底。所以真实基准很低时，例如 15 或 30 kph，速度会改善，但不保证一定达到 60 kph。

### Slew 限幅

Slew 限幅只限制 raw 下降，因为偏移突然消失可能带来突然减速体感。

- 下降限幅：`5%/秒`。
- PCT4 raw 下降速率：`5 * 4 = 20 raw/秒`。
- 偏移上升立即放行。
- 相比之前 `15%/秒`，下降约慢 3 倍，体感更柔和。

### CAN 稳定性功能

- 启用的 TWAI alerts：bus-off、bus recovered、recovery in progress、error passive、bus error、TX failed、RX queue full、RX FIFO overrun。
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

### 已包含 BIN 文件

本分支已包含生成好的全量 BIN 文件：

| 文件 | 大小 | SHA256 |
|------|------|--------|
| `release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full-16MB.bin` | `16777216` | `4070126E0F9C8C873BED9816320EACE1055B33295B6065D0E8D8A6F7C7C8886A` |
| `release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full.bin` | `313344` | `D4BA692975511BCFAA51D1D2321524EC16AA8CB7C95D4F76BAC0BAF2F14C7068` |

推荐用于 `0x0` 全量烧录的文件：

```text
release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full-16MB.bin
```

### 编译、测试与烧录

编译固件：

```bash
pio run
```

运行速度算法单元测试：

```bash
pio test -e native
```

PlatformIO 上传：

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

擦除后上传：

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM15 erase_flash
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload --upload-port COM15
```

从 `0x0` 烧录已包含的 16 MB 全量 BIN：

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM15 write_flash 0x0 release/HW3-speed-optimization/ESP32-FSD-HW3-speed-optimization-full-16MB.bin
```

### 适用场景

如果你需要 ESP32-S3 HW3 FSD 固件，同时希望降速更柔和、刚激活时低速体验更好、目标速度表更细、兼容 PCT4，并保留 TWAI 稳定性保护，使用这个分支。
