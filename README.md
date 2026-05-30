# waveshare_ESP32_S3_RS485_CAN — Speed-Offset (per-second slew)

A CAN-bus middleware for the **Waveshare ESP32-S3-RS485-CAN** board that
adjusts Tesla **HW3** Autopilot/FSD behaviour in real time. It intercepts
specific control frames on the vehicle CAN bus, modifies a few bits, and
re-transmits them — adding a speed-limit-based **speed offset** and a
configurable **driving-mode (speed profile)**.

> ⚠️ **Disclaimer / 免责声明**
> This firmware modifies a moving vehicle's driver-assistance behaviour and
> touches safety-critical systems. It is provided for research/educational use
> only. You are solely responsible for legality, safety and any consequences of
> running it on real hardware.
> 本固件会改变行驶中车辆的驾驶辅助行为,涉及安全关键系统,仅供研究/学习。
> 合规性、安全性及一切后果由使用者自行承担。

---

## Features / 功能

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

## Hardware / 硬件

| Item | Value |
|------|-------|
| Board | Waveshare ESP32-S3-RS485-CAN (16 MB flash, ESP32-S3, 8 MB PSRAM) |
| CAN TX | GPIO15 |
| CAN RX | GPIO16 |
| LED | GPIO14 |
| Bus | 500 kbps, normal mode |

Pins are overridable via PlatformIO `build_flags` (`-DTWAI_TX_PIN=…` etc.).

## Build & Flash / 编译与烧录

**PlatformIO (recommended):**

```bash
pio run   -e waveshare_ESP32_S3_RS485_CAN            # build
pio run   -e waveshare_ESP32_S3_RS485_CAN -t upload  # flash
pio device monitor -b 115200                          # serial (boot log only)
```

**Arduino IDE:** open `ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`, select
*ESP32S3 Dev Module*, set the pins above, compile & upload.

## How it works / 工作原理

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

## Tunables / 可调参数

Defined as `constexpr` near the top of the sketch:

| Name | Default | Meaning |
|------|---------|---------|
| `AUTO_TARGET_SPEED*` | 60/80/100/120 | target cruise speed per limit bucket |
| `MAX_SPEED_OFFSET_KPH` | 25 | absolute pre-clamp on the computed offset |
| `MAX_SPEED_OFFSET_PCT` | 50 | PCT4 wire cap |
| `OFFSET_SLEW_RATE_PCT_PER_SEC` | 15 | downward slew rate (%/sec) |

## Project layout / 目录

```
platformio.ini                      # build config (waveshare_ESP32_S3_RS485_CAN)
ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino   # HW3 firmware (this project)
RP2040CAN-FSD/RP2040CAN-FSD.ino     # original RP2040/MCP2515 reference port
```

## License

GPL-3.0 (see source header).
