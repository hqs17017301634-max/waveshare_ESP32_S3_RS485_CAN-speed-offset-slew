# ESP32-HW4 — Tesla HW4 (FSD V14) CAN middleware

**Language / 语言:** [English](#english) · [中文](#中文)

> ⚠️ **Disclaimer / 免责声明**
> This firmware modifies CAN messages related to a moving vehicle's
> driver-assistance behaviour and touches safety-critical systems. Research /
> educational use only. You are solely responsible for legality, safety,
> validation and all consequences of running it on real hardware.
> 本固件会修改与行驶中车辆驾驶辅助相关的 CAN 报文,涉及安全关键系统,仅供研究/学习。
> 合规性、安全性、验证及一切后果由使用者自行承担。

---

## English

The **HW4-only** branch for the Waveshare ESP32-S3-RS485-CAN board. It is
CAN-only (no WiFi/BT/OTA/Web UI) and targets **Tesla HW4 (FSD V14)**. It
intercepts the AP control frames, modifies a few bits, and re-transmits them —
adding FSD activation, a 5-level driving style, and a fused-speed-limit based
speed offset with downward slew limiting.

### Features

- **CAN-only** TWAI at 500 kbps; radios never initialised.
- **HW4 (FSD V14) only** — single lean handler.
- **FSD activation**, **5-level driving style**, **dynamic speed offset**, and
  **downward slew limiting** (see below).
- **Hardware acceptance filter** (~16 IDs around `0x399/1016/1021`) + software
  exact match.
- **Robust TWAI** — bus-off auto recovery, TX retry, frame/DLC validation.
- Activity LED on GPIO14 (off = processing a relevant frame, on = idle).

### FSD Activation (`1021`)

| mux | Operation |
|-----|-----------|
| 0 | set bit `46` (UI_autosteerEnabled) + bit `60` (HW4 extended enable) + bit `59` (approaching-emergency-vehicle, toggle) |
| 1 | clear bit `19` + set bit `47` |
| 2 | write speed profile → `data[7]` bits 4–6, write speed offset → `data[1]` bits 0–5 |

### Driving Style (speed profile)

From the follow-distance frame `1016`. CAN raw = UI distance − 1.

| UI distance | CAN raw | speedProfile | Style |
|-------------|---------|--------------|-------|
| 2 | 1 | 3 | Max |
| 3 | 2 | 2 | Hurry |
| 4 | 3 | 1 | Normal *(default)* |
| 5 | 4 | 0 | Chill |
| 6 | 5 | 4 | Sloth |

Profile value semantics: `0=Chill 1=Normal 2=Hurry 3=Max 4=Sloth`.

### Speed Offset (dynamic, fused-speed-limit based)

1. Read fused limit from `0x399` (`data[1] & 0x1F`, ×5 kph; `0`/`31` invalid).
2. Look up a target cruise speed, compute `offset = target − limit`, clamp `0..25 kph`.
3. Encode HW4 wire value `raw = round(offset × 1.4)`, 6-bit cap `0x3F`, into
   `1021 mux 2 data[1]` bits 0–5. (Matches ev-open-can-tools presets:
   `+5→7, +7→10, +10→14, +15→21`.)
4. No valid fused limit → pass the stock offset through.

Target-speed table:

| Fused limit (kph) | Target |
|-------------------|--------|
| < 60 | 60 |
| 60–69 | 80 |
| 70–79 | 85 |
| 80–89 | 90 |
| 90–99 | 100 |
| 100–119 | 120 |
| 120–139 | 140 |
| ≥ 140 | same as limit |

### Slew Limiter (downward only)

Damps *downward* changes of the wire offset at **≈5 kph/sec** (7 raw units/sec)
so the car does not brake abruptly when the fused limit suddenly drops. Rising
edges pass through immediately.

### Hardware

| Item | Value |
|------|-------|
| Board | Waveshare ESP32-S3-RS485-CAN (16 MB flash, 8 MB PSRAM) |
| CAN TX / RX | GPIO15 / GPIO16 |
| LED | GPIO14 |
| Bus | 500 kbps |

### Build & Flash

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

Full 16 MB factory image (flash from offset `0x0`):

```bash
esptool.py --chip esp32s3 --port COM15 write_flash 0x0 \
  release/ESP32-HW4/ESP32-HW4-waveshare-full-16MB.bin
```

The 16 MB image is published on the GitHub Releases page.

### Tunables (`constexpr` near top of the sketch)

| Name | Default | Meaning |
|------|---------|---------|
| `AUTO_TARGET_SPEED*` | 60…140 | target cruise speed per limit bucket |
| `MAX_SPEED_OFFSET_KPH` | 25 | absolute offset clamp |
| `OFFSET_HW4_RAW_NUM/DEN` | 14/10 | ×1.4 wire encoding |
| `OFFSET_SLEW_RATE_RAW_PER_SEC` | 7 | downward slew (~5 kph/s) |
| `ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION` | true | mux0 bit 59 |

---

## 中文

面向 Waveshare ESP32-S3-RS485-CAN 的 **HW4 专用**分支。纯 CAN(无 WiFi/蓝牙/OTA/
网页),针对**特斯拉 HW4(FSD V14)**。拦截 AP 控制帧,改几个比特后重发——实现
FSD 激活、5 档驾驶风格,以及基于融合限速的动态速度偏移 + 下降缓降。

### 功能

- **纯 CAN** TWAI 500 kbps;射频从不初始化。
- **仅 HW4(FSD V14)**,单一精简处理器。
- **FSD 激活**、**5 档驾驶风格**、**动态速度偏移**、**下降缓降**(见下)。
- **硬件验收过滤**(`0x399/1016/1021` 附近约 16 个 ID)+ 软件精确匹配。
- **健壮 TWAI**:Bus-Off 自动恢复、发送重试、帧/DLC 校验。
- GPIO14 活动指示灯(灭=处理相关帧,亮=空闲)。

### FSD 激活(`1021`)

| mux | 操作 |
|-----|------|
| 0 | 置 bit `46`(UI_autosteerEnabled)+ bit `60`(HW4 扩展使能)+ bit `59`(应急车辆检测,可关) |
| 1 | 清 bit `19` + 置 bit `47` |
| 2 | 速度档位 → `data[7]` 第4-6位,速度偏移 → `data[1]` 第0-5位 |

### 驾驶风格(speedProfile)

来自跟车距离帧 `1016`。CAN 原始值 = 车机 UI 格数 − 1。

| UI 格数 | CAN raw | speedProfile | 风格 |
|---------|---------|--------------|------|
| 2 | 1 | 3 | Max |
| 3 | 2 | 2 | Hurry |
| 4 | 3 | 1 | Normal *(默认)* |
| 5 | 4 | 0 | Chill |
| 6 | 5 | 4 | Sloth |

数值语义:`0=Chill 1=Normal 2=Hurry 3=Max 4=Sloth`。

### 速度偏移(动态,基于融合限速)

1. 读 `0x399` 融合限速(`data[1] & 0x1F`,×5 kph;`0`/`31` 无效)。
2. 查目标巡航速度,算 `偏移 = 目标 − 限速`,夹紧 `0..25 kph`。
3. HW4 线编码 `raw = round(偏移 × 1.4)`,6 位封顶 `0x3F`,写入 `1021 mux2 data[1]`
   第0-5位(对齐 ev-open-can-tools 预设:`+5→7, +7→10, +10→14, +15→21`)。
4. 无有效限速 → 透传车辆原值。

目标速度表:

| 融合限速(kph) | 目标 |
|---------------|------|
| < 60 | 60 |
| 60–69 | 80 |
| 70–79 | 85 |
| 80–89 | 90 |
| 90–99 | 100 |
| 100–119 | 120 |
| 120–139 | 140 |
| ≥ 140 | 同限速 |

### 缓降(仅限下降)

以 **≈5 kph/秒**(7 raw/秒)限制偏移的*下降*,避免融合限速骤降时突然减速;偏移
上升立即放行。

### 硬件

| 项目 | 取值 |
|------|------|
| 开发板 | Waveshare ESP32-S3-RS485-CAN(16 MB flash,8 MB PSRAM) |
| CAN TX / RX | GPIO15 / GPIO16 |
| LED | GPIO14 |
| 总线 | 500 kbps |

### 编译与烧录

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

16 MB 整片镜像(从 `0x0` 烧录):

```bash
esptool.py --chip esp32s3 --port COM15 write_flash 0x0 \
  release/ESP32-HW4/ESP32-HW4-waveshare-full-16MB.bin
```

16 MB 镜像发布在 GitHub Releases 页面。

### 可调参数(源码顶部 `constexpr`)

| 名称 | 默认 | 含义 |
|------|------|------|
| `AUTO_TARGET_SPEED*` | 60…140 | 各限速区间目标速度 |
| `MAX_SPEED_OFFSET_KPH` | 25 | 偏移绝对上限 |
| `OFFSET_HW4_RAW_NUM/DEN` | 14/10 | ×1.4 线编码 |
| `OFFSET_SLEW_RATE_RAW_PER_SEC` | 7 | 下降缓降(≈5 kph/s) |
| `ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION` | true | mux0 bit 59 |

### 许可证 / License

GPL-3.0(见源码文件头 / see source header)。
