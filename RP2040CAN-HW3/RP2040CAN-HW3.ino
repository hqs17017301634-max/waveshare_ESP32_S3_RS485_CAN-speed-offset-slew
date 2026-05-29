#include <SPI.h>
#include <mcp2515.h>

#define CAN_CS    9    // GPIO9 片选


MCP2515* mcp = nullptr;

struct HW3Handler {
  int speedProfile = 1;
  
  void handelMessage(can_frame& frame) {
    // 处理跟车距离 → 速度曲线切换
    if (frame.can_id == 1016) {
      uint8_t followDistance = (frame.data[5] & 0b11100000) >> 5;
      switch (followDistance) {
        case 1: speedProfile = 2; break;
        case 2: speedProfile = 1; break;
        case 3: speedProfile = 0; break;
        default: break;
      }
      return;
    }
    
    // 处理 FSD 相关消息（1021）
    if (frame.can_id == 1021) {
      auto index = frame.data[0] & 0x07;
      
      // index == 0 时修改速度曲线并转发
      if (index == 0) {
        frame.data[5] |= (1 << 6);           // 使能某标志位
        frame.data[6] &= ~0x06;              // 清零速度曲线位
        frame.data[6] |= (speedProfile << 1); // 写入当前速度曲线
        
        mcp->sendMessage(&frame);
      }
      
      // index == 1 时直接转发（去掉某个bit）
      if (index == 1) {
        frame.data[2] &= ~(1 << 3);
        mcp->sendMessage(&frame);
      }
      
      // index == 2 时不再处理速度偏移，直接跳过（已精简）
    }
  }
};

HW3Handler handler;

void setup() {
  delay(1500);
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1000) {}

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  SPI1.setRX(12);
  SPI1.setTX(11);
  SPI1.setSCK(10);
  SPI1.begin();

  mcp = new MCP2515(CAN_CS, 10000000, &SPI1);

  if (mcp) {
    mcp->reset();
    MCP2515::ERROR e = mcp->setBitrate(CAN_500KBPS, MCP_16MHZ);
    mcp->setNormalMode();
  }
}
__attribute__((optimize("O3"))) void loop() {
  if (!mcp) return;
  
  can_frame frame;
  if (mcp->readMessage(&frame) == MCP2515::ERROR_OK) {
    digitalWrite(PIN_LED, LOW);
    handler.handelMessage(frame);
  } else {
    digitalWrite(PIN_LED, HIGH);
  }
}