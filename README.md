# waveshare_ESP32_S3_RS485_CAN — Speed-Offset (per-second slew)

**Language / 语言:** [English](#english) · [中文](#中文)

> ⚠️ **Disclaimer / 免责声明**
> This firmware modifies a moving vehicle's driver-assistance behaviour and
> touches safety-critical systems. It is provided for research/educational use
> only. You are solely responsible for legality, safety and any consequences of
> running it on real hardware.
> 本固件会改变行驶中车辆的驾驶辅助行为,涉及安全关键系统,仅供研究/学习。
> 合规性、安全性及一切后果由使用者自行承担。

---

## English

A CAN-bus middleware for the **Waveshare ESP32-S3-RS485-CAN** board that
adjusts Tesla **HW3** Autopilot/FSD behaviour in real time. It intercepts
specific control frames on the vehicle CAN bus, modifies a few bits, and
re-transmits them — adding a speed-limit-based **speed offset** and a
configurable **driving-mode (speed profile)**.

### Features

- **CAN-only build** — TWAI (built-in CAN) at 500 kbps. WiFi & Bluetooth are
  never initialised, so both radios stay powered down.
- **HW3 only** — single, lean handler (no HW4/Legacy code paths).
- **FSD activation** — sets the required bits on the `1021` AP-control frame
  (mux 0: activation bit; mux 1: nag-bit clear).
- **Driving-mode / speed profile** — derived from the follow-distance frame
  (`1016`) and written back into the control frame.
- **Speed-limit speed offset (PCT4)** — reads the fused speed limit from
  `0x399` (DAS_STATUS), computes a target cruise speed, and encodes the offset
  as a **percentage of the limit** (`raw = pct × 4`, capped at 50 %) into
  `1021` mux 2.
- **Per-second slew limiter** — damps *downward* changes of the wire offset at
  **15 %/sec** so the car does not brake abruptly when the fused limit suddenly
  drops (e.g. entering a school zone). Rising edges pass through immediately.
- **Hardware acceptance filter** — the TWAI controller only enqueues ~16 IDs
  around our targets (`0x399 / 1016 / 1021`) instead of the whole bus; a
  software exact-match (`isRelevantCanId`) does the final selection.
- **Robust TWAI handling** — automatic bus-off recovery, TX retry, frame
  validation (rejects extended/remote/over-length frames, DLC guards).
- **Activity LED** — GPIO14: off while a relevant frame is being processed,
  on when idle.

### Repository Architecture

`main` documents the baseline single-CAN ESP32-S3 architecture. Other branches
specialize this baseline for HW3/HW4, RP2040/MCP2515, LILYGO T-2CAN dual-CAN,
WebUI, and experimental capture work.

| Layer | Component | Role |
|-------|-----------|------|
| CAN A / CAN1 | ESP32-S3 built-in TWAI | Single primary CAN interface on the baseline branch. It receives selected AP/FSD frames, applies the speed/profile edits, and transmits the modified frame. |
| Main loop | Arduino `loop()` | Services TWAI alerts, drains the RX queue with ID/DLC validation, runs the frame handler, and keeps the LED/status behavior simple. |
| Frame logic | HW3 FSD/speed-offset handler | Tracks follow-distance/profile state, reads fused speed limit, computes PCT4 speed offset, applies slew limiting, and writes `1021` mux fields. |
| Branch family | ESP32 / RP2040 / T-2CAN branches | ESP32 branches use native TWAI; RP2040 branches use MCP2515 over SPI; T-2CAN branches add a second MCP2515 CAN bus and optional WebUI/PSRAM recorder. |
| Configuration | PlatformIO build flags | Pins, queue lengths, board targets, and optional feature flags are selected per branch/environment. |

### Hardware

| Item | Value |
|------|-------|
| Board | Waveshare ESP32-S3-RS485-CAN (16 MB flash, ESP32-S3, 8 MB PSRAM) |
| CAN TX | GPIO15 |
| CAN RX | GPIO16 |
| LED | GPIO14 |
| Bus | 500 kbps, normal mode |

Pins are overridable via PlatformIO `build_flags` (`-DTWAI_TX_PIN=…` etc.).

### Build & Flash

**PlatformIO (recommended):**

```bash
pio run   -e waveshare_ESP32_S3_RS485_CAN            # build
pio run   -e waveshare_ESP32_S3_RS485_CAN -t upload  # flash
pio device monitor -b 115200                          # serial (boot log only)
```

**Arduino IDE:** open `ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`, select
*ESP32S3 Dev Module*, set the pins above, compile & upload.

### How it works

```
loop():
  serviceTwaiAlerts()                 # bus-off recovery / health
  twai_recv()                         # HW filter + SW exact-match, drain queue
  speedLimitMonitor.update(frame)     # cache fused limit from 0x399
  handler.refreshUnifiedSpeedCompensation()   # recompute PCT4 offset
  handler.handelMessage(frame)        # modify + re-transmit 1016 / 1021
```

- **1016** → follow distance → `speedProfile`.
- **1021 mux 0** → set activation bit + write `speedProfile`.
- **1021 mux 1** → clear nag bit.
- **1021 mux 2** → inject the slew-limited PCT4 speed offset (or pass the stock
  value through when no fused limit is available).

### Tunables

Defined as `constexpr` near the top of the sketch:

| Name | Default | Meaning |
|------|---------|---------|
| `AUTO_TARGET_SPEED*` | 60/80/100/120 | target cruise speed per limit bucket |
| `MAX_SPEED_OFFSET_KPH` | 25 | absolute pre-clamp on the computed offset |
| `MAX_SPEED_OFFSET_PCT` | 50 | PCT4 wire cap |
| `OFFSET_SLEW_RATE_PCT_PER_SEC` | 15 | downward slew rate (%/sec) |

### Project layout

```
platformio.ini                      # build config (waveshare_ESP32_S3_RS485_CAN)
ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino   # HW3 firmware (this project)
RP2040CAN-FSD/RP2040CAN-FSD.ino     # original RP2040/MCP2515 reference port
```

### License

GPL-3.0 (see source header).

---

## 中文

一个运行在 **微雪 Waveshare ESP32-S3-RS485-CAN** 开发板上的 CAN 总线中间件,
用于实时调整特斯拉 **HW3** Autopilot/FSD 行为。它在车辆 CAN 总线上拦截特定控制
帧,修改若干比特后重新发出——叠加基于限速的**速度偏移**,以及可配置的
**驾驶模式(速度档位)**。

### 功能

- **纯 CAN 固件** —— 使用 TWAI(内置 CAN),500 kbps。WiFi 与蓝牙从不初始化,
  两个射频始终保持关闭。
- **仅 HW3** —— 单一精简处理器(无 HW4/Legacy 代码路径)。
- **FSD 激活** —— 在 `1021` AP 控制帧上置位(mux 0:激活位;mux 1:清除唠叨位)。
- **驾驶模式 / 速度档位** —— 由跟车距离帧(`1016`)推导,并写回控制帧。
- **限速速度偏移(PCT4)** —— 从 `0x399`(DAS_STATUS)读取融合限速,计算目标巡航
  速度,并把偏移按**限速的百分比**编码(`raw = 百分比 × 4`,上限 50%)写入
  `1021` mux 2。
- **每秒 slew 限幅** —— 以 **15%/秒** 抑制线上偏移的*下降*,避免融合限速骤降时
  (如进入学区)车辆突然刹车;上升边沿立即放行。
- **硬件验收过滤** —— TWAI 控制器只把目标附近约 16 个 ID(`0x399 / 1016 / 1021`)
  入队,而非整条总线;软件再做精确匹配(`isRelevantCanId`)。
- **健壮的 TWAI 处理** —— 自动 Bus-Off 恢复、发送重试、帧校验(拒绝扩展帧/远程帧/
  超长帧,DLC 防护)。
- **活动指示 LED** —— GPIO14:处理相关帧时熄灭,空闲时点亮。

### 仓库架构

`main` 说明基线单 CAN ESP32-S3 架构。其它分支在此基础上分别面向
HW3/HW4、RP2040/MCP2515、LILYGO T-2CAN 双 CAN、WebUI 和实验抓包。

| 层级 | 组件 | 作用 |
|------|------|------|
| CAN A / CAN1 | ESP32-S3 原生 TWAI | 基线分支的唯一主 CAN 接口。接收选定 AP/FSD 帧，应用速度/档位修改，再发送修改后的帧。 |
| 主循环 | Arduino `loop()` | 处理 TWAI alert，带 ID/DLC 校验地读取 RX 队列，运行帧处理器，并保持 LED/状态逻辑简单。 |
| 帧逻辑 | HW3 FSD/速度偏移处理器 | 保存跟车距离/速度档状态，读取融合限速，计算 PCT4 速度偏移，应用 slew 限幅，并写入 `1021` mux 字段。 |
| 分支族 | ESP32 / RP2040 / T-2CAN 分支 | ESP32 分支使用原生 TWAI；RP2040 分支使用 MCP2515 SPI；T-2CAN 分支增加第二路 MCP2515 CAN 与可选 WebUI/PSRAM 抓包器。 |
| 配置 | PlatformIO build flags | 引脚、队列长度、板型目标和可选功能开关按分支/环境选择。 |

### 硬件

| 项目 | 取值 |
|------|------|
| 开发板 | 微雪 ESP32-S3-RS485-CAN(16 MB flash,ESP32-S3,8 MB PSRAM) |
| CAN TX | GPIO15 |
| CAN RX | GPIO16 |
| LED | GPIO14 |
| 总线 | 500 kbps,正常模式 |

引脚可通过 PlatformIO `build_flags`(`-DTWAI_TX_PIN=…` 等)覆盖。

### 编译与烧录

**PlatformIO(推荐):**

```bash
pio run   -e waveshare_ESP32_S3_RS485_CAN            # 编译
pio run   -e waveshare_ESP32_S3_RS485_CAN -t upload  # 烧录
pio device monitor -b 115200                          # 串口(仅启动日志)
```

**Arduino IDE:** 打开 `ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`,选择
*ESP32S3 Dev Module*,设置上述引脚,编译并上传。

### 工作原理

```
loop():
  serviceTwaiAlerts()                 # Bus-Off 恢复 / 健康检查
  twai_recv()                         # 硬件过滤 + 软件精确匹配,清空队列
  speedLimitMonitor.update(frame)     # 缓存 0x399 的融合限速
  handler.refreshUnifiedSpeedCompensation()   # 重算 PCT4 偏移
  handler.handelMessage(frame)        # 修改并重发 1016 / 1021
```

- **1016** → 跟车距离 → `speedProfile`。
- **1021 mux 0** → 置激活位 + 写 `speedProfile`。
- **1021 mux 1** → 清唠叨位。
- **1021 mux 2** → 注入经 slew 限幅的 PCT4 速度偏移(无融合限速时透传车辆原值)。

### 可调参数

在源码顶部以 `constexpr` 定义:

| 名称 | 默认 | 含义 |
|------|------|------|
| `AUTO_TARGET_SPEED*` | 60/80/100/120 | 各限速区间的目标巡航速度 |
| `MAX_SPEED_OFFSET_KPH` | 25 | 对计算偏移的绝对预夹紧 |
| `MAX_SPEED_OFFSET_PCT` | 50 | PCT4 线上上限 |
| `OFFSET_SLEW_RATE_PCT_PER_SEC` | 15 | 下降 slew 速率(%/秒) |

### 目录结构

```
platformio.ini                      # 构建配置(waveshare_ESP32_S3_RS485_CAN)
ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino   # HW3 固件(本项目)
RP2040CAN-FSD/RP2040CAN-FSD.ino     # 原始 RP2040/MCP2515 参考移植
```

### 许可证

GPL-3.0(见源码文件头)。
