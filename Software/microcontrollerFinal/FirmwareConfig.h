#pragma once
#include <stdint.h>

// ESP32-S3-DevKitC-1-N8R8 GPIO numbers, not physical header positions.
// Rev F wiring, with the user-requested pump replacing the solenoid on GPIO16.
// Electrical/README.md lists the pin and power assumptions still to validate.
namespace Config {
// Selected depth-sensor hardware: GY-MS5837-30BA breakout, 3.3 V I2C logic.
// The Rev F project wiring and ESP32-S3 configuration use GPIO8/GPIO9; do not
// substitute the older Rev B GPIO3/GPIO4 mapping here.
constexpr int I2C_SDA = 8, I2C_SCL = 9; // Shared, pulled up to 3.3 V.
constexpr int IMU_INT1 = 4, PPG_INT = 15; // Inputs; currently polled over I2C.
constexpr int CANCEL_BUTTON = 5, SOS_BUTTON = 6; // NO reeds to GND, active LOW.
constexpr int VIBRATION_GATE = 7; // Rev F MOTOR_EN -> R1 -> Q1; held LOW for now.
constexpr int COUNTDOWN_LED_PIN = 48; // Onboard RGB: DevKitC-1 v1.0/Wokwi; v1.1 uses 38.
constexpr uint8_t COUNTDOWN_LED_BLUE = 64; // RGB blue brightness (0..255).
constexpr int PUMP_GATE = 16; // Former SOL_EN -> R3 -> Q2; active HIGH.
constexpr int BATTERY_ADC = 1; // ADC1, midpoint of R5/R6 (100k/100k).
constexpr uint8_t IMU_ADDRESS = 0x68, PPG_ADDRESS = 0x57, DEPTH_ADDRESS = 0x76;
constexpr uint32_t I2C_HZ = 400000;
// The official BlueRobotics MS5837 1.1.1 protocol is pinned by libraries.txt.
// Keep the initial depth cadence at 5 Hz while conversions are serviced
// asynchronously between IMU and PPG work.
constexpr uint32_t DEPTH_SAMPLE_PERIOD_MS = 200; // PROVISIONAL/TBD; initial 5 Hz.
constexpr float DEPTH_WATER_DENSITY_KG_M3 = 1000.0f; // PROVISIONAL/TBD freshwater.
constexpr float FLUID_DENSITY_KG_M3 = DEPTH_WATER_DENSITY_KG_M3; // Legacy alias.
constexpr float BATTERY_DIVIDER_RATIO = 2.0f; // R5 = R6 = 100 kohm.

// Temporary manual bench test: SOS -> blue LED 10s -> pump runs until Cancel.
// Cancel stops both; the vibration motor stays off.
// This enables GPIO commands, not a hardware certification.
// CrocSee B09XH1GYYQ: 12 V recommended, <=350 mA listed load current.
// Startup/stall current, Q2 drive and D3/boost suitability still need validation.
constexpr bool ENABLE_ACTUATOR_TESTS = true;
constexpr uint16_t ACTUATOR_MIN_BATTERY_MV = 3400; // Existing battery interlock.
constexpr uint16_t BATTERY_VALID_MIN_MV = 2500, BATTERY_VALID_MAX_MV = 4350;
// Steady HIGH/LOW switching, no PWM or pump-specific library. No flow feedback.

// Serial diagnostics: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG. Runtime: log info/debug/off.
constexpr uint8_t DEFAULT_LOG_LEVEL = 2;
constexpr uint32_t LOG_STATUS_PERIOD_MS = 2000;

// Example optical gate, not a validated wrist/contact classifier.
constexpr uint32_t PPG_MIN_IR = 50000, PPG_MAX_ADC = 262000;
constexpr uint32_t TELEMETRY_PERIOD_MS = 200;
constexpr uint32_t HISTORY_BYTES = 4 * 1024 * 1024;
constexpr uint32_t ML_WORKSPACE_BYTES = 1024 * 1024; // Two future model workspaces.
constexpr uint32_t PSRAM_HEADROOM_BYTES = 1024 * 1024;
// Final IMU model input: 20 Hz * 3 seconds = 60 samples.
constexpr uint32_t IMU_PERIOD_MS = 50, DEPTH_PERIOD_MS = DEPTH_SAMPLE_PERIOD_MS;
constexpr char DEVICE_NAME[] = "KappaBeta-RevF";
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
