/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ESP32S3 variant — uses the built-in TWAI (CAN) peripheral instead of MCP2515.

    CAN-only build for Waveshare ESP32-S3-RS485-CAN (16MB flash).
    WiFi and Bluetooth are never initialized, so both radios stay powered down.
    HW4 (FSD V14) only: FSD activation + follow-distance speed profile +
    fused-speed-limit speed offset (HW4 wire format) with downward slew limiting.
*/

#include <algorithm>
#include <cstring>
#include <driver/twai.h>

// This build supports the HW4 (FSD V14) car only.

constexpr bool enableAutoOffsetFromFusedSpeedLimit = true;

// Pin assignments — overridable via PlatformIO build_flags (-D...).
// Defaults match the Waveshare ESP32-S3-RS485-CAN board.
#ifndef TWAI_TX_PIN
#define TWAI_TX_PIN GPIO_NUM_15
#endif
#ifndef TWAI_RX_PIN
#define TWAI_RX_PIN GPIO_NUM_16
#endif
#ifndef PIN_LED
#define PIN_LED 14
#endif

// TWAI RX/TX queue depths — overridable via build_flags.
#ifndef TWAI_RX_QUEUE_LEN
#define TWAI_RX_QUEUE_LEN 64
#endif
#ifndef TWAI_TX_QUEUE_LEN
#define TWAI_TX_QUEUE_LEN 16
#endif

constexpr int AUTO_TARGET_SPEED = 60;
constexpr int AUTO_TARGET_SPEED_AT_80 = 80;
constexpr int AUTO_TARGET_SPEED_AT_85 = 85;
constexpr int AUTO_TARGET_SPEED_AT_90 = 90;
constexpr int AUTO_TARGET_SPEED_AT_100 = 100;
constexpr int AUTO_TARGET_SPEED_AT_120 = 120;
constexpr int AUTO_TARGET_SPEED_AT_140 = 140;
constexpr int MAX_SPEED_OFFSET_KPH = 25;        // absolute pre-clamp on the computed offset
// HW4 speed-offset wire encoding (1021 mux 2, data[1] bits 0..5 — a 6-bit field).
// Matches the ev-open-can-tools HW4 presets: raw = round(offset * 1.4).
//   +5 -> 7, +7 -> 10, +10 -> 14, +15 -> 21
constexpr int     OFFSET_HW4_RAW_NUM = 14;       // x1.4 numerator
constexpr int     OFFSET_HW4_RAW_DEN = 10;       // x1.4 denominator
constexpr uint8_t OFFSET_HW4_RAW_MAX = 0x3F;     // 6-bit field cap
constexpr uint8_t OFFSET_SLEW_RATE_RAW_PER_SEC = 7;  // downward slew ~5 kph/sec
// Approaching-emergency-vehicle detection (HW4 FSD V14, 1021 mux 0 bit 59).
constexpr bool ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION = true;
constexpr uint32_t TWAI_ALERT_MASK =
  TWAI_ALERT_BUS_OFF |
  TWAI_ALERT_BUS_RECOVERED |
  TWAI_ALERT_RECOVERY_IN_PROGRESS |
  TWAI_ALERT_ERR_PASS |
  TWAI_ALERT_BUS_ERROR |
  TWAI_ALERT_TX_FAILED |
  TWAI_ALERT_RX_QUEUE_FULL |
  TWAI_ALERT_RX_FIFO_OVERRUN;
constexpr uint32_t TWAI_TX_WAIT_MS = 5;
constexpr uint32_t TWAI_RX_WAIT_MS = 1;
constexpr uint32_t TWAI_TX_RETRY_DELAY_MS = 1;
constexpr uint8_t TWAI_TX_RETRY_COUNT = 1;
constexpr uint8_t TWAI_RX_SCAN_LIMIT = 8;

// ---- CAN frame abstraction (mirrors RP2040 can_frame for handler compatibility) ----

struct can_frame {
  uint32_t can_id;
  uint8_t  can_dlc;
  uint8_t  data[8];
};

static bool twaiRecoveryInProgress = false;

static bool isRelevantCanId(uint32_t canId);
static void serviceTwaiAlerts();

