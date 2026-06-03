/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ESP32S3 variant 鈥?uses the built-in TWAI (CAN) peripheral instead of MCP2515.

    Two supported boards (selected via PlatformIO build_flags):
      * Waveshare ESP32-S3-RS485-CAN 鈥?single CAN, TWAI only.
      * LILYGO T-2CAN 鈥?dual CAN:
          - CAN A / CAN1: ESP32-S3 native TWAI (GPIO7/GPIO6) 鈥?FSD activation and
            speed-limit modification. This is the primary, time-critical bus.
          - CAN B / CAN2: MCP2515 over SPI (GPIO12/11/13/10/9) 鈥?second auxiliary
            bus (basic RX/TX/counters + optional 0x339 service-mode burst).

    WiFi/Bluetooth stay powered down unless the light WebUI is compiled in
    (-DENABLE_LIGHT_WEBUI), in which case a SoftAP + tiny parameter page run on
    a dedicated low-priority task so the CAN fast path is never blocked.
*/

#include <algorithm>
#include <cstring>
#include <driver/twai.h>
#ifdef ENABLE_LIGHT_WEBUI
#include <esp_heap_caps.h>
#endif

// ---- CAN B (MCP2515) 鈥?optional secondary bus ----
// The autowp MCP2515 library provides its own `struct can_frame` with the same
// field layout (can_id / can_dlc / data[8]) the handler already expects, so we
// only define our own copy when the library is absent. Defining both in one
// translation unit would be a redefinition error 鈥?hence the #else.
#ifdef ENABLE_CANB_MCP2515
#include <SPI.h>
#include <mcp2515.h>
#else
struct can_frame {
  uint32_t can_id;
  uint8_t  can_dlc;
  uint8_t  data[8];
};
#endif

// ---- Light WebUI 鈥?optional ----
#ifdef ENABLE_LIGHT_WEBUI
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#endif

// This build supports the HW3 car only.

// Pin assignments 鈥?overridable via PlatformIO build_flags (-D...).
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

// TWAI RX/TX queue depths 鈥?overridable via build_flags.
#ifndef TWAI_RX_QUEUE_LEN
#define TWAI_RX_QUEUE_LEN 64
#endif
#ifndef TWAI_TX_QUEUE_LEN
#define TWAI_TX_QUEUE_LEN 16
#endif

// ---- CAN B (MCP2515) pin/clock fallbacks 鈥?overridable via build_flags ----
#ifdef ENABLE_CANB_MCP2515
#ifndef MCP2515_SCK
#define MCP2515_SCK 12
#endif
#ifndef MCP2515_MOSI
#define MCP2515_MOSI 11
#endif
#ifndef MCP2515_MISO
#define MCP2515_MISO 13
#endif
#ifndef MCP2515_CS
#define MCP2515_CS 10
#endif
#ifndef MCP2515_RST
#define MCP2515_RST 9
#endif
#ifndef MCP2515_CLOCK
#define MCP2515_CLOCK MCP_16MHZ
#endif
// SocketCAN-style id flags (defined by the autowp can.h; guarded for safety).
#ifndef CAN_EFF_FLAG
#define CAN_EFF_FLAG 0x80000000UL
#endif
#ifndef CAN_RTR_FLAG
#define CAN_RTR_FLAG 0x40000000UL
#endif
#ifndef CAN_SFF_MASK
#define CAN_SFF_MASK 0x000007FFUL
#endif
#endif  // ENABLE_CANB_MCP2515
#ifndef CAN_SFF_MASK
#define CAN_SFF_MASK 0x000007FFUL
#endif

// ---- Speed-offset encoding constants (wire format, not user-tunable) ----
constexpr int LOW_SPEED_MAX_PCT_LIMIT_KPH = 50;
constexpr int MAX_SPEED_OFFSET_KPH = 25;     // absolute pre-clamp on the computed offset
constexpr int MAX_SPEED_OFFSET_PCT = 50;     // PCT4 wire cap (matches dev kHw3SpeedOffsetMaxPct)
constexpr int OFFSET_PCT4_RAW_PER_PCT = 4;

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

// ---- Runtime configuration (WebUI-tunable; defaults match the legacy constants) ----
// CAN A reads this every relevant frame, so updates must stay cheap. The legacy
// firmware used plain constexpr values; the defaults below are byte-for-byte
// equivalent, so behaviour with no WebUI is unchanged.
struct RuntimeConfig {
  bool fsdEnabled = true;
  bool autoSpeedOffsetEnabled = true;
  uint8_t slewPctPerSec = 5;
  uint8_t lowSpeedMaxPctRaw = 200;    // = MAX_SPEED_OFFSET_PCT * OFFSET_PCT4_RAW_PER_PCT

  uint16_t targetBelow60 = 60;
  uint16_t target60 = 80;
  uint16_t target70 = 85;
  uint16_t target80 = 90;
  uint16_t target90 = 100;
  uint16_t target100 = 120;
  uint16_t target120 = 140;

  bool canbEnabled = true;
  bool canbServiceModeEnabled = false;
  bool canbFilterEnabled = false;     // OFF = all standard frames; ON = feature IDs only
  bool highBeamStrobeEnabled = false; // arms double-pull flash-to-pass trigger
  bool rearFogBrakeStrobeEnabled = false; // arms brake-triggered 0x273 rear fog burst
};

static RuntimeConfig g_config;

// ---- Runtime status snapshot (CAN fast path writes, WebUI reads) ----
struct RuntimeStatus {
  uint32_t can1Rx = 0;
  uint32_t can1Tx = 0;
  uint32_t can1TxFail = 0;
  uint32_t twaiBusOffCount = 0;

  uint32_t canbRx = 0;
  uint32_t canbTx = 0;
  uint32_t canbTxFail = 0;
  uint32_t canbLastId = 0;
  uint8_t highBeamStrobeActive = 0;
  uint8_t highBeamStrobeRemaining = 0;
  uint8_t rearFogBrakeStrobeActive = 0;
  uint8_t rearFogBrakeStrobeRemaining = 0;

  int fusedLimitKph = 0;
  int targetSpeedKph = 0;
  int offsetKph = 0;
  uint8_t offsetRaw = 0;
};

static RuntimeStatus g_status;

// Config is shared between the CAN core and the WebUI task. A short spinlock
// guards writes; the CAN path takes a one-shot consistent copy per frame. With
// no WebUI compiled in there are no writers, so we skip the lock entirely.
#ifdef ENABLE_LIGHT_WEBUI
static portMUX_TYPE g_cfgMux = portMUX_INITIALIZER_UNLOCKED;
#endif

static inline RuntimeConfig configSnapshot() {
#ifdef ENABLE_LIGHT_WEBUI
  portENTER_CRITICAL(&g_cfgMux);
  RuntimeConfig c = g_config;
  portEXIT_CRITICAL(&g_cfgMux);
  return c;
#else
  return g_config;
#endif
}

#ifdef ENABLE_LIGHT_WEBUI
#ifndef REC_CAP
#define REC_CAP 4000
#endif
static constexpr uint32_t REC_TARGET_CAP = REC_CAP;
static constexpr uint32_t REC_MAX_DURATION_MS = 60000UL;
static constexpr uint8_t REC_FILTER_MAX = 16;

struct RecFrame {
  uint32_t ts;
  char dir;
  uint8_t bus;
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
};

static portMUX_TYPE g_recMux = portMUX_INITIALIZER_UNLOCKED;
static RecFrame* recBuf = nullptr;
static uint32_t recCapacity = 0;
static size_t recBufferBytes = 0;
static bool recPsramReady = false;
static volatile bool recActive = false;
static volatile uint32_t recCount = 0;
static volatile bool recSaved = false;
static uint32_t recStartMs = 0;
static uint32_t recFilterIds[REC_FILTER_MAX];
static uint8_t recFilterCount = 0;
static uint32_t recExcludeIds[REC_FILTER_MAX];
static uint8_t recExcludeCount = 0;

static bool recIdInList(uint32_t id, const uint32_t* ids, uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (ids[i] == id) return true;
  }
  return false;
}

static bool recFramePassesFilter(uint32_t id) {
  if (recFilterCount > 0 && !recIdInList(id, recFilterIds, recFilterCount)) return false;
  if (recExcludeCount > 0 && recIdInList(id, recExcludeIds, recExcludeCount)) return false;
  return true;
}

