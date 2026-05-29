/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "memory"
#include <cstring>
#include <SPI.h>
#include <mcp2515.h>

#define HW3 // for what car to compile: HW4, HW3, or LEGACY

#if defined(HW4)
#define HW HW4Handler
#elif defined(HW3)
#define HW HW3Handler
#elif defined(LEGACY)
#define HW LegacyHandler
#endif

bool enablePrint = false;
bool enableSpeedLimitPrint = false;
bool enableAutoOffsetFromFusedSpeedLimit = true;
bool enableFixedOffsetRawTest = false;

#define LED_PIN PIN_LED                
#define CAN_CS PIN_CAN_CS              
#define CAN_INT_PIN PIN_CAN_INTERRUPT  
#define CAN_STBY PIN_CAN_STANDBY       
#define CAN_RESET PIN_CAN_RESET        

#define ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION true
#define ENABLE_ISA_SPEED_CHIME_SUPPRESS false 

constexpr int AUTO_TARGET_SPEED = 60;
constexpr int AUTO_TARGET_SPEED_AT_80 = 80;
constexpr int AUTO_TARGET_SPEED_AT_100 = 100;
constexpr int AUTO_TARGET_SPEED_AT_120 = 120;
constexpr int SPEED_OFFSET_RAW_PER_MPH = 10;
constexpr int MAX_SPEED_OFFSET_MPH = 25;
constexpr uint8_t FIXED_OFFSET_TEST_RAW = 40;

std::unique_ptr<MCP2515> mcp;

constexpr uint32_t CAN_ID_UI_DRIVER_ASSIST_MAP_DATA = 0x238;
constexpr uint32_t CAN_ID_UI_GPS_VEHICLE_SPEED = 0x3D9;
constexpr uint32_t CAN_ID_DAS_STATUS = 0x399;
constexpr uint32_t CAN_ID_DAS_STATUS2 = 0x389;

struct UnifiedSpeedCompensationPort {
  bool hasFusedSpeedLimit = false;
  int fusedSpeedLimitMph = 0;
  int targetSpeedMph = 0;
  int offsetMph = 0;
};

struct CarManagerBase {
  int speedProfile = 1;
  bool FSDEnabled = true;
  int fallbackSpeedOffsetMph = 0;
  UnifiedSpeedCompensationPort unifiedSpeedCompensation{};
  virtual void handelMessage(can_frame& frame);
  void setFallbackSpeedOffsetMph(int offsetMph);
  void refreshUnifiedSpeedCompensation();
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
  if (fusedSpeedLimitValue < 60) return AUTO_TARGET_SPEED;
  if (fusedSpeedLimitValue < 80) return AUTO_TARGET_SPEED_AT_80;
  if (fusedSpeedLimitValue < 100) return AUTO_TARGET_SPEED_AT_100;
  if (fusedSpeedLimitValue < 120) return AUTO_TARGET_SPEED_AT_120;    
  return fusedSpeedLimitValue;
}

inline void CarManagerBase::setFallbackSpeedOffsetMph(int offsetMph) {
  fallbackSpeedOffsetMph = offsetMph;
}

inline void CarManagerBase::refreshUnifiedSpeedCompensation() {
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

struct LegacyHandler : public CarManagerBase {
  virtual void handelMessage(can_frame& frame) override {
    if (frame.can_id == 69) {
      uint8_t pos = frame.data[1] >> 5;
      if (pos <= 1) speedProfile = 2; 
      else if (pos == 2) speedProfile = 1; 
      else speedProfile = 0;  
      return;
    }
    if (frame.can_id == 1006) {
      auto index = readMuxID(frame);
      if (index == 0 && FSDEnabled) {
        setBit(frame, 46, true);
        setSpeedProfileV12V13(frame, speedProfile);
        mcp->sendMessage(&frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        mcp->sendMessage(&frame);
      }
    }
  }
};

struct HW3Handler : public CarManagerBase {
  virtual void handelMessage(can_frame& frame) override {
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
        mcp->sendMessage(&frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        mcp->sendMessage(&frame);
      }
      if (index == 2 && FSDEnabled) {
        uint8_t speedOffsetRaw = enableFixedOffsetRawTest
          ? FIXED_OFFSET_TEST_RAW
          : encodeSpeedOffsetRaw(unifiedSpeedCompensation.offsetMph);
        frame.data[0] &= ~(0b11000000);
        frame.data[1] &= ~(0b00111111);
        frame.data[0] |= (speedOffsetRaw & 0x03) << 6;
        frame.data[1] |= (speedOffsetRaw >> 2);
        mcp->sendMessage(&frame);
      }
    }
  }
};

struct HW4Handler : public CarManagerBase {
  virtual void handelMessage(can_frame& frame) override {
    if (ENABLE_ISA_SPEED_CHIME_SUPPRESS && frame.can_id == 921) {
      frame.data[1] |= 0x20;
      uint8_t sum = 0;
      for (int i = 0; i < 7; i++) sum += frame.data[i];
      sum += (921 & 0xFF) + (921 >> 8);
      frame.data[7] = sum & 0xFF;
      mcp->sendMessage(&frame);
      return;
    }
    if (frame.can_id == 1016) {
      auto fd = (frame.data[5] & 0b11100000) >> 5;
      switch(fd){
        case 1: speedProfile = 3; break;
        case 2: speedProfile = 2; break;
        case 3: speedProfile = 1; break;
        case 4: speedProfile = 0; break;
        case 5: speedProfile = 4; break;
      }
    }
    if (frame.can_id == 1021) {
      auto index = readMuxID(frame);
      if (index == 0 && FSDEnabled) {
        setBit(frame, 46, true);
        setBit(frame, 60, true);
        if (ENABLE_APPROACHING_EMERGENCY_VEHICLE_DETECTION) setBit(frame, 59, true);
        mcp->sendMessage(&frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        setBit(frame, 47, true);
        mcp->sendMessage(&frame);
      }
      if(index == 2){
        frame.data[7] &= ~(0x07 << 4);
        frame.data[7] |= (speedProfile & 0x07) << 4;
        mcp->sendMessage(&frame);
      }
    }
  }
};

std::unique_ptr<CarManagerBase> handler;

void setup() {
  handler = std::make_unique<HW>();
  delay(500); 
  mcp = std::make_unique<MCP2515>(CAN_CS);
  mcp->reset();
  mcp->setBitrate(CAN_500KBPS, MCP_16MHZ);  
  mcp->setNormalMode();
}

__attribute__((optimize("O3"))) void loop() {
  can_frame frame;
  if (mcp->readMessage(&frame) != MCP2515::ERROR_OK) {
    digitalWrite(LED_PIN, HIGH);
    return;
  }
  digitalWrite(LED_PIN, LOW);
  speedLimitMonitor.update(frame);
  handler->refreshUnifiedSpeedCompensation();
  handler->handelMessage(frame);
}