static bool twai_send(const can_frame& frame) {
  if (frame.can_dlc > 8) return false;

  twai_message_t msg = {};
  msg.identifier = frame.can_id;
  msg.data_length_code = frame.can_dlc;
  memcpy(msg.data, frame.data, frame.can_dlc);

  for (uint8_t attempt = 0; attempt <= TWAI_TX_RETRY_COUNT; ++attempt) {
    serviceTwaiAlerts();
    if (twaiRecoveryInProgress) return false;

    if (twai_transmit(&msg, pdMS_TO_TICKS(TWAI_TX_WAIT_MS)) == ESP_OK) {
      return true;
    }

    serviceTwaiAlerts();
    if (attempt < TWAI_TX_RETRY_COUNT) delay(TWAI_TX_RETRY_DELAY_MS);
  }
  return false;
}

static bool twai_recv(can_frame& frame) {
  twai_message_t msg;

  for (uint8_t i = 0; i < TWAI_RX_SCAN_LIMIT; ++i) {
    TickType_t waitTicks = (i == 0) ? pdMS_TO_TICKS(TWAI_RX_WAIT_MS) : 0;
    if (twai_receive(&msg, waitTicks) != ESP_OK) return false;
    if (msg.extd || msg.rtr || msg.data_length_code > 8 || !isRelevantCanId(msg.identifier)) {
      continue;
    }

    frame.can_id  = msg.identifier;
    frame.can_dlc = msg.data_length_code;
    memset(frame.data, 0, sizeof(frame.data));
    memcpy(frame.data, msg.data, frame.can_dlc);
    return true;
  }
  return false;
}

static void serviceTwaiAlerts() {
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) == ESP_OK) {
    if (alerts & TWAI_ALERT_BUS_OFF) {
      if (!twaiRecoveryInProgress) {
        twaiRecoveryInProgress = true;
        twai_initiate_recovery();
      }
      return;
    }

    if (alerts & TWAI_ALERT_RECOVERY_IN_PROGRESS) {
      twaiRecoveryInProgress = true;
      return;
    }

    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
      twaiRecoveryInProgress = twai_start() != ESP_OK;
      return;
    }
  }

  twai_status_info_t statusInfo = {};
  if (twai_get_status_info(&statusInfo) != ESP_OK) return;

  if (statusInfo.state == TWAI_STATE_BUS_OFF) {
    if (!twaiRecoveryInProgress) {
      twaiRecoveryInProgress = true;
      twai_initiate_recovery();
    }
    return;
  }

  if (statusInfo.state == TWAI_STATE_RECOVERING) {
    twaiRecoveryInProgress = true;
    return;
  }

  if (twaiRecoveryInProgress && statusInfo.state == TWAI_STATE_STOPPED) {
    twaiRecoveryInProgress = twai_start() != ESP_OK;
    return;
  }

  if (statusInfo.state == TWAI_STATE_RUNNING) {
    twaiRecoveryInProgress = false;
  }
}

// ---- CAN IDs ----

constexpr uint32_t CAN_ID_FOLLOW_DISTANCE = 1016;
constexpr uint32_t CAN_ID_AP_CONTROL = 1021;
constexpr uint32_t CAN_ID_DAS_STATUS = 0x399;

static inline bool isRelevantCanId(uint32_t canId) {
  return canId == CAN_ID_DAS_STATUS ||
         canId == CAN_ID_FOLLOW_DISTANCE ||
         canId == CAN_ID_AP_CONTROL;
}

// Hardware acceptance filter — a coarse pre-filter so the controller only
// enqueues the IDs near our targets instead of the whole bus; isRelevantCanId()
// still does the exact 3-ID match. Derived from the only IDs we touch:
//   0x399 (921), 0x3F8 (1016), 0x3FD (1021).
// Their common must-match bits give acceptance code 0x398; the differing ID
// bits {0,2,5,6} (mask 0x79A marks the must-match bits) are "don't care", so
// exactly 16 IDs in 0x398..0x3FD pass. Standard 11-bit IDs sit in bits [31:21];
// in twai_filter_config_t a mask bit of 1 means "don't care".
constexpr uint32_t CAN_ACCEPT_CODE = static_cast<uint32_t>(0x398) << 21;
constexpr uint32_t CAN_ACCEPT_MASK = ~(static_cast<uint32_t>(0x79A) << 21);

// ---- Unified speed compensation ----

struct UnifiedSpeedCompensationPort {
  bool hasFusedSpeedLimit = false;
  int fusedSpeedLimitKph = 0;
  int targetSpeedKph = 0;
  int offsetKph = 0;
  uint8_t speedOffsetRaw = 0;
};

struct SpeedLimitMonitor {
  can_frame dasStatusFrame{};
  bool hasDasStatusFrame = false;