static void setupRecorderBuffer() {
  recPsramReady = psramInit();
  if (!recPsramReady) return;

  recBufferBytes = static_cast<size_t>(REC_TARGET_CAP) * sizeof(RecFrame);
  recBuf = static_cast<RecFrame*>(heap_caps_malloc(recBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!recBuf) {
    recBufferBytes = 0;
    recCapacity = 0;
    return;
  }
  recCapacity = REC_TARGET_CAP;
}

static void recordCanFrame(const can_frame& frame, char dir, uint8_t bus) {
  if (!recActive) return;
  if (!recBuf || recCapacity == 0) return;
  if (millis() - recStartMs >= REC_MAX_DURATION_MS) {
    portENTER_CRITICAL(&g_recMux);
    recActive = false;
    recSaved = true;
    portEXIT_CRITICAL(&g_recMux);
    return;
  }
  const uint32_t id = frame.can_id & CAN_SFF_MASK;
  if (!recFramePassesFilter(id)) return;

  portENTER_CRITICAL(&g_recMux);
  if (!recActive) {
    portEXIT_CRITICAL(&g_recMux);
    return;
  }
  uint32_t idx = recCount;
  if (idx >= recCapacity) {
    recActive = false;
    recSaved = true;
    portEXIT_CRITICAL(&g_recMux);
    return;
  }
  RecFrame& r = recBuf[idx];
  r.ts = millis();
  r.dir = dir;
  r.bus = bus;
  r.id = id;
  r.dlc = frame.can_dlc <= 8 ? frame.can_dlc : 8;
  memset(r.data, 0, sizeof(r.data));
  memcpy(r.data, frame.data, r.dlc);
  recCount = idx + 1;
  if (recCount >= recCapacity) {
    recActive = false;
    recSaved = true;
  }
  portEXIT_CRITICAL(&g_recMux);
}
#else
static inline void recordCanFrame(const can_frame&, char, uint8_t) {}
#endif

// ---- TWAI helpers ----

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
    if (twaiRecoveryInProgress) {
      g_status.can1TxFail++;
      return false;
    }

    if (twai_transmit(&msg, pdMS_TO_TICKS(TWAI_TX_WAIT_MS)) == ESP_OK) {
      g_status.can1Tx++;
      recordCanFrame(frame, 'T', 1);
      return true;
    }

    serviceTwaiAlerts();
    if (attempt < TWAI_TX_RETRY_COUNT) delay(TWAI_TX_RETRY_DELAY_MS);
  }
  g_status.can1TxFail++;
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
    g_status.can1Rx++;
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
        g_status.twaiBusOffCount++;
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
      g_status.twaiBusOffCount++;
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
constexpr uint32_t CAN_ID_BRAKE_PEDAL = 0x145;
constexpr uint32_t CAN_ID_RCM_INERTIAL2_CH = 0x111;
constexpr uint32_t CAN_ID_RCM_INERTIAL2_ETH = 0x116;
constexpr uint32_t CAN_ID_DI_SYSTEM_STATUS = 0x118;
constexpr uint32_t CAN_ID_DI_CHASSIS_CONTROL = 0x148;
constexpr uint32_t CAN_ID_ESP_BRAKE_TORQUE = 0x185;
constexpr uint32_t CAN_ID_DIF_TORQUE = 0x186;
constexpr uint32_t CAN_ID_VEHICLE_SPEED = 0x257;

static inline bool isRelevantCanId(uint32_t canId) {
  return canId == CAN_ID_BRAKE_PEDAL ||
         canId == CAN_ID_RCM_INERTIAL2_CH ||
         canId == CAN_ID_RCM_INERTIAL2_ETH ||
         canId == CAN_ID_DI_SYSTEM_STATUS ||
         canId == CAN_ID_DI_CHASSIS_CONTROL ||
         canId == CAN_ID_ESP_BRAKE_TORQUE ||
         canId == CAN_ID_DIF_TORQUE ||
         canId == CAN_ID_VEHICLE_SPEED ||
         canId == CAN_ID_DAS_STATUS ||
         canId == CAN_ID_FOLLOW_DISTANCE ||
         canId == CAN_ID_AP_CONTROL;
}

// CAN A 璋冭瘯瑕佺偣锛?// TWAI 纭欢杩囨护鍙仛鈥滅矖杩囨护鈥濓紝鍑忓皯鎬荤嚎甯ц繘鍏?RX 闃熷垪鐨勬暟閲忥紱
// 鐪熸鍙備笌涓氬姟閫昏緫鐨勪粛鐒跺彧鍏佽 0x399銆?x3F8銆?x3FD 涓変釜 ID銆?// Hardware acceptance filter 鈥?a coarse pre-filter so the controller only
// enqueues the IDs near our targets instead of the whole bus; isRelevantCanId()
// still does the exact ID match. Derived from the IDs we touch:
//   0x145, 0x399, 0x3F8, 0x3FD.
// Adding 0x145 widens this coarse filter; software filtering above still drops
// every unrelated ID. Standard 11-bit IDs sit in bits [31:21];
// in twai_filter_config_t a mask bit of 1 means "don't care".
constexpr uint32_t CAN_ACCEPT_CODE = 0;
constexpr uint32_t CAN_ACCEPT_MASK = 0xFFFFFFFF;

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
    // 0x399 淇濆瓨鏈€杩戜竴娆?DAS 鐘舵€佸抚锛涢€熷害鍋忕Щ璁＄畻鍙缂撳瓨锛屼笉鍦?CAN 蹇矾寰勯噷鍋氶噸娲汇€?    if (frame.can_id == CAN_ID_DAS_STATUS && frame.can_dlc >= 2) {
      dasStatusFrame = frame;
      hasDasStatusFrame = true;
    }
  }

  bool getFusedSpeedLimitValue(int& limitValue) const {
    if (!hasDasStatusFrame || dasStatusFrame.can_dlc < 2) return false;
    const uint64_t fusedLimitRaw = (static_cast<uint64_t>(dasStatusFrame.data[1]) & 0x1F);
    // 浣?5 bit 涓鸿瀺鍚堥檺閫熺储寮曪紝鍗曚綅 5 km/h锛? 鍜?31 瑙嗕负鏃犳晥/鏈煡銆?    const uint64_t fusedLimitRaw = (static_cast<uint64_t>(dasStatusFrame.data[1]) & 0x1F);
    if (fusedLimitRaw == 0 || fusedLimitRaw == 31) return false;
    limitValue = static_cast<int>(fusedLimitRaw * 5ULL);
    return true;
  }
};

SpeedLimitMonitor speedLimitMonitor;

#ifdef ENABLE_CANB_MCP2515
static void handleRearFogPedalBrakeEdge(bool brakeActive, const RuntimeConfig& cfg);
static void handleRearFogBrakeLampState(bool brakeActive, const RuntimeConfig& cfg);
static void handleRearFogCanADecelFrame(const can_frame& frame, const RuntimeConfig& cfg);
#endif

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

inline int clampOffsetKph(int value) { return std::max(std::min(value, MAX_SPEED_OFFSET_KPH), 0); }

inline uint8_t encodeSpeedOffsetRawPct4(int offsetKph, int fusedSpeedLimitKph) {
  if (fusedSpeedLimitKph <= 0) return 0;
  // 閫熷害鍋忕Щ鍦?CAN 绾夸笂鎸?PCT4 缂栫爜锛?% = 4 raw銆?  if (fusedSpeedLimitKph <= 0) return 0;
  int pct = (offsetKph * 100 + fusedSpeedLimitKph / 2) / fusedSpeedLimitKph;
  pct = std::max(std::min(pct, MAX_SPEED_OFFSET_PCT), 0);
  return static_cast<uint8_t>(pct * OFFSET_PCT4_RAW_PER_PCT);
}

inline int getTargetSpeedForLimit(int fusedSpeedLimitValue, const RuntimeConfig& cfg) {
  if (fusedSpeedLimitValue < 60)  return cfg.targetBelow60;
  if (fusedSpeedLimitValue < 70)  return cfg.target60;
  if (fusedSpeedLimitValue < 80)  return cfg.target70;
  if (fusedSpeedLimitValue < 90)  return cfg.target80;
  if (fusedSpeedLimitValue < 100) return cfg.target90;
  if (fusedSpeedLimitValue < 120) return cfg.target100;
  if (fusedSpeedLimitValue < 140) return cfg.target120;
  return fusedSpeedLimitValue;
}

inline uint8_t readSpeedOffsetRaw(const can_frame& frame) {
  return static_cast<uint8_t>(((frame.data[1] & 0x3F) << 2) | ((frame.data[0] >> 6) & 0x03));
}

inline void writeSpeedOffsetRaw(can_frame& frame, uint8_t raw) {
  frame.data[0] = static_cast<uint8_t>((frame.data[0] & ~0xC0) | ((raw & 0x03) << 6));
  // 0x3FD mux 2 鐨?offset raw 璺?data[0] bit6-7 鍜?data[1] bit0-5銆?  frame.data[0] = static_cast<uint8_t>((frame.data[0] & ~0xC0) | ((raw & 0x03) << 6));
  frame.data[1] = static_cast<uint8_t>((frame.data[1] & ~0x3F) | (raw >> 2));
}

struct OffsetSlewLimiter {
  uint8_t lastRaw = 0;
  uint32_t lastSentMs = 0;

