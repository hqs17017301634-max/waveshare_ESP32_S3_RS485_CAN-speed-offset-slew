# ESP32-HW4 - Tesla HW4 (FSD V14) CAN Middleware

> Warning: this firmware modifies CAN messages related to driver assistance on a
> moving vehicle. Research and educational use only. You are responsible for
> legality, safety, validation, and all consequences of running it on hardware.

## Overview

This is the HW4-only branch for the Waveshare ESP32-S3-RS485-CAN board. It is
CAN-only, does not initialize WiFi/Bluetooth/OTA/Web UI, and targets Tesla HW4
with FSD V14.

Current behavior:

- FSD activation on AP control frame `1021`.
- 5-level driving style from follow distance frame `1016`.
- Fixed HW4 speed offset `+15 kph`.
- AP control `mux 2` writes speed profile to `data[7]` bits 4..6.
- AP control `mux 2` writes speed offset to `data[1]` bits 0..5.
- TWAI bus-off recovery, TX retry, frame/DLC validation.

## Project Architecture

This branch is a single-bus Arduino / PlatformIO ESP32-S3 firmware. The primary
entry point is `ESP32S3CAN-FSD/ESP32S3CAN-FSD.ino`; no WebUI, Bluetooth, OTA,
dashboard, recorder, or secondary CAN runtime is started.

| Layer | Component | Role |
|-------|-----------|------|
| CAN A / CAN1 | ESP32-S3 built-in TWAI | The only CAN interface. It receives HW4 AP-control and follow-distance frames, edits the HW4 activation/profile/offset fields, and transmits the modified frame. |
| Main loop | Arduino `loop()` | Services TWAI alerts, receives bounded CAN frames, validates ID/DLC, and runs the HW4 handler. |
| Runtime logic | HW4 frame handler | Tracks follow-distance-derived driving style and writes fixed `+15 kph` speed offset on AP-control mux 2. |
| Configuration | Compile-time constants | No WebUI or persistent runtime config; behavior is fixed by this branch's source and PlatformIO build flags. |

## FSD Activation (`1021`)

| mux | Operation |
|-----|-----------|
| 0 | Set bit `46` (`UI_autosteerEnabled`), bit `60` (HW4 extended enable), and bit `59` (approaching-emergency-vehicle detection) |
| 1 | Clear bit `19`, set bit `47` |
| 2 | Write speed profile and fixed speed offset |

## Driving Style

Driving style is read from follow-distance frame `1016`. CAN raw equals UI
distance minus 1.

| UI distance | CAN raw | speedProfile | Style |
|-------------|---------|--------------|-------|
| 2 | 1 | 3 | Max |
| 3 | 2 | 2 | Hurry |
| 4 | 3 | 1 | Normal |
| 5 | 4 | 0 | Chill |
| 6 | 5 | 4 | Sloth |

## Speed Offset

The speed offset is fixed at `+15 kph`.

HW4 wire encoding:

```text
raw = round(offset_kph * 1.4)
```

For `+15 kph`:

```text
raw = round(15 * 1.4) = 21
```

The firmware writes `raw 21` to:

```text
1021 mux 2 data[1] bits 0..5
```

This matches the ev-open-can-tools HW4 preset:

```json
{ "type": "set_byte", "byte": 1, "val": 21, "mask": 63 }
```

## Hardware

| Item | Value |
|------|-------|
| Board | Waveshare ESP32-S3-RS485-CAN (16 MB flash, 8 MB PSRAM) |
| CAN TX / RX | GPIO15 / GPIO16 |
| LED | GPIO14 |
| Bus | 500 kbps |

## Build And Flash

```bash
pio run -e waveshare_ESP32_S3_RS485_CAN
pio run -e waveshare_ESP32_S3_RS485_CAN -t upload
```

Full 16 MB factory image, flashed from offset `0x0`:

```bash
esptool.py --chip esp32s3 --port COM15 write_flash 0x0 \
  release/ESP32-HW4/ESP32-HW4-waveshare-full-16MB.bin
```

## Tunables

| Name | Default | Meaning |
|------|---------|---------|
| `FIXED_SPEED_OFFSET_KPH` | 15 | Fixed speed offset in kph |
| `OFFSET_HW4_RAW_NUM/DEN` | 14/10 | HW4 offset encoding multiplier |
| `ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION` | true | mux0 bit 59 |

## Notes

The older RP2040/HW3 mixed sketch was removed from this branch. The active
PlatformIO source directory is `ESP32S3CAN-FSD`.
