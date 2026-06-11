# T-2CAN-WEB

**Language / 语言:** [English](#english) · [中文](#中文)

> **Safety disclaimer / 安全免责声明**
> This firmware modifies and injects vehicle CAN messages. It is provided for research and educational use only. You are responsible for legality, safety validation, installation quality, and all consequences of using it on real hardware.
> 本固件会修改和发送车辆 CAN 报文，仅供研究和学习使用。实际使用前请自行承担合规、安全验证、安装质量及全部后果。

---

## English

### Overview

`T-2CAN-WEB` is the LILYGO T-2CAN branch of this firmware. It keeps the Arduino / PlatformIO build and a bounded CAN fast path while adding:

- HW3 FSD activation and speed-limit offset on the primary TWAI bus.
- A second MCP2515 CAN bus for body/lighting/preheat features.
- A lightweight SoftAP WebUI for runtime switches, status, and PSRAM-based CSV capture.

This README describes the current `T-2CAN-WEB` branch only. Older branch-generic notes were intentionally removed to avoid mixing incompatible wiring and build instructions.

### Hardware Target

- Board: LILYGO T-2CAN, ESP32-S3 N16R8 class hardware.
- Framework: Arduino via PlatformIO.
- CAN bitrate: 500 kbps on both buses.
- Flash: 16 MB.
- WebUI build uses OPI PSRAM for the recorder buffer.

### Bus Mapping

LILYGO's physical connector names are easy to confuse with older project text. In this branch, use the mapping below:

| Firmware bus | Controller | Official LILYGO physical port | Pins | Role |
|---|---|---|---|---|
| `bus=1` | ESP32-S3 native TWAI | physical `CANB` | TX `GPIO7`, RX `GPIO6` | Primary FSD activation and speed-control bus |
| `bus=2` | MCP2515 over SPI | physical `CANA` | SCK `GPIO12`, MOSI `GPIO11`, MISO `GPIO13`, CS `GPIO10`, RST `GPIO9`, INT `GPIO8`, 16 MHz crystal | Auxiliary body/lighting/preheat/recorder bus |

Important: official LILYGO T-2CAN V1.0 names physical `CANA` as MCP2515/SPI and physical `CANB` as native TWAI. This README uses the official physical names plus the firmware `bus=1` / `bus=2` labels.

### Architecture

| Layer | Component | Behavior |
|---|---|---|
| Main loop | Arduino `loop()` | Services TWAI alerts, handles one primary TWAI frame, drains MCP2515 with a bounded budget, and advances non-blocking feature state machines. |
| Primary CAN | `bus=1` / TWAI / physical CANB | Handles FSD activation, speed profile, speed offset, brake/gear context, and TWAI recovery. |
| Secondary CAN | `bus=2` / MCP2515 / physical CANA | Handles body/lighting/preheat/service-mode features and capture context. |
| WebUI | Low-priority FreeRTOS task on core 0 | Serves the SoftAP page, reads cached status, updates runtime config, and never runs inside the CAN fast path. |
| Recorder | PSRAM buffer | Stores binary CAN frames first; CSV is formatted only during HTTP download after recording stops. |

No OTA, SPIFFS, LittleFS, arbitrary CAN-send page, cross-bus bridge, or MITM rewrite is included in this branch.

### Primary FSD / Speed Control

Primary bus: `bus=1` / TWAI / physical CANB.

| CAN ID | Function | Behavior |
|---|---|---|
| `0x399` | Fused speed limit | Reads `data[1] & 0x1F`; raw `0` and `31` are invalid; valid value is `raw * 5 kph`. |
| `1016` / `0x3F8` | Follow distance | Reads `data[5] bit 5..7` and updates `speedProfile`. |
| `1021` / `0x3FD` mux 0 | FSD/profile control | Sets bit `46`, writes `speedProfile` into `data[6] bit 1..2`, then transmits. |
| `1021` / `0x3FD` mux 1 | Control bit | Clears bit `19`, then transmits. |
| `1021` / `0x3FD` mux 2 | Speed offset | Writes the computed PCT4 speed-offset raw value after downward slew limiting. |

Follow-distance mapping:

| Follow distance raw | Written `speedProfile` |
|---|---|
| `1` | `2` |
| `2` | `1` |
| `3` | `0` |
| Other | unchanged |

### Speed Offset Logic

The default speed table is:

| Fused speed limit | Target / behavior |
|---|---|
| `< 50 kph` | direct PCT4 raw `200` (50%) |
| `50..59 kph` | target `60 kph` |
| `60..69 kph` | target `80 kph` |
| `70..79 kph` | target `85 kph` |
| `80..89 kph` | target `90 kph` |
| `90..99 kph` | target `100 kph` |
| `100..119 kph` | target `120 kph` |
| `120..139 kph` | target `140 kph` |
| `>= 140 kph` | no added offset |

Rules:

- Desired offset is `targetSpeedKph - fusedSpeedLimitKph`.
- Absolute pre-clamp is `0..25 kph`.
- PCT4 wire encoding is `raw = round(offsetKph / fusedSpeedLimitKph * 100) * 4`.
- PCT4 cap is 50%, so max raw is `200`.
- Downward slew limit defaults to `5%/s`.
- Rising offset changes pass immediately.
- If no valid fused speed limit exists, the firmware preserves the stock speed-offset raw from the incoming frame.

### Reliability Features

- TWAI alert handling.
- Bus-off detection and automatic recovery.
- Short TX retry with bounded timeout.
- DLC length protection.
- Hardware and software filtering on the primary CAN IDs.
- Optional `bus=1` receive-only mode from WebUI, which blocks TWAI TX only.

### Secondary CAN Features

Secondary bus: `bus=2` / MCP2515 / physical CANA.

- MCP2515 starts after TWAI. If MCP2515 init fails, primary FSD/speed control continues.
- Normal drain budget: up to 4 frames per loop.
- When MCP2515 INT `GPIO8` is asserted or recorder is active: up to 24 frames with a 900 us time cap.
- Hardware filter modes:
  - `0`: receive all standard frames for debugging.
  - `1`: feature IDs.
  - `2`: minimum runtime IDs.
- Feature/minimum filters include `0x082` and `0x339` so battery-preheat monitoring and VCSEC lock-status detection are not lost.

Implemented `bus=2` features:

- **Service mode (`0x339`)**: WebUI switch queues a 4-frame burst at 10 ms spacing. Enable frame is `00 00 00 00 00 80 00 00`; disable frame clears byte 5.
- **Flash-to-pass strobe (`0x249`)**: when armed, two pull events within 1.2 s trigger 8 PULL/idle pulses. Default cadence is 75 ms ON / 75 ms OFF.
- **Rear-fog deceleration strobe (`0x273`)**: when armed, mild deceleration triggers 3 pulses and hard deceleration triggers 5 pulses. Cadence is 500 ms.
- **Reverse hazard + rear-fog strobe**: when armed, reverse gear on `bus=1` `0x118`, or brake + right-scroll back on `bus=2` `0x3C2`, triggers hazard and rear-fog pulses.
- **Scroll gear injection (`0x229`)**: experimental, default off. With brake pressed, right-scroll back requests R and right-scroll forward requests D.
- **Battery preheat (`0x082`)**: when enabled, sends dynamic `UI_tripPlanning` on `bus=2` every 200 ms.
- **Lock-triggered deep sleep**: when enabled, detects validated lock signals on `0x273` or `0x339`, inhibits all CAN TX, shuts down WiFi/TWAI, and enters ESP32 deep sleep.

### Battery Preheat Details

The firmware caches the latest valid vehicle `0x082` context (`byte1..7`) and only overrides `byte0`.

- Current fallback/context bytes: `01 50 AC 32 FF 03 61 15`.
- WebUI ON frame becomes: `AF 50 AC 32 FF 03 61 15`.
- WebUI OFF uses byte0 `0x01` and sends several OFF frames before stopping.

WebUI diagnostics decode:

- request/state bits,
- power request,
- target temperature,
- destination ambient temperature,
- charge target,
- raw energy-at-destination value,
- BMS temperature candidate frames `0x312`, `0x712`, and `0x374`.

Set MCP2515 hardware filter to receive-all when testing BMS temperature diagnostics, because those BMS candidate IDs are not in the feature/minimum filter set.

### Lock Deep Sleep

When enabled from WebUI, lock sleep only references `bus=2` / MCP2515 / physical CANA:

- `0x273` lock request values `1` or `4`.
- `0x339` VCSEC lock status.

On a validated lock signal:

- all CAN TX is inhibited,
- active strobe/preheat/gear state is cleared,
- WiFi is disabled,
- TWAI is stopped and uninstalled,
- ESP32 enters deep sleep.

No seat, EPAS, or generic timeout sleep heuristic is included. In the intended vehicle USB-power installation, the board wakes by cold-booting when USB power returns.

### WebUI

Build environment: `lilygo_t2can_arduino_webui`.

- SoftAP IP/gateway: `100.100.1.1`.
- Web task runs on core 0 at low priority.
- The CAN fast path does not call `server.handleClient()`.
- Page polling is user-controlled and limited to 1 second intervals.
- Closing WebUI stops requests and shuts the SoftAP down.
- Runtime config changes apply in RAM; persistent settings are written only when Save is pressed.
- No OTA page is included.

The WebUI exposes:

- FSD enable,
- auto speed-offset enable,
- speed table and slew setting,
- CANB enable,
- CANB filter mode,
- service mode,
- lighting/strobe features,
- scroll gear injection,
- battery preheat,
- lock deep sleep,
- receive-only TWAI mode,
- status counters,
- PSRAM recorder controls.

### Recorder

The WebUI build includes a lightweight dual-bus CSV recorder:

- stores binary frames in PSRAM first,
- supports include filters and excludes,
- records both bus directions and physical/controller labels,
- formats and streams CSV only when downloading after recording stops,
- does not write SPIFFS/LittleFS in the CAN fast path.

CSV labels:

- `bus=1`: `TWAI`, physical `CANB`.
- `bus=2`: `MCP2515`, physical `CANA`.

### Build And Upload

Non-WebUI LILYGO build:

```powershell
pio run -e lilygo_t2can_arduino
pio run -e lilygo_t2can_arduino -t upload --upload-port COMx
```

WebUI LILYGO build:

```powershell
pio run -e lilygo_t2can_arduino_webui
pio run -e lilygo_t2can_arduino_webui -t upload --upload-port COMx
```

Recent verified upload example:

```powershell
pio run -e lilygo_t2can_arduino_webui -t upload --upload-port COM23
```

### Files

- Firmware: `ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`
- WebUI page: `ESP32S3CAN-FSD/web_ui_page.h`
- Build config: `platformio.ini`
- Work log: `WORK_SUMMARY.md`

---

## 中文

### 概述

`T-2CAN-WEB` 是面向 LILYGO T-2CAN 的分支。它保持 Arduino / PlatformIO 框架和有预算限制的 CAN 快速处理路径，同时加入：

- 主 TWAI 总线上的 HW3 FSD 激活与限速偏移。
- 第二路 MCP2515 CAN，用于车身、灯光、预热等辅助功能。
- 轻量 SoftAP WebUI，用于运行期开关、状态查看和基于 PSRAM 的 CSV 抓包。

本 README 只描述当前 `T-2CAN-WEB` 分支。旧的通用分支说明已经从本分支 README 中删除，避免混用错误接线和编译命令。

### 目标硬件

- 开发板：LILYGO T-2CAN，ESP32-S3 N16R8 同级硬件。
- 框架：PlatformIO + Arduino。
- CAN 速率：两路均为 500 kbps。
- Flash：16 MB。
- WebUI 构建会启用 OPI PSRAM，用于抓包缓冲。

### 总线映射

LILYGO 官方物理端子名容易和旧项目文字混淆，本分支按下表理解：

| 固件 bus | 控制器 | LILYGO 官方物理端子 | 引脚 | 职责 |
|---|---|---|---|---|
| `bus=1` | ESP32-S3 原生 TWAI | 物理 `CANB` | TX `GPIO7`、RX `GPIO6` | 主 FSD 激活与限速控制总线 |
| `bus=2` | MCP2515 SPI | 物理 `CANA` | SCK `GPIO12`、MOSI `GPIO11`、MISO `GPIO13`、CS `GPIO10`、RST `GPIO9`、INT `GPIO8`、16 MHz 晶振 | 辅助车身、灯光、预热和抓包总线 |

重点：LILYGO T-2CAN V1.0 官方命名里，物理 `CANA` 是 MCP2515/SPI，物理 `CANB` 是原生 TWAI。本 README 同时写明固件 `bus=1` / `bus=2` 和官方物理端子名。

### 架构

| 层 | 组件 | 行为 |
|---|---|---|
| 主循环 | Arduino `loop()` | 处理 TWAI alert、处理一个主 TWAI 帧、按预算读取 MCP2515，并推进非阻塞功能状态机。 |
| 主 CAN | `bus=1` / TWAI / 物理 CANB | FSD 激活、速度档、限速偏移、刹车/档位上下文和 TWAI 恢复。 |
| 第二路 CAN | `bus=2` / MCP2515 / 物理 CANA | 车身、灯光、预热、Service Mode 和抓包上下文。 |
| WebUI | core 0 低优先级 FreeRTOS 任务 | 提供 SoftAP 页面，只读缓存状态和更新运行期配置，不进入 CAN 快路径。 |
| 抓包 | PSRAM 缓冲 | 先保存二进制 CAN 帧，停止抓包后下载时才格式化 CSV。 |

本分支不包含 OTA、SPIFFS、LittleFS、任意 CAN 发送页面、跨总线桥接或 MITM 改写。

### 主 FSD / 速度控制

主总线：`bus=1` / TWAI / 物理 CANB。

| CAN ID | 功能 | 行为 |
|---|---|---|
| `0x399` | 融合限速 | 读取 `data[1] & 0x1F`；raw `0` 和 `31` 无效；有效值为 `raw * 5 kph`。 |
| `1016` / `0x3F8` | 跟车距离 | 读取 `data[5] bit 5..7`，更新 `speedProfile`。 |
| `1021` / `0x3FD` mux 0 | FSD/速度档控制 | 设置 bit `46`，把 `speedProfile` 写入 `data[6] bit 1..2`，然后发送。 |
| `1021` / `0x3FD` mux 1 | 控制位 | 清除 bit `19`，然后发送。 |
| `1021` / `0x3FD` mux 2 | 速度偏移 | 写入经过缓降限制后的 PCT4 offset raw。 |

跟车距离映射：

| 跟车距离 raw | 写入的 `speedProfile` |
|---|---|
| `1` | `2` |
| `2` | `1` |
| `3` | `0` |
| 其他 | 保持不变 |

### 限速偏移逻辑

默认速度表：

| 融合限速 | 目标 / 行为 |
|---|---|
| `< 50 kph` | 直接 PCT4 raw `200`，即 50% |
| `50..59 kph` | 目标 `60 kph` |
| `60..69 kph` | 目标 `80 kph` |
| `70..79 kph` | 目标 `85 kph` |
| `80..89 kph` | 目标 `90 kph` |
| `90..99 kph` | 目标 `100 kph` |
| `100..119 kph` | 目标 `120 kph` |
| `120..139 kph` | 目标 `140 kph` |
| `>= 140 kph` | 不加偏移 |

规则：

- 期望偏移为 `targetSpeedKph - fusedSpeedLimitKph`。
- 绝对偏移预夹紧为 `0..25 kph`。
- PCT4 线编码为 `raw = round(offsetKph / fusedSpeedLimitKph * 100) * 4`。
- PCT4 上限为 50%，所以最大 raw 为 `200`。
- 下行缓降默认 `5%/s`。
- 上升变化立即放行。
- 如果没有有效融合限速，则保留原车帧里的 stock speed-offset raw。

### 稳定性功能

- TWAI alert 处理。
- bus-off 检测和自动恢复。
- 有界 TX 短重试。
- DLC 长度保护。
- 主 CAN ID 的硬件过滤和软件过滤。
- WebUI 可选 `bus=1` 只收不发模式，仅阻断 TWAI TX。

### 第二路 CAN 功能

第二路总线：`bus=2` / MCP2515 / 物理 CANA。

- MCP2515 在 TWAI 之后初始化。MCP2515 初始化失败时，主 FSD/速度控制仍继续工作。
- 普通循环每轮最多读取 4 帧。
- MCP2515 INT `GPIO8` 触发或抓包开启时，最多读取 24 帧，并有 900 us 时间上限。
- 硬件过滤模式：
  - `0`：接收全部标准帧，用于调试。
  - `1`：功能相关 ID。
  - `2`：最小运行 ID。
- 功能/最小过滤包含 `0x082` 和 `0x339`，避免漏掉电池预热监控和 VCSEC 锁车状态。

已实现的 `bus=2` 功能：

- **Service Mode（`0x339`）**：WebUI 开关会排队发送 4 帧、间隔 10 ms。开启帧为 `00 00 00 00 00 80 00 00`，关闭帧清除 byte 5。
- **超车灯爆闪（`0x249`）**：启用后，1.2 秒内两次 PULL 触发 8 次 PULL/idle 脉冲，默认 75 ms 开 / 75 ms 关。
- **后雾灯减速爆闪（`0x273`）**：启用后，缓减速触发 3 次，急减速触发 5 次，节奏 500 ms。
- **倒车双闪 + 后雾灯爆闪**：启用后，`bus=1` `0x118` 倒挡，或刹车 + `bus=2` `0x3C2` 右滚轮向后，触发双闪和后雾灯脉冲。
- **滚轮换挡注入（`0x229`）**：实验功能，默认关闭。踩刹车时，右滚轮向后请求 R，向前请求 D。
- **电池预热（`0x082`）**：启用后，每 200 ms 在 `bus=2` 发送动态 `UI_tripPlanning`。
- **锁车触发 deep sleep**：启用后，识别 `0x273` 或 `0x339` 的有效锁车信号，禁止所有 CAN TX、关闭 WiFi/TWAI，并进入 ESP32 deep sleep。

### 电池预热细节

固件缓存最新有效原车 `0x082` 上下文字段 `byte1..7`，只覆盖 `byte0`。

- 当前 fallback/context：`01 50 AC 32 FF 03 61 15`。
- WebUI ON 帧：`AF 50 AC 32 FF 03 61 15`。
- WebUI OFF 使用 byte0 `0x01`，并在停止前补发数帧 OFF。

WebUI 诊断会解码：

- 请求位和状态位，
- 功率请求，
- 目标温度，
- 目的地环境温度，
- 充电目标，
- 到达能量 raw 值，
- BMS 温度候选帧 `0x312`、`0x712`、`0x374`。

测试 BMS 温度诊断时，请把 MCP2515 硬件过滤设为接收全部，因为这些 BMS 候选 ID 不在功能/最小过滤集合里。

### 锁车 Deep Sleep

WebUI 启用后，锁车休眠只参考 `bus=2` / MCP2515 / 物理 CANA：

- `0x273` lock request 值 `1` 或 `4`。
- `0x339` VCSEC lock status。

识别到有效锁车信号后：

- 禁止所有 CAN TX，
- 清除活跃的爆闪、预热、换挡状态，
- 关闭 WiFi，
- 停止并卸载 TWAI，
- ESP32 进入 deep sleep。

没有移植座椅、EPAS 或通用超时休眠判断。车载 USB 供电安装场景下，预期唤醒方式是 USB 恢复供电后冷启动。

### WebUI

构建环境：`lilygo_t2can_arduino_webui`。

- SoftAP IP / gateway：`100.100.1.1`。
- Web 任务在 core 0 低优先级运行。
- CAN 快路径不调用 `server.handleClient()`。
- 页面轮询由用户手动开启，间隔限制为 1 秒。
- 关闭 WebUI 后会停止请求并关闭 SoftAP。
- 运行期配置先写 RAM，只有点击 Save 才持久化。
- 不包含 OTA 页面。

WebUI 提供：

- FSD 开关，
- 自动限速偏移开关，
- 速度表和缓降设置，
- CANB 开关，
- CANB 过滤模式，
- Service Mode，
- 灯光/爆闪功能，
- 滚轮换挡注入，
- 电池预热，
- 锁车 deep sleep，
- TWAI 只收不发模式，
- 状态计数器，
- PSRAM 抓包控制。

### 抓包

WebUI 构建包含轻量双路 CSV 抓包：

- 先把二进制帧保存到 PSRAM，
- 支持 include 和 exclude ID 过滤，
- 记录双路总线方向、controller 和 physical 标签，
- 停止抓包后下载时才格式化并流式输出 CSV，
- CAN 快路径不写 SPIFFS/LittleFS。

CSV 标签：

- `bus=1`：`TWAI`，物理 `CANB`。
- `bus=2`：`MCP2515`，物理 `CANA`。

### 编译与下载

无 WebUI 的 LILYGO 构建：

```powershell
pio run -e lilygo_t2can_arduino
pio run -e lilygo_t2can_arduino -t upload --upload-port COMx
```

WebUI 版 LILYGO 构建：

```powershell
pio run -e lilygo_t2can_arduino_webui
pio run -e lilygo_t2can_arduino_webui -t upload --upload-port COMx
```

最近验证过的上传示例：

```powershell
pio run -e lilygo_t2can_arduino_webui -t upload --upload-port COM23
```

### 文件

- 固件：`ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`
- WebUI 页面：`ESP32S3CAN-FSD/web_ui_page.h`
- 编译配置：`platformio.ini`
- 工作总结：`WORK_SUMMARY.md`
