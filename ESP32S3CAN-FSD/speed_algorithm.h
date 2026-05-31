#pragma once

#include <algorithm>
#include <cstdint>

constexpr int TARGET_SPEED_60_KPH = 60;
constexpr int TARGET_SPEED_80_KPH = 80;
constexpr int TARGET_SPEED_90_KPH = 90;
constexpr int TARGET_SPEED_100_KPH = 100;
constexpr int TARGET_SPEED_120_KPH = 120;
constexpr int TARGET_SPEED_140_KPH = 140;

constexpr int MAX_SPEED_OFFSET_KPH = 25;
constexpr int MAX_SPEED_OFFSET_PCT = 50;
constexpr int OFFSET_PCT4_RAW_PER_PCT = 4;
constexpr uint8_t OFFSET_SLEW_RATE_PCT_PER_SEC = 5;

constexpr uint32_t STARTUP_SPEED_FALLBACK_WINDOW_MS = 15000;
constexpr int STARTUP_SPEED_FALLBACK_TARGET_KPH = 60;
constexpr uint8_t STARTUP_SPEED_FALLBACK_RAW =
  static_cast<uint8_t>(MAX_SPEED_OFFSET_PCT * OFFSET_PCT4_RAW_PER_PCT);

struct SpeedCompensation {
  bool hasFusedSpeedLimit = false;
  int fusedSpeedLimitKph = 0;
  int targetSpeedKph = 0;
  int offsetKph = 0;
  uint8_t speedOffsetRaw = 0;
};

inline bool decodeFusedSpeedLimitKph(uint8_t dasStatusData1, int& limitKph) {
  const uint8_t fusedLimitRaw = dasStatusData1 & 0x1F;
  if (fusedLimitRaw == 0 || fusedLimitRaw == 31) return false;
  limitKph = static_cast<int>(fusedLimitRaw) * 5;
  return true;
}

inline int getTargetSpeedForLimit(int fusedSpeedLimitKph) {
  if (fusedSpeedLimitKph < 60)  return TARGET_SPEED_60_KPH;
  if (fusedSpeedLimitKph < 80)  return TARGET_SPEED_80_KPH;
  if (fusedSpeedLimitKph < 90)  return TARGET_SPEED_90_KPH;
  if (fusedSpeedLimitKph < 100) return TARGET_SPEED_100_KPH;
  if (fusedSpeedLimitKph < 120) return TARGET_SPEED_120_KPH;
  if (fusedSpeedLimitKph < 140) return TARGET_SPEED_140_KPH;
  return fusedSpeedLimitKph;
}

inline int clampOffsetKph(int value) {
  return std::max(std::min(value, MAX_SPEED_OFFSET_KPH), 0);
}

inline uint8_t encodeSpeedOffsetRawPct4(int offsetKph, int fusedSpeedLimitKph) {
  if (fusedSpeedLimitKph <= 0) return 0;
  int pct = (offsetKph * 100 + fusedSpeedLimitKph / 2) / fusedSpeedLimitKph;
  pct = std::max(std::min(pct, MAX_SPEED_OFFSET_PCT), 0);
  return static_cast<uint8_t>(pct * OFFSET_PCT4_RAW_PER_PCT);
}

inline SpeedCompensation calculateSpeedCompensation(int fusedSpeedLimitKph) {
  SpeedCompensation result{};
  if (fusedSpeedLimitKph <= 0) return result;

  result.hasFusedSpeedLimit = true;
  result.fusedSpeedLimitKph = fusedSpeedLimitKph;
  result.targetSpeedKph = getTargetSpeedForLimit(fusedSpeedLimitKph);
  const int desiredOffsetKph = result.targetSpeedKph > fusedSpeedLimitKph
    ? result.targetSpeedKph - fusedSpeedLimitKph
    : 0;
  result.offsetKph = clampOffsetKph(desiredOffsetKph);
  result.speedOffsetRaw = encodeSpeedOffsetRawPct4(result.offsetKph, fusedSpeedLimitKph);
  return result;
}

inline uint8_t readSpeedOffsetRaw(const uint8_t data[8]) {
  return static_cast<uint8_t>(((data[1] & 0x3F) << 2) | ((data[0] >> 6) & 0x03));
}

inline void writeSpeedOffsetRaw(uint8_t data[8], uint8_t raw) {
  data[0] = static_cast<uint8_t>((data[0] & ~0xC0) | ((raw & 0x03) << 6));
  data[1] = static_cast<uint8_t>((data[1] & ~0x3F) | (raw >> 2));
}

struct OffsetSlewLimiter {
  uint8_t lastRaw = 0;
  uint32_t lastSentMs = 0;

  void reset(uint8_t raw = 0, uint32_t nowMs = 0) {
    lastRaw = raw;
    lastSentMs = nowMs;
  }

  uint8_t apply(uint8_t targetRaw, uint32_t nowMs) {
    uint8_t shapedRaw = targetRaw;

    if (targetRaw < lastRaw && lastSentMs != 0) {
      const uint32_t rateRawPerSec =
        static_cast<uint32_t>(OFFSET_SLEW_RATE_PCT_PER_SEC) * OFFSET_PCT4_RAW_PER_PCT;
      const uint32_t elapsedMs = nowMs - lastSentMs;
      const uint32_t maxDrop = (rateRawPerSec * elapsedMs + 500U) / 1000U;
      const uint8_t floorRaw = lastRaw > maxDrop ? static_cast<uint8_t>(lastRaw - maxDrop) : 0;
      if (targetRaw < floorRaw) shapedRaw = floorRaw;
    }

    lastRaw = shapedRaw;
    lastSentMs = nowMs;
    return shapedRaw;
  }
};

struct StartupSpeedFallback {
  bool enabled = true;
  bool hasEverValidFusedSpeedLimit = false;
  bool hasFirstUse = false;
  uint32_t firstUseMs = 0;

  bool markValidFusedSpeedLimit() {
    const bool wasFirstValid = !hasEverValidFusedSpeedLimit;
    hasEverValidFusedSpeedLimit = true;
    enabled = false;
    return wasFirstValid;
  }

  bool shouldUse(uint32_t nowMs) {
    if (!enabled || hasEverValidFusedSpeedLimit) return false;
    if (!hasFirstUse) {
      firstUseMs = nowMs;
      hasFirstUse = true;
    }
    return (nowMs - firstUseMs) <= STARTUP_SPEED_FALLBACK_WINDOW_MS;
  }

  uint8_t selectRaw(uint8_t stockRaw, uint32_t nowMs) {
    if (!shouldUse(nowMs)) return stockRaw;
    return std::max(stockRaw, STARTUP_SPEED_FALLBACK_RAW);
  }
};