  uint8_t apply(uint8_t targetRaw, uint8_t slewPctPerSec) {
    uint8_t shapedRaw = targetRaw;
    const uint32_t now = millis();

    if (targetRaw < lastRaw && lastSentMs != 0) {
      const uint32_t rateRawPerSec =
        static_cast<uint32_t>(slewPctPerSec) * OFFSET_PCT4_RAW_PER_PCT;
      // 鍙檺鍒垛€滀笅闄嶉€熷害鈥濓細闄愰€熺獊鐒堕檷浣庢椂骞虫粦鍥炶惤锛岄伩鍏嶇洰鏍囬€熷害鐬棿璺冲彉銆?      const uint32_t rateRawPerSec =
        static_cast<uint32_t>(slewPctPerSec) * OFFSET_PCT4_RAW_PER_PCT;
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

// ---- HW3 car handler ----

struct HW3Handler {
  int speedProfile = 1;
  UnifiedSpeedCompensationPort unifiedSpeedCompensation{};
  OffsetSlewLimiter offsetSlewLimiter{};

  void refreshUnifiedSpeedCompensation(const RuntimeConfig& cfg) {
    unifiedSpeedCompensation.hasFusedSpeedLimit = false;
    // 姣忚疆鏀跺埌鐩稿叧 CAN A 甯у悗鍒锋柊涓€娆＄紦瀛樼姸鎬侊紝鍚庣画 mux 2 鐩存帴鍐欏叆璁＄畻濂界殑 offset銆?    unifiedSpeedCompensation.hasFusedSpeedLimit = false;
    unifiedSpeedCompensation.fusedSpeedLimitKph = 0;
    unifiedSpeedCompensation.targetSpeedKph = 0;
    unifiedSpeedCompensation.offsetKph = 0;
    unifiedSpeedCompensation.speedOffsetRaw = 0;
    if (cfg.autoSpeedOffsetEnabled) {
      int fusedSpeedLimitValue = 0;
      if (speedLimitMonitor.getFusedSpeedLimitValue(fusedSpeedLimitValue)) {
        unifiedSpeedCompensation.hasFusedSpeedLimit = true;
        unifiedSpeedCompensation.fusedSpeedLimitKph = fusedSpeedLimitValue;
        unifiedSpeedCompensation.targetSpeedKph = getTargetSpeedForLimit(fusedSpeedLimitValue, cfg);
        int desiredOffsetKph = unifiedSpeedCompensation.targetSpeedKph > fusedSpeedLimitValue
          ? (unifiedSpeedCompensation.targetSpeedKph - fusedSpeedLimitValue)
          : 0;
        // 鐩爣閫熷害琛ㄧ敱 WebUI 閰嶇疆锛涙渶缁?offset 浠嶅彈 MAX_SPEED_OFFSET_KPH/PCT 涓婇檺淇濇姢銆?        int desiredOffsetKph = unifiedSpeedCompensation.targetSpeedKph > fusedSpeedLimitValue
        unifiedSpeedCompensation.offsetKph = clampOffsetKph(desiredOffsetKph);
        unifiedSpeedCompensation.speedOffsetRaw =
          fusedSpeedLimitValue < LOW_SPEED_MAX_PCT_LIMIT_KPH
            ? cfg.lowSpeedMaxPctRaw
            : encodeSpeedOffsetRawPct4(
                unifiedSpeedCompensation.offsetKph,
                unifiedSpeedCompensation.fusedSpeedLimitKph);
      }
    }

    g_status.fusedLimitKph = unifiedSpeedCompensation.fusedSpeedLimitKph;
    g_status.targetSpeedKph = unifiedSpeedCompensation.targetSpeedKph;
    g_status.offsetKph = unifiedSpeedCompensation.offsetKph;
  }

  void handelMessage(can_frame& frame, const RuntimeConfig& cfg) {
#ifdef ENABLE_CANB_MCP2515
    handleRearFogCanADecelFrame(frame, cfg);
#endif
    if (frame.can_id == CAN_ID_BRAKE_PEDAL) {
      if (frame.can_dlc < 4) return;
      return;
    }

    if (frame.can_id == CAN_ID_FOLLOW_DISTANCE) {
      if (frame.can_dlc < 6) return;
      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
      // 0x3F8 璺熻溅璺濈鎷ㄦ潌澶嶇敤涓洪┚椹堕鏍硷細鏁板€艰秺灏忚秺婵€杩涖€?      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
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
      if (index == 0 && cfg.fsdEnabled) {
        setBit(frame, 46, true);
        // 0x3FD mux 0锛氬紑鍚?FSD/AP 鐩稿叧 bit锛屽苟鍐欏叆褰撳墠椹鹃┒椋庢牸銆?        setBit(frame, 46, true);
        setSpeedProfileV12V13(frame, speedProfile);
        twai_send(frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        // 0x3FD mux 1锛氭竻 bit 19锛屾部鐢ㄥ師鍒嗘敮鐨?AP/FSD 婵€娲昏緟鍔╅€昏緫銆?        setBit(frame, 19, false);
        twai_send(frame);
      }
      if (index == 2 && cfg.fsdEnabled) {
        uint8_t speedOffsetRaw = unifiedSpeedCompensation.hasFusedSpeedLimit
          ? unifiedSpeedCompensation.speedOffsetRaw
          : readSpeedOffsetRaw(frame);
        // 0x3FD mux 2锛氬啓鍏ラ€熷害鍋忕Щ銆傝嫢娌℃湁鏈夋晥闄愰€燂紝鍒欎繚鐣欏師杞﹀綋鍓?offset銆?        uint8_t speedOffsetRaw = unifiedSpeedCompensation.hasFusedSpeedLimit
        speedOffsetRaw = offsetSlewLimiter.apply(speedOffsetRaw, cfg.slewPctPerSec);
        g_status.offsetRaw = speedOffsetRaw;
        writeSpeedOffsetRaw(frame, speedOffsetRaw);
        twai_send(frame);
      }
    }
  }
};

HW3Handler handler;

// ============================================================================
// CAN B (MCP2515) secondary bus 鈥?basic comms + non-blocking service-mode burst
// ============================================================================
#ifdef ENABLE_CANB_MCP2515

static MCP2515 canb(MCP2515_CS);
static bool canbReady = false;
static bool canbHardwareFilterEnabled = false;
static uint32_t canbRxCount = 0;
static uint32_t canbTxCount = 0;
static uint32_t canbTxFailCount = 0;
static uint32_t canbLastId = 0;

// 0x339 VCSEC service-mode burst state (RAM-only, OFF on boot). Each toggle
// queues 4 frames at 10ms spacing, scheduled with millis() 鈥?never delay().
// volatile: setCanBServiceMode() may run on the WebUI task (core 0) while
// serviceCanBScheduledTx() runs on the CAN loop (core 1).
static volatile bool canbServiceModeActive = false;
static volatile uint8_t canbServiceBurstRemaining = 0;
static volatile uint8_t canbServiceBurstByte5 = 0;
static volatile uint32_t canbLastServiceBurstMs = 0;

// CAN B feature IDs:
//   0x249: SCCMLeftStalk command. status=1 PULL triggers high-light strobe;
//          injected strobe uses only status=1 PULL and status=0 idle.
//   0x273: body lighting frame used for brake/fog context and rear-fog strobe.
//   0x3F5: lighting feedback frame; accepted by the hardware filter for logging.
constexpr uint32_t CANB_ID_STW_ACTN_RQ = 0x249;
constexpr uint32_t CANB_ID_BODY_LIGHTING = 0x273;
constexpr uint32_t CANB_ID_LIGHTING_STATUS = 0x3F5;
constexpr uint8_t HIGH_BEAM_STROBE_PULSES = 8;
constexpr uint8_t REAR_FOG_PEDAL_STROBE_PULSES = 3;
constexpr uint8_t REAR_FOG_BODY_STROBE_PULSES = 6;
constexpr uint8_t REAR_FOG_PRIORITY_PEDAL = 1;
constexpr uint8_t REAR_FOG_PRIORITY_BODY = 2;
constexpr uint16_t HIGH_BEAM_STROBE_INTERVAL_MS = 75;
constexpr uint16_t HIGH_BEAM_STROBE_RESEND_MS = 45;
constexpr uint16_t REAR_FOG_STROBE_INTERVAL_MS = 135;
constexpr uint16_t REAR_FOG_MILD_DECEL_HOLD_MS = 300;
constexpr uint16_t REAR_FOG_HARD_DECEL_HOLD_MS = 150;
constexpr uint16_t REAR_FOG_DECEL_RECENT_MS = 800;
constexpr float REAR_FOG_MILD_DECEL_THRESHOLD = -0.80f;
constexpr float REAR_FOG_HARD_DECEL_THRESHOLD = -2.50f;
constexpr float REAR_FOG_VERY_HARD_DECEL_THRESHOLD = -3.50f;
constexpr uint16_t HIGH_BEAM_DOUBLE_PULL_WINDOW_MS = 1200;
constexpr uint8_t STALK_STATUS_IDLE = 0;
constexpr uint8_t STALK_STATUS_PULL = 1;
constexpr uint8_t REAR_FOG_MASK = 0x80;
constexpr uint8_t REAR_FOG_OFF = 0x10;
constexpr uint8_t REAR_FOG_ON = 0x90;
static can_frame canbLastStwActnRqFrame{};
static bool canbHasLastStwActnRqFrame = false;
static can_frame canbLastBodyLightingFrame{};
static bool canbHasLastBodyLightingFrame = false;
static volatile uint8_t highBeamStalkLastCounter = 0;

static volatile bool highBeamStrobeActive = false;
// Non-blocking light-effect state. loop() only emits frames when millis() reaches
// the next phase edge, so CAN A / FSD activation never waits on a delay().
static volatile bool highBeamStrobeManualTrigger = false;
static volatile bool highBeamStrobeOutputOn = false;
static volatile uint8_t highBeamStrobePulsesRemaining = 0;
static volatile uint32_t highBeamStrobeLastToggleMs = 0;
static volatile uint32_t highBeamStrobeLastSendMs = 0;
static bool highBeamLastPullDown = false;
static uint8_t highBeamPullCount = 0;
static uint32_t highBeamLastPullMs = 0;
static volatile bool rearFogBrakeStrobeActive = false;
static volatile bool rearFogBrakeStrobeManualTrigger = false;
static volatile bool rearFogBrakeStrobeOutputOn = false;
static volatile uint8_t rearFogBrakeStrobePulsesRemaining = 0;
static volatile uint8_t rearFogBrakeStrobePriority = 0;
static volatile uint32_t rearFogBrakeStrobeLastToggleMs = 0;
static bool rearFogLastPedalBrakeActive = false;
static bool rearFogLastBrakeActive = false;
static uint32_t rearFogRecentPedalBrakeUntilMs = 0;
static uint32_t rearFogRecentBrakeLampUntilMs = 0;
static uint32_t rearFogRecentBrakeTorqueUntilMs = 0;
static uint32_t rearFogRecentRegenUntilMs = 0;
static uint32_t rearFogRecentNegTorqueUntilMs = 0;
static uint32_t rearFogRecentSpeedFallingUntilMs = 0;
static uint32_t rearFogMildDecelStartMs = 0;
static uint32_t rearFogHardDecelStartMs = 0;
static bool rearFogMildDecelTriggered = false;
static bool rearFogHardDecelTriggered = false;
static float rearFogLastVehicleSpeedKph = -1.0f;

// CAN B read budget per loop pass 鈥?bounded so it can never starve CAN A.
constexpr uint8_t CANB_RX_SCAN_LIMIT = 4;

static void setupCanB();
static bool applyCanBFilters(bool enabled);
static bool canb_recv(can_frame& frame);
static bool canb_send(const can_frame& frame);
static void drainCanBWithBudget();
static void handleCanBFrame(const can_frame& frame);
static void setCanBServiceMode(bool enabled);
static void serviceCanBScheduledTx();
static void serviceHighBeamStrobe(const RuntimeConfig& cfg);
static void serviceRearFogBrakeStrobe(const RuntimeConfig& cfg);

static void setupCanB() {
  canbReady = false;

  // Hard reset the MCP2515 via its RST line: high / low / high.
  pinMode(MCP2515_RST, OUTPUT);
  digitalWrite(MCP2515_RST, HIGH);
  delay(10);
  digitalWrite(MCP2515_RST, LOW);
  delay(10);
  digitalWrite(MCP2515_RST, HIGH);
  delay(10);

  SPI.begin(MCP2515_SCK, MCP2515_MISO, MCP2515_MOSI, MCP2515_CS);

  // reset()/setNormalMode() return types vary across library versions, so we
  // call them as statements and only gate on setBitrate(), which is the
  // meaningful failure point. A failure here just leaves canbReady = false; it
  // must never block or disturb CAN A / the FSD pipeline.
  canb.reset();
  delay(10);
  if (canb.setBitrate(CAN_500KBPS, MCP2515_CLOCK) != MCP2515::ERROR_OK) return;
  if (!applyCanBFilters(configSnapshot().canbFilterEnabled)) return;
  canbReady = true;
}

static bool applyCanBFilters(bool enabled) {
  if (enabled) {
    if (canb.setFilterMask(MCP2515::MASK0, false, 0x7FF) != MCP2515::ERROR_OK) return false;
    // WebUI 寮€鍚繃婊わ細MCP2515 鍙帴鏀跺綋鍓嶅姛鑳界浉鍏虫爣鍑嗗抚锛岄檷浣?CAN B 澶勭悊鍘嬪姏銆?    if (canb.setFilterMask(MCP2515::MASK0, false, 0x7FF) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF0, false, CANB_ID_STW_ACTN_RQ) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF1, false, CANB_ID_BODY_LIGHTING) != MCP2515::ERROR_OK) return false;

    if (canb.setFilterMask(MCP2515::MASK1, false, 0x7FF) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF2, false, CANB_ID_LIGHTING_STATUS) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF3, false, CANB_ID_LIGHTING_STATUS) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF4, false, CANB_ID_LIGHTING_STATUS) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF5, false, CANB_ID_LIGHTING_STATUS) != MCP2515::ERROR_OK) return false;
  } else {
    if (canb.setFilterMask(MCP2515::MASK0, false, 0x000) != MCP2515::ERROR_OK) return false;
    // WebUI 鍏抽棴杩囨护锛歮ask=0 鎺ユ敹鍏ㄩ儴鏍囧噯甯э紝鏂逛究瀹炶溅鎶撳寘/璋冭瘯鏂?ID銆?    if (canb.setFilterMask(MCP2515::MASK0, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF0, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF1, false, 0x000) != MCP2515::ERROR_OK) return false;

    if (canb.setFilterMask(MCP2515::MASK1, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF2, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF3, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF4, false, 0x000) != MCP2515::ERROR_OK) return false;
    if (canb.setFilter(MCP2515::RXF5, false, 0x000) != MCP2515::ERROR_OK) return false;
  }

