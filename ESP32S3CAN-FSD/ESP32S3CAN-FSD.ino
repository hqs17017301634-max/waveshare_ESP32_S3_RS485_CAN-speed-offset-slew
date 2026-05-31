/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ESP32S3 variant — uses the built-in TWAI (CAN) peripheral instead of MCP2515.

    CAN-only build for Waveshare ESP32-S3-RS485-CAN (16MB flash).
    WiFi and Bluetooth are never initialized, so both radios stay powered down.
    Only stable CAN messaging + FSD activation / speed control is kept.
*/

#include <cstring>
#include <driver/twai.h>
#include "speed_algorithm.h"

// This build supports the HW3 car only.

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
    return decodeFusedSpeedLimitKph(dasStatusFrame.data[1], limitValue);
  }
};

SpeedLimitMonitor speedLimitMonitor;

// ---- Bit-level helpers ----

inline uint8_t readMuxID(const can_frame& frame) { return frame.data[0] & 0x07; }

inline void setSpeedProfileV12V13(can_frame& frame, int profile) {
  frame.data[6] &= ~0x06;
  frame.data[6] |= (profile << 1);
}

inline void setBit(can_frame& frame, int bit, bool value) {
  int byteIndex = bit / 8;
  int bitIndex = bit % 8;
  uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
  if (value) frame.data[byteIndex] |= mask;
  else frame.data[byteIndex] &= static_cast<uint8_t>(~mask);
}

// ---- HW3 car handler ----

struct HW3Handler {
  int speedProfile = 1;
  bool FSDEnabled = true;
  SpeedCompensation unifiedSpeedCompensation{};
  OffsetSlewLimiter offsetSlewLimiter{};
  StartupSpeedFallback startupSpeedFallback{};

  void refreshUnifiedSpeedCompensation() {
    unifiedSpeedCompensation = {};
    if (enableAutoOffsetFromFusedSpeedLimit) {
      int fusedSpeedLimitValue = 0;
      if (speedLimitMonitor.getFusedSpeedLimitValue(fusedSpeedLimitValue)) {
        unifiedSpeedCompensation = calculateSpeedCompensation(fusedSpeedLimitValue);
        if (startupSpeedFallback.markValidFusedSpeedLimit()) {
          offsetSlewLimiter.reset(unifiedSpeedCompensation.speedOffsetRaw, millis());
        }
      }
    }
  }

  void handelMessage(can_frame& frame) {
    if (frame.can_id == CAN_ID_FOLLOW_DISTANCE) {
      if (frame.can_dlc < 6) return;
      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
      switch (followDistance) {
        case 1: speedProfile = 2; break;
        case 2: speedProfile = 1; break;
        case 3: speedProfile = 0; break;
      }
      return;
    }
    if (frame.can_id == CAN_ID_AP_CONTROL) {
      if (frame.can_dlc < 8) return;
      auto index = readMuxID(frame);
      if (index == 0 && FSDEnabled) {
        setBit(frame, 46, true);
        setSpeedProfileV12V13(frame, speedProfile);
        twai_send(frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        twai_send(frame);
      }
      if (index == 2 && FSDEnabled) {
        const uint32_t now = millis();
        const uint8_t stockSpeedOffsetRaw = readSpeedOffsetRaw(frame.data);
        uint8_t speedOffsetRaw = stockSpeedOffsetRaw;
        if (unifiedSpeedCompensation.hasFusedSpeedLimit) {
          speedOffsetRaw = offsetSlewLimiter.apply(unifiedSpeedCompensation.speedOffsetRaw, now);
        } else {
          speedOffsetRaw = startupSpeedFallback.selectRaw(stockSpeedOffsetRaw, now);
        }
        writeSpeedOffsetRaw(frame.data, speedOffsetRaw);
        twai_send(frame);
      }
    }
  }
};

// ---- Main ----

HW3Handler handler;

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
