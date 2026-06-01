/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ESP32S3 variant — uses the built-in TWAI (CAN) peripheral instead of MCP2515.

    Two supported boards (selected via PlatformIO build_flags):
      * Waveshare ESP32-S3-RS485-CAN — single CAN, TWAI only.
      * LILYGO T-2CAN — dual CAN:
          - CAN A / CAN1: ESP32-S3 native TWAI (GPIO7/GPIO6) — FSD activation and
            speed-limit modification. This is the primary, time-critical bus.
          - CAN B / CAN2: MCP2515 over SPI (GPIO12/11/13/10/9) — second auxiliary
            bus (basic RX/TX/counters + optional 0x339 service-mode burst).

    WiFi/Bluetooth stay powered down unless the light WebUI is compiled in
    (-DENABLE_LIGHT_WEBUI), in which case a SoftAP + tiny parameter page run on
    a dedicated low-priority task so the CAN fast path is never blocked.
*/

#include <algorithm>
#include <cstring>
#include <driver/twai.h>

// ---- CAN B (MCP2515) — optional secondary bus ----
// The autowp MCP2515 library provides its own `struct can_frame` with the same
// field layout (can_id / can_dlc / data[8]) the handler already expects, so we
// only define our own copy when the library is absent. Defining both in one
// translation unit would be a redefinition error — hence the #else.
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

// ---- Light WebUI — optional ----
#ifdef ENABLE_LIGHT_WEBUI
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#endif

// This build supports the HW3 car only.

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

// ---- CAN B (MCP2515) pin/clock fallbacks — overridable via build_flags ----
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
  bool canbFilterEnabled = false;     // reserved for a future CAN B accept filter
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
      if (index == 0 && cfg.fsdEnabled) {
        setBit(frame, 46, true);
        setSpeedProfileV12V13(frame, speedProfile);
        twai_send(frame);
      }
      if (index == 1) {
        setBit(frame, 19, false);
        twai_send(frame);
      }
      if (index == 2 && cfg.fsdEnabled) {
        uint8_t speedOffsetRaw = unifiedSpeedCompensation.hasFusedSpeedLimit
          ? unifiedSpeedCompensation.speedOffsetRaw
          : readSpeedOffsetRaw(frame);
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
// CAN B (MCP2515) secondary bus — basic comms + non-blocking service-mode burst
// ============================================================================
#ifdef ENABLE_CANB_MCP2515

static MCP2515 canb(MCP2515_CS);
static bool canbReady = false;
static uint32_t canbRxCount = 0;
static uint32_t canbTxCount = 0;
static uint32_t canbTxFailCount = 0;
static uint32_t canbLastId = 0;

// 0x339 VCSEC service-mode burst state (RAM-only, OFF on boot). Each toggle
// queues 4 frames at 10ms spacing, scheduled with millis() — never delay().
// volatile: setCanBServiceMode() may run on the WebUI task (core 0) while
// serviceCanBScheduledTx() runs on the CAN loop (core 1).
static volatile bool canbServiceModeActive = false;
static volatile uint8_t canbServiceBurstRemaining = 0;
static volatile uint8_t canbServiceBurstByte5 = 0;
static volatile uint32_t canbLastServiceBurstMs = 0;

// CAN B read budget per loop pass — bounded so it can never starve CAN A.
constexpr uint8_t CANB_RX_SCAN_LIMIT = 4;

static void setupCanB();
static bool canb_recv(can_frame& frame);
static bool canb_send(const can_frame& frame);
static void drainCanBWithBudget();
static void handleCanBFrame(const can_frame& frame);
static void setCanBServiceMode(bool enabled);
static void serviceCanBScheduledTx();

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
  canb.setNormalMode();
  canbReady = true;
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
      return true;
    }
  }
  canbTxFailCount++;
  g_status.canbTxFail = canbTxFailCount;
  return false;
}

static void handleCanBFrame(const can_frame& frame) {
  // Stage 1: statistics only. No heavy work, no Serial, no JSON, no bridging.
  canbLastId = frame.can_id;
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
// Light WebUI — optional SoftAP parameter page on a dedicated low-prio task
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

#include "web_ui_page.h"  // kIndexHtml — kept out of the .ino prototype scanner

static void handleRoot() {
  server.send_P(200, "text/html", kIndexHtml);
}

static void handleStatus() {
  // Read-only: snapshot the cached config + status, never touch the CAN bus.
  RuntimeConfig c = configSnapshot();
  RuntimeStatus s = g_status;

  String j;
  j.reserve(640);
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
  j += ",\"can1Rx\":";               j += s.can1Rx;
  j += ",\"can1Tx\":";               j += s.can1Tx;
  j += ",\"can1TxFail\":";           j += s.can1TxFail;
  j += ",\"twaiBusOffCount\":";      j += s.twaiBusOffCount;
  j += ",\"canbRx\":";               j += s.canbRx;
  j += ",\"canbTx\":";               j += s.canbTx;
  j += ",\"canbTxFail\":";           j += s.canbTxFail;
  j += ",\"canbLastId\":";           j += s.canbLastId;
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

// POST /config — update the live config in RAM only (no Flash write here).
static void handleConfig() {
  RuntimeConfig c = configSnapshot();

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

  const bool newServiceMode = argBool("canbServiceModeEnabled", c.canbServiceModeEnabled);
  const bool serviceModeChanged = (newServiceMode != c.canbServiceModeEnabled);
  c.canbServiceModeEnabled  = newServiceMode;

  portENTER_CRITICAL(&g_cfgMux);
  g_config = c;
  portEXIT_CRITICAL(&g_cfgMux);

#ifdef ENABLE_CANB_MCP2515
  // Toggling service mode queues a 0x339 burst (handled non-blocking in loop()).
  if (serviceModeChanged) setCanBServiceMode(newServiceMode);
#else
  (void)serviceModeChanged;
#endif

  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /save — persist the current RAM config to Flash (Preferences).
static void handleSave() {
  saveConfigToPrefs();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /web/off — acknowledge, then shut the WebUI down asynchronously in the
// web task so this handler never blocks. CAN is unaffected.
static void handleWebOff() {
  server.send(200, "application/json", "{\"ok\":true}");
  webUiShutdownPending = true;
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
  prefs.end();
}

static void setupLightWebUi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WEBUI_AP_SSID, WEBUI_AP_PASS);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
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
    RuntimeConfig cfg = configSnapshot();
    speedLimitMonitor.update(frame);
    handler.refreshUnifiedSpeedCompensation(cfg);
    handler.handelMessage(frame, cfg);
  }

#ifdef ENABLE_CANB_MCP2515
  if (g_config.canbEnabled) {
    drainCanBWithBudget();
    serviceCanBScheduledTx();
  }
#endif

  if (!didWork) {
    digitalWrite(PIN_LED, HIGH);
  }
}