  if (canb.setNormalMode() != MCP2515::ERROR_OK) return false;
  canbHardwareFilterEnabled = enabled;
  return true;
}

static bool canb_recv(can_frame& frame) {
  if (!canbReady) return false;
  if (canb.readMessage(&frame) != MCP2515::ERROR_OK) return false;

  // Stage 1: ignore extended and remote frames; clamp DLC defensively.
  if (frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG)) return false;
  frame.can_id &= CAN_SFF_MASK;
  if (frame.can_dlc > 8) frame.can_dlc = 8;

  canbRxCount++;
  canbLastId = frame.can_id;
  g_status.canbRx = canbRxCount;
  g_status.canbLastId = canbLastId;
  recordCanFrame(frame, 'R', 2);
  return true;
}

static bool canb_send(const can_frame& frame) {
  if (!canbReady) return false;
  if (frame.can_dlc > 8) return false;

  // One short retry on a busy/failed mailbox; no blocking delay so a stuck
  // CAN B can never stall CAN A.
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    if (canb.sendMessage(&frame) == MCP2515::ERROR_OK) {
      canbTxCount++;
      g_status.canbTx = canbTxCount;
      recordCanFrame(frame, 'T', 2);
      return true;
    }
  }
  canbTxFailCount++;
  g_status.canbTxFail = canbTxFailCount;
  return false;
}

static uint8_t readStalkStatus(const can_frame& frame) {
  if (frame.can_dlc < 2) return STALK_STATUS_IDLE;
  return static_cast<uint8_t>((frame.data[1] >> 4) & 0x03);
}

static uint8_t stalkCrc249(uint8_t counter, uint8_t status) {
  static const uint8_t base[16] = {
    0x9B, 0xE8, 0x2A, 0xD3, 0xD3, 0x83, 0x4C, 0x5E,
    0x3F, 0x5E, 0xE2, 0x28, 0x3A, 0x13, 0xAF, 0xCE
  };
  static const uint8_t offset[8] = {0x00, 0x76, 0xEC, 0x00, 0xF7, 0x00, 0x00, 0x00};
  return static_cast<uint8_t>(base[counter & 0x0F] ^ offset[status & 0x07]);
}

static can_frame highBeamFrame(uint8_t status) {
  can_frame f = {};
  f.can_id = CANB_ID_STW_ACTN_RQ;
  f.can_dlc = 4;
  const uint8_t counter = static_cast<uint8_t>((highBeamStalkLastCounter + 1) & 0x0F);
  f.data[0] = stalkCrc249(counter, status);
  f.data[1] = static_cast<uint8_t>(((status & 0x07) << 4) | counter);
  highBeamStalkLastCounter = counter;
  return f;
}

static uint8_t teslaCanChecksum(uint16_t canId, const uint8_t* data, uint8_t len) {
  uint8_t checksum = static_cast<uint8_t>((canId & 0xFF) + ((canId >> 8) & 0xFF));
  for (uint8_t i = 0; i + 1 < len; ++i) checksum += data[i];
  return checksum;
}

static can_frame rearFogFrame(bool fogOn) {
  can_frame f = {};
  static const uint8_t baseData[8] = {0x81, 0xE1, 0x10, 0x40, 0x0B, 0x03, 0x30, 0x12};
  if (canbHasLastBodyLightingFrame) {
    f = canbLastBodyLightingFrame;
  } else {
    f.can_id = CANB_ID_BODY_LIGHTING;
    f.can_dlc = 8;
    memcpy(f.data, baseData, sizeof(f.data));
  }
  f.can_id = CANB_ID_BODY_LIGHTING;
  if (f.can_dlc < 8) f.can_dlc = 8;
  f.data[2] = fogOn
    ? static_cast<uint8_t>(f.data[2] | REAR_FOG_MASK)
    : static_cast<uint8_t>(f.data[2] & ~REAR_FOG_MASK);
  return f;
}

static void stopHighBeamStrobe(bool sendIdle) {
  if (sendIdle && canbReady) {
    canb_send(highBeamFrame(STALK_STATUS_IDLE));
  }
  highBeamStrobeActive = false;
  highBeamStrobeManualTrigger = false;
  highBeamStrobeOutputOn = false;
  highBeamStrobePulsesRemaining = 0;
  highBeamStrobeLastToggleMs = 0;
  highBeamStrobeLastSendMs = 0;
  g_status.highBeamStrobeActive = 0;
  g_status.highBeamStrobeRemaining = 0;
}

