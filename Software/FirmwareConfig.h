#pragma once
#include <stdint.h>

// ESP32 GPIO numbers traced from Rev F (2026-11-03).
// User confirmed intended wiring; hardware is not yet assembled.
namespace Config {
constexpr int I2C_SDA = 8, I2C_SCL = 9;
constexpr int IMU_INT1 = 4, PPG_INT = 15;
constexpr int CANCEL_BUTTON = 5, SOS_BUTTON = 6;
constexpr int MOTOR_GATE = 7, SOLENOID_GATE = 16, BATTERY_ADC = 1;
constexpr uint8_t IMU_ADDRESS = 0x68, PPG_ADDRESS = 0x57, DEPTH_ADDRESS = 0x76;
constexpr uint32_t I2C_HZ = 400000;
constexpr float FLUID_DENSITY_KG_M3 = 997.0f; // Fresh water; seawater: 1029.
constexpr float BATTERY_DIVIDER_RATIO = 2.0f; // R5 = R6 = 100 kohm.

// Explicitly requested manual bench test: SOS -> vibrate 10s -> valve open.
// Cancel stops both. This enables GPIO commands, not a hardware certification.
// AOMAG B01KMI3HVI: listing 12 V / 6.5 W, continuous duty. Validate driver load.
constexpr bool ENABLE_ACTUATOR_TESTS = true;
constexpr uint16_t VALVE_MIN_BATTERY_MV = 3400;
// No guessed PWM hold duty. User requested full-voltage OPEN until Cancel.

// Example optical gate, not a validated wrist/contact classifier.
constexpr uint32_t PPG_MIN_IR = 50000, PPG_MAX_ADC = 262000;
constexpr uint32_t TELEMETRY_PERIOD_MS = 200;
constexpr char DEVICE_NAME[] = "KappaBeta-RevF";
constexpr char SERVICE_UUID[] = "e81b6200-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char DEPTH_UUID[] = "e81b6210-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char IMU_UUID[] = "e81b6211-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char PPG_UUID[] = "e81b6212-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char SYSTEM_UUID[] = "e81b6213-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char ZERO_DEPTH_UUID[] = "e81b6220-7e57-4d8e-b6f4-889c1d4e8270";
}
