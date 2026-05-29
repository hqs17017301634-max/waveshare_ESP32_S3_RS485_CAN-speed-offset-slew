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

#include <algorithm>
#include <cstring>
#include <driver/twai.h>

// This build supports the HW3 car only.

bool enableAutoOffsetFromFusedSpeedLimit = true;
bool enableFixedOffsetRawTest = false;

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
constexpr int AUTO_TARGET_SPEED_AT_100 = 100;
constexpr int AUTO_TARGET_SPEED_AT_120 = 120;
constexpr int SPEED_OFFSET_RAW_PER_MPH = 10;
constexpr int MAX_SPEED_OFFSET_MPH = 25;
constexpr uint8_t FIXED_OFFSET_TEST_RAW = 40;

// ---- CAN frame abstraction (mirrors RP2040 can_frame for handler compatibility) ----

struct can_frame {
  uint32_t can_id;
  uint8_t  can_dlc;
  uint8_t  data[8];
};

static bool twai_send(const can_frame& frame) {
  twai_message_t msg = {};
  msg.identifier = frame.can_id;
  msg.data_length_code = frame.can_dlc;
  memcpy(msg.data, frame.data, 8);
  return twai_transmit(&msg, pdMS_TO_TICKS(5)) == ESP_OK;
}

static bool twai_recv(can_frame& frame) {
  twai_message_t msg;
  if (twai_receive(&msg, 0) != ESP_OK) return false;
  frame.can_id  = msg.identifier;
  frame.can_dlc = msg.data_length_code;
  memcpy(frame.data, msg.data, 8);
  return true;
}

// ---- CAN IDs ----

constexpr uint32_t CAN_ID_DAS_STATUS = 0x399;

// ---- Unified speed compensation ----

struct UnifiedSpeedCompensationPort {
  bool hasFusedSpeedLimit = false;
  int fusedSpeedLimitMph = 0;
  int targetSpeedMph = 0;
  int offsetMph = 0;
};

struct SpeedLimitMonitor {
  can_frame dasStatusFrame{};
  bool hasDasStatusFrame = false;

  void update(const can_frame& frame) {
    if (frame.can_id == CAN_ID_DAS_STATUS) {
      dasStatusFrame = frame;
      hasDasStatusFrame = true;
    }
  }

  bool getFusedSpeedLimitValue(int& limitValue) const {
    if (!hasDasStatusFrame) return false;
    const uint64_t fusedLimitRaw = (static_cast<uint64_t>(dasStatusFrame.data[1]) & 0x1F);
    if (fusedLimitRaw == 0 || fusedLimitRaw == 31) return false;
    limitValue = static_cast<int>(fusedLimitRaw * 5ULL);
    return true;
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

inline int clampOffsetMph(int value) { return std::max(std::min(value, MAX_SPEED_OFFSET_MPH), 0); }

inline uint8_t encodeSpeedOffsetRaw(int offsetMph) {
  return static_cast<uint8_t>(std::max(std::min(offsetMph * SPEED_OFFSET_RAW_PER_MPH, 255), 0));
}

inline int getTargetSpeedForLimit(int fusedSpeedLimitValue) {
  if (fusedSpeedLimitValue < 60)  return AUTO_TARGET_SPEED;
  if (fusedSpeedLimitValue < 80)  return AUTO_TARGET_SPEED_AT_80;
  if (fusedSpeedLimitValue < 100) return AUTO_TARGET_SPEED_AT_100;
  if (fusedSpeedLimitValue < 120) return AUTO_TARGET_SPEED_AT_120;
  return fusedSpeedLimitValue;
}

// ---- HW3 car handler ----

struct HW3Handler {
  int speedProfile = 1;
  bool FSDEnabled = true;
  int fallbackSpeedOffsetMph = 0;
  UnifiedSpeedCompensationPort unifiedSpeedCompensation{};

  void setFallbackSpeedOffsetMph(int offsetMph) {
    fallbackSpeedOffsetMph = offsetMph;
  }

  void refreshUnifiedSpeedCompensation() {
    unifiedSpeedCompensation.hasFusedSpeedLimit = false;
    if (enableAutoOffsetFromFusedSpeedLimit) {
      int fusedSpeedLimitValue = 0;
      if (speedLimitMonitor.getFusedSpeedLimitValue(fusedSpeedLimitValue)) {
        unifiedSpeedCompensation.hasFusedSpeedLimit = true;
        unifiedSpeedCompensation.fusedSpeedLimitMph = fusedSpeedLimitValue;
        unifiedSpeedCompensation.targetSpeedMph = getTargetSpeedForLimit(fusedSpeedLimitValue);
        int desiredOffsetMph = unifiedSpeedCompensation.targetSpeedMph > fusedSpeedLimitValue
          ? (unifiedSpeedCompensation.targetSpeedMph - fusedSpeedLimitValue)
          : 0;
        unifiedSpeedCompensation.offsetMph = clampOffsetMph(desiredOffsetMph);
        return;
      }
    }
    unifiedSpeedCompensation.offsetMph = clampOffsetMph(fallbackSpeedOffsetMph);
  }

  void handelMessage(can_frame& frame) {
    if (frame.can_id == 1016) {
      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
      switch (followDistance) {
        case 1: speedProfile = 2; break;
        case 2: speedProfile = 1; break;
        case 3: speedProfile = 0; break;
      }
      return;
    }
    if (frame.can_id == 1021) {
      auto index = readMuxID(frame);
      if (index == 0 && FSDEnabled) {
        auto off = (uint8_t)((frame.data[3] >> 1) & 0x3F) - 30;
        setFallbackSpeedOffsetMph(off);
        refreshUnifiedSpeedCompensation();
        setBit(frame, 46, true);
        setSpeedProfileV12V13(frame, speedProfile);
        twai_send(frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        twai_send(frame);
      }
      if (index == 2 && FSDEnabled) {
        uint8_t speedOffsetRaw = enableFixedOffsetRawTest
          ? FIXED_OFFSET_TEST_RAW
          : encodeSpeedOffsetRaw(unifiedSpeedCompensation.offsetMph);
        frame.data[0] &= ~(0b11000000);
        frame.data[1] &= ~(0b00111111);
        frame.data[0] |= (speedOffsetRaw & 0x03) << 6;
        frame.data[1] |= (speedOffsetRaw >> 2);
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
  twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();
}

void loop() {
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