static void startHighBeamStrobe(bool manualTrigger = false) {
  highBeamStrobeActive = true;
  highBeamStrobeManualTrigger = manualTrigger;
  highBeamStrobeOutputOn = false;
  highBeamStrobePulsesRemaining = HIGH_BEAM_STROBE_PULSES;
  highBeamStrobeLastToggleMs = 0;
  highBeamStrobeLastSendMs = 0;
  g_status.highBeamStrobeActive = 1;
  g_status.highBeamStrobeRemaining = HIGH_BEAM_STROBE_PULSES;
}

static void stopRearFogBrakeStrobe(bool sendOff) {
  if (sendOff && canbReady) {
    canb_send(rearFogFrame(false));
  }
  rearFogBrakeStrobeActive = false;
  rearFogBrakeStrobeManualTrigger = false;
  rearFogBrakeStrobeOutputOn = false;
  rearFogBrakeStrobePulsesRemaining = 0;
  rearFogBrakeStrobePriority = 0;
  rearFogBrakeStrobeLastToggleMs = 0;
  g_status.rearFogBrakeStrobeActive = 0;
  g_status.rearFogBrakeStrobeRemaining = 0;
}

static void startRearFogBrakeStrobe(uint8_t pulses, uint8_t priority, bool manualTrigger = false) {
  if (rearFogBrakeStrobeActive && priority < rearFogBrakeStrobePriority) return;
  rearFogBrakeStrobeActive = true;
  rearFogBrakeStrobeManualTrigger = manualTrigger;
  rearFogBrakeStrobeOutputOn = false;
  rearFogBrakeStrobePulsesRemaining = pulses;
  rearFogBrakeStrobePriority = priority;
  rearFogBrakeStrobeLastToggleMs = 0;
  g_status.rearFogBrakeStrobeActive = 1;
  g_status.rearFogBrakeStrobeRemaining = pulses;
}

static bool readBitsLE(const can_frame& frame, uint8_t startBit, uint8_t length, uint32_t& value) {
  if (length == 0 || length > 32) return false;
  if (static_cast<uint16_t>(startBit) + length > static_cast<uint16_t>(frame.can_dlc) * 8U) return false;
  uint32_t raw = 0;
  for (uint8_t i = 0; i < length; ++i) {
    const uint8_t bit = static_cast<uint8_t>(startBit + i);
    if ((frame.data[bit / 8] >> (bit % 8)) & 0x01) {
      raw |= (1UL << i);
    }
  }
  value = raw;
  return true;
}

static bool readSignedBitsLE(const can_frame& frame, uint8_t startBit, uint8_t length, int32_t& value) {
  uint32_t raw = 0;
  if (!readBitsLE(frame, startBit, length, raw)) return false;
  if (length < 32 && (raw & (1UL << (length - 1)))) {
    raw |= (~0UL << length);
  }
  value = static_cast<int32_t>(raw);
  return true;
}

static void handleRearFogDecelAccel(float accel, const RuntimeConfig& cfg) {
  const uint32_t now = millis();
  if (!canbReady || !cfg.canbEnabled || !cfg.rearFogBrakeStrobeEnabled) {
    rearFogMildDecelStartMs = 0;
    rearFogHardDecelStartMs = 0;
    rearFogMildDecelTriggered = false;
    rearFogHardDecelTriggered = false;
    return;
  }

  const bool recentPedalBrake = now < rearFogRecentPedalBrakeUntilMs;
  const bool recentBrakeLamp = now < rearFogRecentBrakeLampUntilMs;
  const bool recentBrakeTorque = now < rearFogRecentBrakeTorqueUntilMs;
  const bool recentRegen = now < rearFogRecentRegenUntilMs;
  const bool recentNegTorque = now < rearFogRecentNegTorqueUntilMs;
  const bool recentSpeedFalling = now < rearFogRecentSpeedFallingUntilMs;

  const bool hardAux = recentBrakeLamp || recentBrakeTorque || recentPedalBrake;
  const bool mildAux = hardAux || recentRegen || recentNegTorque || recentSpeedFalling;
  const bool hardCandidate =
    (accel <= REAR_FOG_HARD_DECEL_THRESHOLD && hardAux) ||
    (accel <= REAR_FOG_VERY_HARD_DECEL_THRESHOLD);
  const bool mildCandidate =
    !hardCandidate && accel <= REAR_FOG_MILD_DECEL_THRESHOLD && mildAux;

  if (!hardCandidate) {
    rearFogHardDecelStartMs = 0;
    rearFogHardDecelTriggered = false;
  } else {
    if (rearFogHardDecelStartMs == 0) rearFogHardDecelStartMs = now;
    if (!rearFogHardDecelTriggered &&
        (now - rearFogHardDecelStartMs) >= REAR_FOG_HARD_DECEL_HOLD_MS) {
      startRearFogBrakeStrobe(REAR_FOG_BODY_STROBE_PULSES, REAR_FOG_PRIORITY_BODY);
      rearFogHardDecelTriggered = true;
      rearFogMildDecelTriggered = true;
    }
  }

  if (!mildCandidate || hardCandidate) {
    rearFogMildDecelStartMs = 0;
    if (!mildCandidate) rearFogMildDecelTriggered = false;
    return;
  }

  if (rearFogMildDecelStartMs == 0) rearFogMildDecelStartMs = now;
  if (!rearFogMildDecelTriggered &&
      (now - rearFogMildDecelStartMs) >= REAR_FOG_MILD_DECEL_HOLD_MS) {
    startRearFogBrakeStrobe(REAR_FOG_PEDAL_STROBE_PULSES, REAR_FOG_PRIORITY_PEDAL);
    rearFogMildDecelTriggered = true;
  }
}

static void serviceHighBeamStrobe(const RuntimeConfig& cfg) {
  if (!canbReady) return;
  // 0x249 high-light strobe: alternate PULL(status=1) and idle(status=0).
  if (!cfg.canbEnabled) {
    if (highBeamStrobeActive || highBeamStrobeOutputOn) stopHighBeamStrobe(false);
    return;
  }

  if (!cfg.highBeamStrobeEnabled && !highBeamStrobeManualTrigger) {
    if (highBeamStrobeActive || highBeamStrobeOutputOn) stopHighBeamStrobe(true);
    highBeamPullCount = 0;
    highBeamLastPullDown = false;
    return;
  }

  if (!highBeamStrobeActive) return;

  const uint32_t now = millis();
  if (highBeamStrobeLastToggleMs != 0 &&
      (now - highBeamStrobeLastToggleMs) < HIGH_BEAM_STROBE_INTERVAL_MS) {
    if (highBeamStrobeLastSendMs == 0 ||
        (now - highBeamStrobeLastSendMs) >= HIGH_BEAM_STROBE_RESEND_MS) {
      canb_send(highBeamFrame(highBeamStrobeOutputOn ? STALK_STATUS_PULL : STALK_STATUS_IDLE));
      highBeamStrobeLastSendMs = now;
    }
    g_status.highBeamStrobeActive = 1;
    g_status.highBeamStrobeRemaining = highBeamStrobePulsesRemaining;
    return;
  }
  highBeamStrobeLastToggleMs = now;

  if (!highBeamStrobeOutputOn) {
    canb_send(highBeamFrame(STALK_STATUS_PULL));
    highBeamStrobeOutputOn = true;
  } else {
    canb_send(highBeamFrame(STALK_STATUS_IDLE));
    highBeamStrobeOutputOn = false;
    if (highBeamStrobePulsesRemaining > 0) highBeamStrobePulsesRemaining--;
    if (highBeamStrobePulsesRemaining == 0) {
      stopHighBeamStrobe(false);
      return;
    }
  }
  highBeamStrobeLastSendMs = now;

  g_status.highBeamStrobeActive = 1;
  g_status.highBeamStrobeRemaining = highBeamStrobePulsesRemaining;
}

static void serviceRearFogBrakeStrobe(const RuntimeConfig& cfg) {
  if (!canbReady) return;
  // 0x273 鍚庨浘鐏垎闂細韪╁埞杞﹀彧璐熻矗瑙﹀彂锛屽疄闄呰緭鍑哄浐瀹?6 涓?ON/OFF 鑴夊啿銆?  if (!canbReady) return;
  if (!cfg.canbEnabled) {
    if (rearFogBrakeStrobeActive || rearFogBrakeStrobeOutputOn) stopRearFogBrakeStrobe(false);
    return;
  }

  if (!cfg.rearFogBrakeStrobeEnabled && !rearFogBrakeStrobeManualTrigger) {
    if (rearFogBrakeStrobeActive || rearFogBrakeStrobeOutputOn) stopRearFogBrakeStrobe(true);
    rearFogLastPedalBrakeActive = false;
    rearFogLastBrakeActive = false;
    return;
  }

  if (!rearFogBrakeStrobeActive) return;

  const uint32_t now = millis();
  if (rearFogBrakeStrobeLastToggleMs != 0 &&
      (now - rearFogBrakeStrobeLastToggleMs) < REAR_FOG_STROBE_INTERVAL_MS) {
    g_status.rearFogBrakeStrobeActive = 1;
    g_status.rearFogBrakeStrobeRemaining = rearFogBrakeStrobePulsesRemaining;
    return;
  }
  rearFogBrakeStrobeLastToggleMs = now;

  if (!rearFogBrakeStrobeOutputOn) {
    canb_send(rearFogFrame(true));
    rearFogBrakeStrobeOutputOn = true;
  } else {
    canb_send(rearFogFrame(false));
    rearFogBrakeStrobeOutputOn = false;
    if (rearFogBrakeStrobePulsesRemaining > 0) rearFogBrakeStrobePulsesRemaining--;
    if (rearFogBrakeStrobePulsesRemaining == 0) {
      stopRearFogBrakeStrobe(false);
      return;
    }
  }

  g_status.rearFogBrakeStrobeActive = 1;
  g_status.rearFogBrakeStrobeRemaining = rearFogBrakeStrobePulsesRemaining;
}

