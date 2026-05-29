#include <Arduino.h>
#include <driver/twai.h>

#ifndef TWAI_TX_PIN
#define TWAI_TX_PIN GPIO_NUM_15
#endif

#ifndef TWAI_RX_PIN
#define TWAI_RX_PIN GPIO_NUM_16
#endif

#ifndef TWAI_RX_QUEUE_LEN
#define TWAI_RX_QUEUE_LEN 64
#endif

#ifndef TWAI_TX_QUEUE_LEN
#define TWAI_TX_QUEUE_LEN 16
#endif

constexpr uint32_t CAN_ID_FOLLOW_DISTANCE = 1016;
constexpr uint32_t CAN_ID_FSD_CONTROL = 1021;
constexpr uint32_t CAN_STD_ID_MASK = 0x7FF;

constexpr uint8_t MUX_MASK = 0x07;
constexpr uint8_t FOLLOW_DISTANCE_MASK = 0b11100000;
constexpr uint8_t FOLLOW_DISTANCE_SHIFT = 5;
constexpr uint8_t SPEED_PROFILE_MASK = 0x06;
constexpr uint8_t SPEED_PROFILE_SHIFT = 1;
constexpr uint8_t TWAI_STANDARD_ID_SHIFT = 21;
constexpr uint32_t TWAI_FILTER_COMPARE_MASK =
  CAN_STD_ID_MASK & ~(CAN_ID_FOLLOW_DISTANCE ^ CAN_ID_FSD_CONTROL);
constexpr uint32_t TWAI_FILTER_ACCEPTANCE_CODE =
  (CAN_ID_FOLLOW_DISTANCE & TWAI_FILTER_COMPARE_MASK) << TWAI_STANDARD_ID_SHIFT;
constexpr uint32_t TWAI_FILTER_ACCEPTANCE_MASK =
  ~(TWAI_FILTER_COMPARE_MASK << TWAI_STANDARD_ID_SHIFT);
constexpr uint32_t TWAI_ALERTS =
  TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_RX_QUEUE_FULL |
  TWAI_ALERT_RX_FIFO_OVERRUN | TWAI_ALERT_TX_FAILED;
constexpr TickType_t TWAI_RX_TIMEOUT = pdMS_TO_TICKS(10);
constexpr TickType_t TWAI_TX_TIMEOUT = pdMS_TO_TICKS(1);

bool twaiRunning = false;
bool twaiRecovering = false;

struct HW3Handler {
  uint8_t speedProfile = 1;

  void handleMessage(twai_message_t& message) {
    if (message.extd || message.rtr || message.data_length_code < 7) return;

    if (message.identifier == CAN_ID_FOLLOW_DISTANCE) {
      updateSpeedProfile(message);
      return;
    }

    if (message.identifier != CAN_ID_FSD_CONTROL) return;

    switch (readMux(message)) {
      case 0:
        enableFsdSpeedProfile(message);
        transmitMessage(message);
        break;
      case 1:
        suppressControlFlag(message);
        transmitMessage(message);
        break;
      default:
        break;
    }
  }

private:
  static uint8_t readMux(const twai_message_t& message) {
    return message.data[0] & MUX_MASK;
  }

  void updateSpeedProfile(const twai_message_t& message) {
    const uint8_t followDistance =
      (message.data[5] & FOLLOW_DISTANCE_MASK) >> FOLLOW_DISTANCE_SHIFT;

    switch (followDistance) {
      case 1: speedProfile = 2; break;
      case 2: speedProfile = 1; break;
      case 3: speedProfile = 0; break;
      default: break;
    }
  }

  void enableFsdSpeedProfile(twai_message_t& message) const {
    message.data[5] |= (1U << 6);
    message.data[6] = (message.data[6] & static_cast<uint8_t>(~SPEED_PROFILE_MASK)) |
                      ((speedProfile << SPEED_PROFILE_SHIFT) & SPEED_PROFILE_MASK);
  }

  static void suppressControlFlag(twai_message_t& message) {
    message.data[2] &= static_cast<uint8_t>(~(1U << 3));
  }

  static void transmitMessage(const twai_message_t& message) {
    twai_transmit(&message, TWAI_TX_TIMEOUT);
  }
};

HW3Handler handler;

void handleTwaiAlerts() {
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) != ESP_OK) return;

  if (alerts & TWAI_ALERT_BUS_OFF) {
    twaiRunning = false;
    twaiRecovering = (twai_initiate_recovery() == ESP_OK);
    return;
  }

  if (alerts & TWAI_ALERT_BUS_RECOVERED) {
    twaiRecovering = false;
    twaiRunning = (twai_start() == ESP_OK);
  }

  if (alerts & (TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_RX_FIFO_OVERRUN)) {
    twai_clear_receive_queue();
  }
}

void setup() {
  twai_general_config_t generalConfig = TWAI_GENERAL_CONFIG_DEFAULT(
    static_cast<gpio_num_t>(TWAI_TX_PIN),
    static_cast<gpio_num_t>(TWAI_RX_PIN),
    TWAI_MODE_NORMAL);
  generalConfig.rx_queue_len = TWAI_RX_QUEUE_LEN;
  generalConfig.tx_queue_len = TWAI_TX_QUEUE_LEN;
  generalConfig.alerts_enabled = TWAI_ALERTS;

  twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t filterConfig = {};
  filterConfig.acceptance_code = TWAI_FILTER_ACCEPTANCE_CODE;
  filterConfig.acceptance_mask = TWAI_FILTER_ACCEPTANCE_MASK;
  filterConfig.single_filter = true;

  if (twai_driver_install(&generalConfig, &timingConfig, &filterConfig) == ESP_OK) {
    twaiRunning = (twai_start() == ESP_OK);
  }
}

__attribute__((optimize("O3"))) void loop() {
  handleTwaiAlerts();
  if (!twaiRunning || twaiRecovering) {
    delay(1);
    return;
  }

  twai_message_t message;
  if (twai_receive(&message, TWAI_RX_TIMEOUT) == ESP_OK) {
    handler.handleMessage(message);
  }
}
