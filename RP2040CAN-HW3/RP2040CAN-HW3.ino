#include <SPI.h>
#include <mcp2515.h>

constexpr uint8_t CAN_CS_PIN = 9;
constexpr uint8_t SPI1_RX_PIN = 12;
constexpr uint8_t SPI1_TX_PIN = 11;
constexpr uint8_t SPI1_SCK_PIN = 10;
constexpr uint32_t MCP_CLOCK_HZ = 10000000;

constexpr uint32_t CAN_ID_FOLLOW_DISTANCE = 1016;
constexpr uint32_t CAN_ID_FSD_CONTROL = 1021;

constexpr uint8_t MUX_MASK = 0x07;
constexpr uint8_t FOLLOW_DISTANCE_MASK = 0b11100000;
constexpr uint8_t FOLLOW_DISTANCE_SHIFT = 5;
constexpr uint8_t SPEED_PROFILE_MASK = 0x06;
constexpr uint8_t SPEED_PROFILE_SHIFT = 1;

MCP2515 mcp(CAN_CS_PIN, MCP_CLOCK_HZ, &SPI1);

struct HW3Handler {
  uint8_t speedProfile = 1;

  void handleMessage(can_frame& frame) {
    if (frame.can_id == CAN_ID_FOLLOW_DISTANCE) {
      updateSpeedProfile(frame);
      return;
    }

    if (frame.can_id != CAN_ID_FSD_CONTROL) return;

    switch (readMux(frame)) {
      case 0:
        enableFsdSpeedProfile(frame);
        mcp.sendMessage(&frame);
        break;
      case 1:
        suppressControlFlag(frame);
        mcp.sendMessage(&frame);
        break;
      default:
        break;
    }
  }

private:
  static uint8_t readMux(const can_frame& frame) {
    return frame.data[0] & MUX_MASK;
  }

  void updateSpeedProfile(const can_frame& frame) {
    const uint8_t followDistance =
      (frame.data[5] & FOLLOW_DISTANCE_MASK) >> FOLLOW_DISTANCE_SHIFT;

    switch (followDistance) {
      case 1: speedProfile = 2; break;
      case 2: speedProfile = 1; break;
      case 3: speedProfile = 0; break;
      default: break;
    }
  }

  void enableFsdSpeedProfile(can_frame& frame) const {
    frame.data[5] |= (1U << 6);
    frame.data[6] = (frame.data[6] & static_cast<uint8_t>(~SPEED_PROFILE_MASK)) |
                    ((speedProfile << SPEED_PROFILE_SHIFT) & SPEED_PROFILE_MASK);
  }

  static void suppressControlFlag(can_frame& frame) {
    frame.data[2] &= static_cast<uint8_t>(~(1U << 3));
  }
};

HW3Handler handler;

void setup() {
  delay(500);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  SPI1.setRX(SPI1_RX_PIN);
  SPI1.setTX(SPI1_TX_PIN);
  SPI1.setSCK(SPI1_SCK_PIN);
  SPI1.begin();

  mcp.reset();
  mcp.setBitrate(CAN_500KBPS, MCP_16MHZ);
  mcp.setNormalMode();
}

__attribute__((optimize("O3"))) void loop() {
  can_frame frame;
  if (mcp.readMessage(&frame) != MCP2515::ERROR_OK) {
    digitalWrite(PIN_LED, HIGH);
    return;
  }

  digitalWrite(PIN_LED, LOW);
  handler.handleMessage(frame);
}