  void update(const can_frame& frame) {
    if (frame.can_id == CAN_ID_DAS_STATUS && frame.can_dlc >= 2) {
      dasStatusFrame = frame;
      hasDasStatusFrame = true;
    }
  }

  bool getFusedSpeedLimitValue(int& limitValue) const {
    if (!hasDasStatusFrame || dasStatusFrame.can_dlc < 2) return false;
    const uint64_t fusedLimitRaw = (static_cast<uint64_t>(dasStatusFrame.data[1]) & 0x1F);
    if (fusedLimitRaw == 0 || fusedLimitRaw == 31) return false;
    limitValue = static_cast<int>(fusedLimitRaw * 5ULL);
    return true;
  }
};

SpeedLimitMonitor speedLimitMonitor;

// ---- Bit-level helpers ----

inline uint8_t readMuxID(const can_frame& frame) { return frame.data[0] & 0x07; }

inline void setBit(can_frame& frame, int bit, bool value) {
  int byteIndex = bit / 8;
  int bitIndex = bit % 8;
  uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
  if (value) frame.data[byteIndex] |= mask;
  else frame.data[byteIndex] &= static_cast<uint8_t>(~mask);
}

inline int clampOffsetKph(int value) { return std::max(std::min(value, MAX_SPEED_OFFSET_KPH), 0); }

inline uint8_t encodeSpeedOffsetRawHw4(int offsetKph) {
  if (offsetKph <= 0) return 0;
  int raw = (offsetKph * OFFSET_HW4_RAW_NUM + OFFSET_HW4_RAW_DEN / 2) / OFFSET_HW4_RAW_DEN;
  return static_cast<uint8_t>(std::min(raw, static_cast<int>(OFFSET_HW4_RAW_MAX)));
}

inline int getTargetSpeedForLimit(int fusedSpeedLimitValue) {
  if (fusedSpeedLimitValue < 60)  return AUTO_TARGET_SPEED;
  if (fusedSpeedLimitValue < 70)  return AUTO_TARGET_SPEED_AT_80;
  if (fusedSpeedLimitValue < 80)  return AUTO_TARGET_SPEED_AT_85;
  if (fusedSpeedLimitValue < 90)  return AUTO_TARGET_SPEED_AT_90;
  if (fusedSpeedLimitValue < 100) return AUTO_TARGET_SPEED_AT_100;
  if (fusedSpeedLimitValue < 120) return AUTO_TARGET_SPEED_AT_120;
  if (fusedSpeedLimitValue < 140) return AUTO_TARGET_SPEED_AT_140;
  return fusedSpeedLimitValue;
}

// HW4 speed offset lives only in 1021 mux 2 data[1] bits 0..5 (6-bit field).
inline uint8_t readSpeedOffsetRaw(const can_frame& frame) {
  return static_cast<uint8_t>(frame.data[1] & 0x3F);
}

inline void writeSpeedOffsetRaw(can_frame& frame, uint8_t raw) {
  frame.data[1] = static_cast<uint8_t>((frame.data[1] & ~0x3F) | (raw & 0x3F));
}

// HW4 writes the speed profile into 1021 mux 2 data[7] bits 4..6.
inline void setSpeedProfileHw4(can_frame& frame, int profile) {
  frame.data[7] = static_cast<uint8_t>((frame.data[7] & ~(0x07 << 4)) | ((profile & 0x07) << 4));
}

struct OffsetSlewLimiter {
  uint8_t lastRaw = 0;
  uint32_t lastSentMs = 0;

  uint8_t apply(uint8_t targetRaw) {
    uint8_t shapedRaw = targetRaw;
    const uint32_t now = millis();

    if (targetRaw < lastRaw && lastSentMs != 0) {
      const uint32_t rateRawPerSec = OFFSET_SLEW_RATE_RAW_PER_SEC;
      const uint32_t elapsedMs = now - lastSentMs;
      const uint32_t maxDrop = (rateRawPerSec * elapsedMs + 500U) / 1000U;
      const uint8_t floorRaw = lastRaw > maxDrop ? static_cast<uint8_t>(lastRaw - maxDrop) : 0;
      if (targetRaw < floorRaw) shapedRaw = floorRaw;
    }

    lastRaw = shapedRaw;
    lastSentMs = now;
    return shapedRaw;
  }
};

// ---- HW4 car handler ----

struct HW4Handler {
  int speedProfile = 1;
  bool FSDEnabled = true;
  UnifiedSpeedCompensationPort unifiedSpeedCompensation{};
  OffsetSlewLimiter offsetSlewLimiter{};