static void handleRearFogPedalBrakeEdge(bool brakeActive, const RuntimeConfig& cfg) {
  (void)cfg;
  const uint32_t now = millis();
  if (brakeActive) {
    rearFogRecentPedalBrakeUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
  }
  rearFogLastPedalBrakeActive = brakeActive;
}

static void handleRearFogBrakeLampState(bool brakeActive, const RuntimeConfig& cfg) {
  (void)cfg;
  const uint32_t now = millis();
  if (brakeActive) {
    rearFogRecentBrakeLampUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
  }
  rearFogLastBrakeActive = brakeActive;
}

static void handleRearFogCanADecelFrame(const can_frame& frame, const RuntimeConfig& cfg) {
  const uint32_t now = millis();

  if (frame.can_id == CAN_ID_BRAKE_PEDAL && frame.can_dlc >= 8) {
    uint32_t raw = 0;
    const bool brakeLamp = readBitsLE(frame, 21, 1, raw) && raw != 0;
    const bool driverBrake =
      readBitsLE(frame, 29, 2, raw) && raw == 2;
    const bool brakeApply =
      readBitsLE(frame, 31, 1, raw) && raw != 0;
    const bool brakeTorque =
      readBitsLE(frame, 51, 13, raw) && raw > 0;
    handleRearFogPedalBrakeEdge(driverBrake || brakeApply, cfg);
    if (brakeLamp) rearFogRecentBrakeLampUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    if (brakeTorque) rearFogRecentBrakeTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    return;
  }

  if ((frame.can_id == CAN_ID_RCM_INERTIAL2_CH || frame.can_id == CAN_ID_RCM_INERTIAL2_ETH) &&
      frame.can_dlc >= 6) {
    int32_t rawAccel = 0;
    if (readSignedBitsLE(frame, 0, 16, rawAccel) && rawAccel != -32768) {
      bool qfOk = true;
      uint32_t qf = 1;
      if (frame.can_id == CAN_ID_RCM_INERTIAL2_CH && frame.can_dlc >= 7) {
        qfOk = readBitsLE(frame, 48, 1, qf) && qf != 0;
      }
      if (qfOk) {
        handleRearFogDecelAccel(static_cast<float>(rawAccel) * 0.00125f, cfg);
      }
    }
    return;
  }

  if (frame.can_id == CAN_ID_DI_SYSTEM_STATUS && frame.can_dlc >= 8) {
    uint32_t raw = 0;
    if (readBitsLE(frame, 51, 1, raw) && raw != 0) {
      rearFogRecentRegenUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    int32_t accelRaw = 0;
    if (readSignedBitsLE(frame, 52, 12, accelRaw) && accelRaw != -2048) {
      handleRearFogDecelAccel(static_cast<float>(accelRaw) * 0.01f, cfg);
    }
    return;
  }

  if (frame.can_id == CAN_ID_DI_CHASSIS_CONTROL && frame.can_dlc >= 4) {
    uint32_t raw = 0;
    if (readBitsLE(frame, 15, 1, raw) && raw != 0) {
      rearFogRecentBrakeTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    return;
  }

  if (frame.can_id == CAN_ID_ESP_BRAKE_TORQUE && frame.can_dlc >= 7) {
    uint32_t qf = 0;
    uint32_t frL = 0, frR = 0, reL = 0, reR = 0;
    if (readBitsLE(frame, 50, 1, qf) && qf != 0 &&
        readBitsLE(frame, 0, 12, frL) &&
        readBitsLE(frame, 12, 12, frR) &&
        readBitsLE(frame, 24, 12, reL) &&
        readBitsLE(frame, 36, 12, reR) &&
        (frL + frR + reL + reR) > 0) {
      rearFogRecentBrakeTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    return;
  }

  if (frame.can_id == CAN_ID_DIF_TORQUE && frame.can_dlc >= 8) {
    int32_t torqueActualRaw = 0;
    uint32_t qf = 0;
    if (readSignedBitsLE(frame, 27, 13, torqueActualRaw) &&
        readBitsLE(frame, 56, 2, qf) && qf == 1 &&
        torqueActualRaw < 0) {
      rearFogRecentNegTorqueUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    return;
  }

  if (frame.can_id == CAN_ID_VEHICLE_SPEED && frame.can_dlc >= 4) {
    const uint16_t rawSpeed =
      static_cast<uint16_t>((static_cast<uint16_t>(frame.data[2]) << 4) | (frame.data[1] >> 4));
    float speedKph = static_cast<float>(rawSpeed) * 0.08f - 40.0f;
    if (speedKph < 0.0f) speedKph = 0.0f;
    if (rearFogLastVehicleSpeedKph >= 0.0f &&
        speedKph + 0.5f < rearFogLastVehicleSpeedKph) {
      rearFogRecentSpeedFallingUntilMs = now + REAR_FOG_DECEL_RECENT_MS;
    }
    rearFogLastVehicleSpeedKph = speedKph;
    return;
  }
}

static void handleCanBFrame(const can_frame& frame) {
  // Stage 1: statistics only. No heavy work, no Serial, no JSON, no bridging.
  canbLastId = frame.can_id;
  if (frame.can_id == CANB_ID_STW_ACTN_RQ && frame.can_dlc >= 2) {
    canbLastStwActnRqFrame = frame;
    canbHasLastStwActnRqFrame = true;
    highBeamStalkLastCounter = static_cast<uint8_t>(frame.data[1] & 0x0F);

    const RuntimeConfig cfg = configSnapshot();
    const uint8_t stalkStatus = readStalkStatus(frame);
    const bool pullDown = stalkStatus == STALK_STATUS_PULL;
    const uint32_t now = millis();

    if (!cfg.highBeamStrobeEnabled) {
      highBeamPullCount = 0;
    } else if (pullDown && !highBeamLastPullDown && !highBeamStrobeActive) {
      if (highBeamLastPullMs == 0 ||
          (now - highBeamLastPullMs) > HIGH_BEAM_DOUBLE_PULL_WINDOW_MS) {
        highBeamPullCount = 0;
      }
      highBeamLastPullMs = now;
      highBeamPullCount++;
      if (highBeamPullCount >= 2) {
        highBeamPullCount = 0;
        startHighBeamStrobe();
      }
    }
    highBeamLastPullDown = pullDown;
  }

  if (frame.can_id == CANB_ID_BODY_LIGHTING && frame.can_dlc >= 8) {
    canbLastBodyLightingFrame = frame;
    // 0x273 璺緞锛氬彧鐢?data[7] bit0 鐨勫埞杞︿笂鍗囨部瑙﹀彂锛屼笉鍦ㄨ繖閲岀洿鎺ュ彂閫併€?    canbLastBodyLightingFrame = frame;
    canbHasLastBodyLightingFrame = true;

    const RuntimeConfig cfg = configSnapshot();
    const bool brakeActive = (frame.data[7] & 0x01) != 0;
    handleRearFogBrakeLampState(brakeActive, cfg);
  }
}

static void drainCanBWithBudget() {
  if (!canbReady) return;

  for (uint8_t i = 0; i < CANB_RX_SCAN_LIMIT; ++i) {
    can_frame frame;
    if (!canb_recv(frame)) break;
    handleCanBFrame(frame);
  }
}

// VCSEC_serviceDiagnosticRequest (0x339) on the BODY bus / CAN B.
//   enter service mode -> 00 00 00 00 00 80 00 00  (byte5 bit7 = 1)
//   exit  service mode -> 00 00 00 00 00 00 00 00
// Spec: 4 frames at 10ms spacing. Default OFF; only sent after a toggle.
static void setCanBServiceMode(bool enabled) {
  canbServiceModeActive = enabled;
  canbServiceBurstByte5 = enabled ? 0x80 : 0x00;
  canbServiceBurstRemaining = 4;
  canbLastServiceBurstMs = 0;  // fire the first frame on the next tick
}

static void serviceCanBScheduledTx() {
  if (canbServiceBurstRemaining == 0 || !canbReady) return;

  const uint32_t now = millis();
  if (canbLastServiceBurstMs != 0 && (now - canbLastServiceBurstMs) < 10) return;
  canbLastServiceBurstMs = now;

  can_frame f = {};
  f.can_id = 0x339;
  f.can_dlc = 8;
  f.data[5] = canbServiceBurstByte5;
  canb_send(f);
  canbServiceBurstRemaining--;
}

#endif  // ENABLE_CANB_MCP2515

// ============================================================================
// Light WebUI 鈥?optional SoftAP parameter page on a dedicated low-prio task
// ============================================================================
#ifdef ENABLE_LIGHT_WEBUI

#ifndef WEBUI_AP_SSID
#define WEBUI_AP_SSID "T2CAN-FSD"
#endif
#ifndef WEBUI_AP_PASS
#define WEBUI_AP_PASS "12345678"
#endif

static WebServer server(80);
static volatile bool webUiEnabled = true;
static volatile bool webUiShutdownPending = false;
static Preferences prefs;

static void loadConfigFromPrefs();
static void saveConfigToPrefs();
static void setupLightWebUi();
static void webTask(void*);

#include "web_ui_page.h"  // kIndexHtml 鈥?kept out of the .ino prototype scanner

static void handleRoot() {
  server.send_P(200, "text/html", kIndexHtml);
}

static void handleStatus() {
  // Read-only: snapshot the cached config + status, never touch the CAN bus.
  RuntimeConfig c = configSnapshot();
  RuntimeStatus s = g_status;

  String j;
  j.reserve(800);
  j += '{';
  j += "\"fsdEnabled\":";            j += c.fsdEnabled ? 1 : 0;
  j += ",\"autoSpeedOffsetEnabled\":"; j += c.autoSpeedOffsetEnabled ? 1 : 0;
  j += ",\"slewPctPerSec\":";        j += c.slewPctPerSec;
  j += ",\"lowSpeedMaxPctRaw\":";    j += c.lowSpeedMaxPctRaw;
  j += ",\"targetBelow60\":";        j += c.targetBelow60;
  j += ",\"target60\":";             j += c.target60;
  j += ",\"target70\":";             j += c.target70;
  j += ",\"target80\":";             j += c.target80;
  j += ",\"target90\":";             j += c.target90;
  j += ",\"target100\":";            j += c.target100;
  j += ",\"target120\":";            j += c.target120;
  j += ",\"canbEnabled\":";          j += c.canbEnabled ? 1 : 0;
  j += ",\"canbServiceModeEnabled\":"; j += c.canbServiceModeEnabled ? 1 : 0;
  j += ",\"canbFilterEnabled\":";    j += c.canbFilterEnabled ? 1 : 0;
#ifdef ENABLE_CANB_MCP2515
  j += ",\"canbReady\":";            j += canbReady ? 1 : 0;
  j += ",\"canbHardwareFilterEnabled\":"; j += canbHardwareFilterEnabled ? 1 : 0;
#else
  j += ",\"canbReady\":0";
  j += ",\"canbHardwareFilterEnabled\":0";
#endif
  j += ",\"highBeamStrobeEnabled\":"; j += c.highBeamStrobeEnabled ? 1 : 0;
  j += ",\"rearFogBrakeStrobeEnabled\":"; j += c.rearFogBrakeStrobeEnabled ? 1 : 0;
  j += ",\"can1Rx\":";               j += s.can1Rx;
  j += ",\"can1Tx\":";               j += s.can1Tx;
  j += ",\"can1TxFail\":";           j += s.can1TxFail;
  j += ",\"twaiBusOffCount\":";      j += s.twaiBusOffCount;
  j += ",\"canbRx\":";               j += s.canbRx;
  j += ",\"canbTx\":";               j += s.canbTx;
  j += ",\"canbTxFail\":";           j += s.canbTxFail;
  j += ",\"canbLastId\":";           j += s.canbLastId;
  j += ",\"highBeamStrobeActive\":"; j += s.highBeamStrobeActive;
  j += ",\"highBeamStrobeRemaining\":"; j += s.highBeamStrobeRemaining;
  j += ",\"rearFogBrakeStrobeActive\":"; j += s.rearFogBrakeStrobeActive;
  j += ",\"rearFogBrakeStrobeRemaining\":"; j += s.rearFogBrakeStrobeRemaining;
  j += ",\"fusedLimitKph\":";        j += s.fusedLimitKph;
  j += ",\"targetSpeedKph\":";       j += s.targetSpeedKph;
  j += ",\"offsetKph\":";            j += s.offsetKph;
  j += ",\"offsetRaw\":";            j += s.offsetRaw;
  j += ",\"uptime\":";               j += (uint32_t)(millis() / 1000);
  j += '}';
  server.send(200, "application/json", j);
}

static uint16_t argU16(const char* name, uint16_t fallback) {
  if (!server.hasArg(name)) return fallback;
  long v = server.arg(name).toInt();
  if (v < 0) v = 0;
  if (v > 65535) v = 65535;
  return static_cast<uint16_t>(v);
}

static bool argBool(const char* name, bool fallback) {
  if (!server.hasArg(name)) return fallback;
  String v = server.arg(name);
  return v == "1" || v == "true" || v == "on";
}

// POST /config 鈥?update the live config in RAM only (no Flash write here).
static void handleConfig() {
  RuntimeConfig c = configSnapshot();
  const bool oldCanBFilterEnabled = c.canbFilterEnabled;

  c.fsdEnabled              = argBool("fsdEnabled", c.fsdEnabled);
  c.autoSpeedOffsetEnabled  = argBool("autoSpeedOffsetEnabled", c.autoSpeedOffsetEnabled);
  c.slewPctPerSec           = static_cast<uint8_t>(argU16("slewPctPerSec", c.slewPctPerSec));
  c.lowSpeedMaxPctRaw       = static_cast<uint8_t>(argU16("lowSpeedMaxPctRaw", c.lowSpeedMaxPctRaw));
  c.targetBelow60           = argU16("targetBelow60", c.targetBelow60);
  c.target60                = argU16("target60", c.target60);
  c.target70                = argU16("target70", c.target70);
  c.target80                = argU16("target80", c.target80);
  c.target90                = argU16("target90", c.target90);
  c.target100               = argU16("target100", c.target100);
  c.target120               = argU16("target120", c.target120);
  c.canbEnabled             = argBool("canbEnabled", c.canbEnabled);
  c.canbFilterEnabled       = argBool("canbFilterEnabled", c.canbFilterEnabled);
  c.highBeamStrobeEnabled   = argBool("highBeamStrobeEnabled", c.highBeamStrobeEnabled);
  c.rearFogBrakeStrobeEnabled = argBool("rearFogBrakeStrobeEnabled", c.rearFogBrakeStrobeEnabled);

  const bool newServiceMode = argBool("canbServiceModeEnabled", c.canbServiceModeEnabled);
  const bool serviceModeChanged = (newServiceMode != c.canbServiceModeEnabled);
  c.canbServiceModeEnabled  = newServiceMode;

  portENTER_CRITICAL(&g_cfgMux);
  g_config = c;
  portEXIT_CRITICAL(&g_cfgMux);

#ifdef ENABLE_CANB_MCP2515
  if (canbReady && c.canbFilterEnabled != oldCanBFilterEnabled) {
    if (!applyCanBFilters(c.canbFilterEnabled)) {
      c.canbFilterEnabled = oldCanBFilterEnabled;
      portENTER_CRITICAL(&g_cfgMux);
      g_config = c;
      portEXIT_CRITICAL(&g_cfgMux);
      server.send(500, "application/json", "{\"ok\":false,\"error\":\"canb_filter\"}");
      return;
    }
  }

  // Toggling service mode queues a 0x339 burst (handled non-blocking in loop()).
  if (serviceModeChanged) setCanBServiceMode(newServiceMode);
#else
  (void)serviceModeChanged;
#endif

  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /save 鈥?persist the current RAM config to Flash (Preferences).
static void handleSave() {
  saveConfigToPrefs();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /web/off 鈥?acknowledge, then shut the WebUI down asynchronously in the
// web task so this handler never blocks. CAN is unaffected.
static void handleWebOff() {
  server.send(200, "application/json", "{\"ok\":true}");
  webUiShutdownPending = true;
}

static void handleCanBTest() {
#ifdef ENABLE_CANB_MCP2515
  RuntimeConfig c = configSnapshot();
  if (!c.canbEnabled) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"canb_disabled\"}");
    return;
  }
  if (!canbReady) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"canb_not_ready\"}");
    return;
  }
  if (!server.hasArg("type")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing_type\"}");
    return;
  }

  String type = server.arg("type");
  if (type == "strobe") {
    startHighBeamStrobe(true);
  } else if (type == "fog") {
    startRearFogBrakeStrobe(REAR_FOG_BODY_STROBE_PULSES, REAR_FOG_PRIORITY_BODY, true);
  } else {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_type\"}");
    return;
  }

  server.send(200, "application/json", "{\"ok\":true}");
#else
  server.send(400, "application/json", "{\"ok\":false,\"error\":\"canb_not_compiled\"}");
#endif
}

