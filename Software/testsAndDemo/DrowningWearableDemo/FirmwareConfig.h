#pragma once
#include <stdint.h>

// Rev I GPIO numbers, not connector positions. See README.md for the source.
namespace Config {
constexpr int I2C_SDA = 8, I2C_SCL = 9;
constexpr int IMU_INT1 = 4, PPG_INT = 15;
constexpr int CANCEL_BUTTON = 5, SOS_BUTTON = 6;
constexpr int VIBRATION_GATE = 7, PUMP_GATE = 13;
constexpr int BATTERY_ADC = 1;
constexpr uint8_t IMU_ADDRESS = 0x68, PPG_ADDRESS = 0x57, DEPTH_ADDRESS = 0x76;
constexpr uint32_t I2C_HZ = 400000;
constexpr bool ENABLE_ACTUATORS = true;
constexpr float BATTERY_DIVIDER_RATIO = 2.0f;
constexpr uint16_t BATTERY_VALID_MIN_MV = 2500, BATTERY_VALID_MAX_MV = 4350;
constexpr uint16_t ACTUATOR_MIN_BATTERY_MV = 3400;
constexpr uint32_t CONTROL_PERIOD_MS = 5, SOS_DEBOUNCE_MS = 30;
constexpr uint32_t BATTERY_PERIOD_MS = 100;
constexpr uint32_t IMU_PERIOD_MS = 50, DEPTH_PERIOD_MS = 200;
constexpr uint32_t TELEMETRY_PERIOD_MS = 200, SENSOR_RETRY_MS = 5000;
constexpr uint32_t LOG_STATUS_PERIOD_MS = 2000;
constexpr float FLUID_DENSITY_KG_M3 = 997.0f;
// Stock MAX3010x: hardware 100 Hz / averaging 4 = 25 FIFO pairs/s.
constexpr uint16_t PPG_SAMPLE_HZ = 100;
constexpr uint8_t PPG_AVERAGING = 4;
constexpr uint32_t PPG_MIN_IR = 50000, PPG_MAX_ADC = 262000;
constexpr uint32_t PPG_SERVICE_GAP_MS = 100;
constexpr char DEVICE_NAME[] = "KappaBetaDemo";
constexpr char SERVICE_UUID[] = "e81b6200-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char DEPTH_UUID[] = "e81b6210-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char IMU_UUID[] = "e81b6211-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char PPG_UUID[] = "e81b6212-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char SYSTEM_UUID[] = "e81b6213-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char ZERO_DEPTH_UUID[] = "e81b6220-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_INFO_UUID[] = "e81b6230-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_STATUS_UUID[] = "e81b6231-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_DATA_UUID[] = "e81b6232-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_COMMAND_UUID[] = "e81b6233-7e57-4d8e-b6f4-889c1d4e8270";
}
