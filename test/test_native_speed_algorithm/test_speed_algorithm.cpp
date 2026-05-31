#include <unity.h>
#include "../../ESP32S3CAN-FSD/speed_algorithm.h"

void setUp() {}
void tearDown() {}

void test_decode_fused_speed_limit() {
  int limitKph = 0;
  TEST_ASSERT_FALSE(decodeFusedSpeedLimitKph(0x00, limitKph));
  TEST_ASSERT_FALSE(decodeFusedSpeedLimitKph(0x1F, limitKph));
  TEST_ASSERT_TRUE(decodeFusedSpeedLimitKph(0x0A, limitKph));
  TEST_ASSERT_EQUAL_INT(50, limitKph);
}

void test_target_speed_table() {
  TEST_ASSERT_EQUAL_INT(60, getTargetSpeedForLimit(50));
  TEST_ASSERT_EQUAL_INT(80, getTargetSpeedForLimit(60));
  TEST_ASSERT_EQUAL_INT(80, getTargetSpeedForLimit(70));
  TEST_ASSERT_EQUAL_INT(90, getTargetSpeedForLimit(80));
  TEST_ASSERT_EQUAL_INT(100, getTargetSpeedForLimit(90));
  TEST_ASSERT_EQUAL_INT(120, getTargetSpeedForLimit(100));
  TEST_ASSERT_EQUAL_INT(140, getTargetSpeedForLimit(120));
  TEST_ASSERT_EQUAL_INT(140, getTargetSpeedForLimit(140));
  TEST_ASSERT_EQUAL_INT(150, getTargetSpeedForLimit(150));
}

void test_pct4_encoding_examples() {
  TEST_ASSERT_EQUAL_UINT8(80, calculateSpeedCompensation(50).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(132, calculateSpeedCompensation(60).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(56, calculateSpeedCompensation(70).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(52, calculateSpeedCompensation(80).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(44, calculateSpeedCompensation(90).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(80, calculateSpeedCompensation(100).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(68, calculateSpeedCompensation(120).speedOffsetRaw);
  TEST_ASSERT_EQUAL_UINT8(0, calculateSpeedCompensation(140).speedOffsetRaw);
}

void test_offset_clamp_and_pct_cap() {
  const SpeedCompensation result = calculateSpeedCompensation(30);
  TEST_ASSERT_TRUE(result.hasFusedSpeedLimit);
  TEST_ASSERT_EQUAL_INT(60, result.targetSpeedKph);
  TEST_ASSERT_EQUAL_INT(25, result.offsetKph);
  TEST_ASSERT_EQUAL_UINT8(200, result.speedOffsetRaw);
}

void test_speed_offset_raw_roundtrip_preserves_other_bits() {
  uint8_t data[8] = {0x3F, 0xC0, 0, 0, 0, 0, 0, 0};
  writeSpeedOffsetRaw(data, 132);
  TEST_ASSERT_EQUAL_UINT8(132, readSpeedOffsetRaw(data));
  TEST_ASSERT_EQUAL_UINT8(0x3F, data[0] & 0x3F);
  TEST_ASSERT_EQUAL_UINT8(0xC0, data[1] & 0xC0);
}

void test_slew_falling_uses_5_percent_per_second() {
  OffsetSlewLimiter limiter;
  TEST_ASSERT_EQUAL_UINT8(200, limiter.apply(200, 1000));
  TEST_ASSERT_EQUAL_UINT8(180, limiter.apply(80, 2000));
  TEST_ASSERT_EQUAL_UINT8(80, limiter.apply(80, 7000));
}

void test_slew_rising_passes_immediately() {
  OffsetSlewLimiter limiter;
  TEST_ASSERT_EQUAL_UINT8(50, limiter.apply(50, 1000));
  TEST_ASSERT_EQUAL_UINT8(120, limiter.apply(120, 1100));
}

void test_startup_fallback_uses_capped_raw_until_valid_limit() {
  StartupSpeedFallback fallback;
  TEST_ASSERT_EQUAL_UINT8(200, fallback.selectRaw(0, 1000));
  TEST_ASSERT_EQUAL_UINT8(200, fallback.selectRaw(20, 2000));
  TEST_ASSERT_EQUAL_UINT8(220, fallback.selectRaw(220, 3000));
  TEST_ASSERT_TRUE(fallback.markValidFusedSpeedLimit());
  TEST_ASSERT_EQUAL_UINT8(20, fallback.selectRaw(20, 4000));
  TEST_ASSERT_FALSE(fallback.markValidFusedSpeedLimit());
}

void test_startup_fallback_expires_without_valid_limit() {
  StartupSpeedFallback fallback;
  TEST_ASSERT_EQUAL_UINT8(200, fallback.selectRaw(0, 1000));
  TEST_ASSERT_EQUAL_UINT8(0, fallback.selectRaw(0, 17001));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_decode_fused_speed_limit);
  RUN_TEST(test_target_speed_table);
  RUN_TEST(test_pct4_encoding_examples);
  RUN_TEST(test_offset_clamp_and_pct_cap);
  RUN_TEST(test_speed_offset_raw_roundtrip_preserves_other_bits);
  RUN_TEST(test_slew_falling_uses_5_percent_per_second);
  RUN_TEST(test_slew_rising_passes_immediately);
  RUN_TEST(test_startup_fallback_uses_capped_raw_until_valid_limit);
  RUN_TEST(test_startup_fallback_expires_without_valid_limit);
  return UNITY_END();
}