static uint8_t parseRecIdList(const String& s, uint32_t* out, uint8_t maxCount) {
  uint8_t count = 0;
  const char* p = s.c_str();
  while (*p && count < maxCount) {
    while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    if (!*p) break;
    char* end = nullptr;
    uint32_t v = static_cast<uint32_t>(strtoul(p, &end, 16));
    if (end == p) break;
    out[count++] = v & CAN_SFF_MASK;
    p = end;
  }
  return count;
}

static void handleRecStart() {
  if (!recBuf || recCapacity == 0) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"psram_rec_buffer\"}");
    return;
  }

  if (server.hasArg("ids")) {
    recFilterCount = parseRecIdList(server.arg("ids"), recFilterIds, REC_FILTER_MAX);
  } else {
    recFilterCount = 0;
  }
  if (server.hasArg("exclude")) {
    recExcludeCount = parseRecIdList(server.arg("exclude"), recExcludeIds, REC_FILTER_MAX);
  } else {
    recExcludeCount = 0;
  }

  portENTER_CRITICAL(&g_recMux);
  recCount = 0;
  recSaved = false;
  recStartMs = millis();
  recActive = true;
  portEXIT_CRITICAL(&g_recMux);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRecStop() {
  portENTER_CRITICAL(&g_recMux);
  recActive = false;
  recSaved = true;
  portEXIT_CRITICAL(&g_recMux);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleRecStatus() {
  if (recActive && (millis() - recStartMs >= REC_MAX_DURATION_MS)) {
    portENTER_CRITICAL(&g_recMux);
    recActive = false;
    recSaved = true;
    portEXIT_CRITICAL(&g_recMux);
  }
  String j;
  j.reserve(128);
  j += "{\"active\":";
  j += recActive ? "true" : "false";
  j += ",\"count\":";
  j += recCount;
  j += ",\"cap\":";
  j += recCapacity;
  j += ",\"psram\":";
  j += recPsramReady ? "true" : "false";
  j += ",\"bytes\":";
  j += static_cast<unsigned long>(recBufferBytes);
  j += ",\"saved\":";
  j += recSaved ? "true" : "false";
  j += ",\"filter\":";
  j += recFilterCount;
  j += ",\"exclude\":";
  j += recExcludeCount;
  j += "}";
  server.send(200, "application/json", j);
}

static void handleRecDownload() {
  if (recActive) {
    server.send(409, "text/plain", "Stop recording before download");
    return;
  }
  if (!recSaved || recCount == 0) {
    server.send(404, "text/plain", "No recording saved yet");
    return;
  }

  const uint32_t n = recCount;
  server.sendHeader("Content-Disposition", "attachment; filename=\"can_recording.csv\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  char chunk[1024];
  size_t used = 0;
  auto flushChunk = [&]() {
    if (used == 0) return;
    server.sendContent(chunk, used);
    used = 0;
    yield();
  };
  auto appendChunk = [&](const char* text, size_t len) {
    while (len > 0) {
      const size_t room = sizeof(chunk) - used;
      if (room == 0) flushChunk();
      const size_t take = std::min(len, sizeof(chunk) - used);
      memcpy(chunk + used, text, take);
      used += take;
      text += take;
      len -= take;
    }
  };

  static const char header[] = "ts_ms,dir,bus,id,dlc,b0,b1,b2,b3,b4,b5,b6,b7\n";
  appendChunk(header, sizeof(header) - 1);
  char line[96];
  for (uint32_t i = 0; i < n; ++i) {
    const RecFrame& r = recBuf[i];
    snprintf(line, sizeof(line),
             "%lu,%c,%u,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
             static_cast<unsigned long>(r.ts),
             r.dir ? r.dir : 'R',
             static_cast<unsigned>(r.bus),
             static_cast<unsigned long>(r.id),
             static_cast<unsigned>(r.dlc),
             static_cast<unsigned>(r.data[0]),
             static_cast<unsigned>(r.data[1]),
             static_cast<unsigned>(r.data[2]),
             static_cast<unsigned>(r.data[3]),
             static_cast<unsigned>(r.data[4]),
             static_cast<unsigned>(r.data[5]),
             static_cast<unsigned>(r.data[6]),
             static_cast<unsigned>(r.data[7]));
    appendChunk(line, strlen(line));
    if ((i & 0x3F) == 0) flushChunk();
  }
  flushChunk();
  server.sendContent("");
}

static void loadConfigFromPrefs() {
  prefs.begin("t2can", true);
  RuntimeConfig c;  // defaults
  c.fsdEnabled             = prefs.getBool("fsdEnabled", c.fsdEnabled);
  c.autoSpeedOffsetEnabled = prefs.getBool("autoOffset", c.autoSpeedOffsetEnabled);
  c.slewPctPerSec          = prefs.getUChar("slewPct", c.slewPctPerSec);
  c.lowSpeedMaxPctRaw      = prefs.getUChar("lowRaw", c.lowSpeedMaxPctRaw);
  c.targetBelow60          = prefs.getUShort("tB60", c.targetBelow60);
  c.target60               = prefs.getUShort("t60", c.target60);
  c.target70               = prefs.getUShort("t70", c.target70);
  c.target80               = prefs.getUShort("t80", c.target80);
  c.target90               = prefs.getUShort("t90", c.target90);
  c.target100              = prefs.getUShort("t100", c.target100);
  c.target120              = prefs.getUShort("t120", c.target120);
  c.canbEnabled            = prefs.getBool("canbEn", c.canbEnabled);
  c.canbServiceModeEnabled = prefs.getBool("canbSvc", c.canbServiceModeEnabled);
  c.canbFilterEnabled      = prefs.getBool("canbFilt", c.canbFilterEnabled);
  c.highBeamStrobeEnabled  = prefs.getBool("hbStrobe", c.highBeamStrobeEnabled);
  c.rearFogBrakeStrobeEnabled = prefs.getBool("fogBrake", c.rearFogBrakeStrobeEnabled);
  prefs.end();

  portENTER_CRITICAL(&g_cfgMux);
  g_config = c;
  portEXIT_CRITICAL(&g_cfgMux);
}

static void saveConfigToPrefs() {
  RuntimeConfig c = configSnapshot();
  prefs.begin("t2can", false);
  prefs.putBool("fsdEnabled", c.fsdEnabled);
  prefs.putBool("autoOffset", c.autoSpeedOffsetEnabled);
  prefs.putUChar("slewPct", c.slewPctPerSec);
  prefs.putUChar("lowRaw", c.lowSpeedMaxPctRaw);
  prefs.putUShort("tB60", c.targetBelow60);
  prefs.putUShort("t60", c.target60);
  prefs.putUShort("t70", c.target70);
  prefs.putUShort("t80", c.target80);
  prefs.putUShort("t90", c.target90);
  prefs.putUShort("t100", c.target100);
  prefs.putUShort("t120", c.target120);
  prefs.putBool("canbEn", c.canbEnabled);
  prefs.putBool("canbSvc", c.canbServiceModeEnabled);
  prefs.putBool("canbFilt", c.canbFilterEnabled);
  prefs.putBool("hbStrobe", c.highBeamStrobeEnabled);
  prefs.putBool("fogBrake", c.rearFogBrakeStrobeEnabled);
  prefs.end();
}

static void setupLightWebUi() {
  setupRecorderBuffer();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WEBUI_AP_SSID, WEBUI_AP_PASS);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/test", HTTP_POST, handleCanBTest);
  server.on("/rec_start", HTTP_POST, handleRecStart);
  server.on("/rec_stop", HTTP_POST, handleRecStop);
  server.on("/rec_status", HTTP_GET, handleRecStatus);
  server.on("/rec_download", HTTP_GET, handleRecDownload);
  server.on("/web/off", HTTP_POST, handleWebOff);
  server.begin();
  webUiEnabled = true;
}

// Dedicated WebUI task (pinned to core 0, low priority). The CAN main loop on
// core 1 never waits on this; HTTP is only serviced while the WebUI is enabled.
static void webTask(void*) {
  for (;;) {
    if (webUiShutdownPending) {
      delay(50);
      server.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_OFF);
      webUiEnabled = false;
      webUiShutdownPending = false;
    }
    if (webUiEnabled) {
      server.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

#endif  // ENABLE_LIGHT_WEBUI

// ---- Main ----

void setup() {
  pinMode(PIN_LED, OUTPUT);

  delay(500);

#ifdef ENABLE_LIGHT_WEBUI
  loadConfigFromPrefs();
#endif

  // Configure TWAI (CAN) peripheral at 500 kbps
  twai_general_config_t g_config_twai = TWAI_GENERAL_CONFIG_DEFAULT(
    static_cast<gpio_num_t>(TWAI_TX_PIN),
    static_cast<gpio_num_t>(TWAI_RX_PIN),
    TWAI_MODE_NORMAL);
  g_config_twai.rx_queue_len = TWAI_RX_QUEUE_LEN;
  g_config_twai.tx_queue_len = TWAI_TX_QUEUE_LEN;
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f_config = { CAN_ACCEPT_CODE, CAN_ACCEPT_MASK, true };

  twai_driver_install(&g_config_twai, &t_config, &f_config);
  twai_start();
  twai_reconfigure_alerts(TWAI_ALERT_MASK, nullptr);

#ifdef ENABLE_CANB_MCP2515
  setupCanB();
#endif

#ifdef ENABLE_LIGHT_WEBUI
  setupLightWebUi();
  xTaskCreatePinnedToCore(webTask, "web", 4096, nullptr, 1, nullptr, 0);
#endif
}

void loop() {
  serviceTwaiAlerts();

  bool didWork = false;

  can_frame frame;
  if (twai_recv(frame)) {
    didWork = true;
    digitalWrite(PIN_LED, LOW);
    recordCanFrame(frame, 'R', 1);
    RuntimeConfig cfg = configSnapshot();
    speedLimitMonitor.update(frame);
    handler.refreshUnifiedSpeedCompensation(cfg);
    handler.handelMessage(frame, cfg);
  }

#ifdef ENABLE_CANB_MCP2515
  RuntimeConfig canbCfg = configSnapshot();
  if (canbCfg.canbEnabled) {
    drainCanBWithBudget();
    serviceCanBScheduledTx();
    serviceHighBeamStrobe(canbCfg);
    serviceRearFogBrakeStrobe(canbCfg);
  } else {
    serviceHighBeamStrobe(canbCfg);
    serviceRearFogBrakeStrobe(canbCfg);
  }
#endif

  if (!didWork) {
    digitalWrite(PIN_LED, HIGH);
  }
}