  void refreshUnifiedSpeedCompensation() {
    unifiedSpeedCompensation.hasFusedSpeedLimit = false;
    unifiedSpeedCompensation.fusedSpeedLimitKph = 0;
    unifiedSpeedCompensation.targetSpeedKph = 0;
    unifiedSpeedCompensation.offsetKph = 0;
    unifiedSpeedCompensation.speedOffsetRaw = 0;
    if (enableAutoOffsetFromFusedSpeedLimit) {
      int fusedSpeedLimitValue = 0;
      if (speedLimitMonitor.getFusedSpeedLimitValue(fusedSpeedLimitValue)) {
        unifiedSpeedCompensation.hasFusedSpeedLimit = true;
        unifiedSpeedCompensation.fusedSpeedLimitKph = fusedSpeedLimitValue;
        unifiedSpeedCompensation.targetSpeedKph = getTargetSpeedForLimit(fusedSpeedLimitValue);
        int desiredOffsetKph = unifiedSpeedCompensation.targetSpeedKph > fusedSpeedLimitValue
          ? (unifiedSpeedCompensation.targetSpeedKph - fusedSpeedLimitValue)
          : 0;
        unifiedSpeedCompensation.offsetKph = clampOffsetKph(desiredOffsetKph);
        unifiedSpeedCompensation.speedOffsetRaw =
          encodeSpeedOffsetRawHw4(unifiedSpeedCompensation.offsetKph);
      }
    }
  }

  void handelMessage(can_frame& frame) {
    if (frame.can_id == CAN_ID_FOLLOW_DISTANCE) {
      if (frame.can_dlc < 6) return;
      // Follow-distance (CAN raw = UI distance - 1) -> HW4 speed profile / style.
      // Profile value semantics: 0=Chill 1=Normal 2=Hurry 3=Max 4=Sloth.
      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
      switch (followDistance) {
        case 1: speedProfile = 3; break;  // UI 2 -> Max
        case 2: speedProfile = 2; break;  // UI 3 -> Hurry
        case 3: speedProfile = 1; break;  // UI 4 -> Normal
        case 4: speedProfile = 0; break;  // UI 5 -> Chill
        case 5: speedProfile = 4; break;  // UI 6 -> Sloth
      }
      return;
    }
    if (frame.can_id == CAN_ID_AP_CONTROL) {
      if (frame.can_dlc < 8) return;
      auto index = readMuxID(frame);
      if (index == 0 && FSDEnabled) {
        setBit(frame, 46, true);   // UI_autosteerEnabled
        setBit(frame, 60, true);   // HW4 extended enable
        if (ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION) setBit(frame, 59, true);
        twai_send(frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        setBit(frame, 47, true);
        twai_send(frame);
      }
      if (index == 2 && FSDEnabled) {
        // HW4 mux 2 carries both the speed profile (data[7]) and the speed offset (data[1]).
        setSpeedProfileHw4(frame, speedProfile);
        uint8_t speedOffsetRaw = unifiedSpeedCompensation.hasFusedSpeedLimit
          ? unifiedSpeedCompensation.speedOffsetRaw
          : readSpeedOffsetRaw(frame);
        speedOffsetRaw = offsetSlewLimiter.apply(speedOffsetRaw);
        writeSpeedOffsetRaw(frame, speedOffsetRaw);
        twai_send(frame);
      }
    }
  }
};

// ---- Main ----

HW4Handler handler;

void setup() {
  pinMode(PIN_LED, OUTPUT);

  delay(500);

  // Configure TWAI (CAN) peripheral at 500 kbps
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
    static_cast<gpio_num_t>(TWAI_TX_PIN),
    static_cast<gpio_num_t>(TWAI_RX_PIN),
    TWAI_MODE_NORMAL);
  g_config.rx_queue_len = TWAI_RX_QUEUE_LEN;
  g_config.tx_queue_len = TWAI_TX_QUEUE_LEN;
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f_config = { CAN_ACCEPT_CODE, CAN_ACCEPT_MASK, true };

  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();
  twai_reconfigure_alerts(TWAI_ALERT_MASK, nullptr);
}

void loop() {
  serviceTwaiAlerts();
  can_frame frame;
  if (!twai_recv(frame)) {
    digitalWrite(PIN_LED, HIGH);
    return;
  }
  digitalWrite(PIN_LED, LOW);
  speedLimitMonitor.update(frame);
  handler.refreshUnifiedSpeedCompensation();
  handler.handelMessage(frame);
}
