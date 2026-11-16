#pragma once
#include <stdint.h>

#include "imuDeployCNN/imu_inference_config.h"

// ESP32-S3-DevKitC-1-N8R8 GPIO numbers, not physical header positions.
// Rev F wiring. GPIO13 drives DRV8871 IN1; hold IN2 LOW externally.
// Electrical/README.md lists the pin and power assumptions still to validate.
namespace Config {
// Selected depth-sensor hardware: GY-MS5837-30BA breakout, 3.3 V I2C logic.
// The Rev F project wiring and ESP32-S3 configuration use GPIO8/GPIO9; do not
// substitute the older Rev B GPIO3/GPIO4 mapping here.
constexpr int I2C_SDA = 8, I2C_SCL = 9; // Shared, pulled up to 3.3 V.
constexpr int IMU_INT1 = 4, PPG_INT = 15; // Inputs; currently polled over I2C.
constexpr int CANCEL_BUTTON = 5, SOS_BUTTON = 6; // NO reeds to GND, active LOW.
constexpr int VIBRATION_GATE = 7; // Active-high vibration motor gate.
constexpr int PUMP_GATE = 13; // DRV8871 IN1; IN2 is held LOW externally.
constexpr uint32_t VIBRATION_DURATION_MS = 2000;
constexpr uint32_t PUMP_RAMP_DURATION_MS = 2000;
constexpr uint32_t PUMP_PWM_FREQUENCY_HZ = 20000;
constexpr uint8_t PUMP_PWM_RESOLUTION_BITS = 10;
constexpr uint32_t PUMP_PWM_MAX_DUTY =
    (1UL << PUMP_PWM_RESOLUTION_BITS) - 1;
constexpr int BATTERY_ADC = 1; // ADC1, midpoint of R5/R6 (100k/100k).
constexpr uint8_t IMU_ADDRESS = 0x68, PPG_ADDRESS = 0x57, DEPTH_ADDRESS = 0x76;
constexpr uint32_t I2C_HZ = 400000;
// The official BlueRobotics MS5837 1.1.1 protocol is pinned by libraries.txt.
// Depth conversions are serviced asynchronously between IMU and PPG work.
constexpr uint32_t DEPTH_SAMPLE_PERIOD_MS = 100; // 10 Hz target cadence.
constexpr uint8_t DEPTH_HAMPEL_PREVIOUS_READINGS = 5;
constexpr uint8_t DEPTH_HAMPEL_FUTURE_READINGS = 2;
constexpr uint8_t DEPTH_HAMPEL_WINDOW_READINGS =
    DEPTH_HAMPEL_PREVIOUS_READINGS + 1 + DEPTH_HAMPEL_FUTURE_READINGS;
constexpr uint32_t DEPTH_FILTER_DELAY_MS = 200;
static_assert(DEPTH_FILTER_DELAY_MS ==
                  DEPTH_HAMPEL_FUTURE_READINGS * DEPTH_SAMPLE_PERIOD_MS,
              "Depth filter delay must cover the configured future readings.");
constexpr float DEPTH_WATER_DENSITY_KG_M3 = 1000.0f; // PROVISIONAL/TBD freshwater.
constexpr float FLUID_DENSITY_KG_M3 = DEPTH_WATER_DENSITY_KG_M3; // Legacy alias.
// Provisional example profile for exercising depth-risk processing only.
// Replace with measured wearer/profile inputs before interpreting the score.
constexpr float DEPTH_RISK_PROFILE_TOTAL_MASS_KG = 75.0f;
constexpr float DEPTH_RISK_PROFILE_DISPLACED_VOLUME_M3 = 0.070f;
constexpr float DEPTH_RISK_PROFILE_PROJECTED_AREA_M2 = 0.70f;
constexpr float BATTERY_DIVIDER_RATIO = 2.0f; // R5 = R6 = 100 kohm.

// Manual actuator sequence: vibrate 2 s, PWM-ramp the pump for 2 s, then
// hold the pump on until Cancel/off. Both outputs stop on battery interlock.
// This enables GPIO commands, not a hardware certification.
constexpr bool ENABLE_ACTUATOR_TESTS = true;
constexpr uint16_t ACTUATOR_MIN_BATTERY_MV = 3400; // Existing battery interlock.
constexpr uint16_t BATTERY_VALID_MIN_MV = 2500, BATTERY_VALID_MAX_MV = 4350;
// GPIO13 uses Arduino-ESP32 LEDC PWM during the nonblocking ramp. No flow feedback.

// Serial diagnostics: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG. Runtime: log info/debug/off.
constexpr uint8_t DEFAULT_LOG_LEVEL = 2;
constexpr uint32_t LOG_STATUS_PERIOD_MS = 2000;

// Example optical gate, not a validated wrist/contact classifier.
constexpr uint32_t PPG_MIN_IR = 50000, PPG_MAX_ADC = 262000;
constexpr uint32_t TELEMETRY_PERIOD_MS = 200;
constexpr uint32_t RISK_RESULT_STALE_AFTER_MS = 2000;
constexpr uint32_t HISTORY_BYTES = 4 * 1024 * 1024;
constexpr uint32_t ML_WORKSPACE_BYTES = 1024 * 1024; // Per-model tensor arena.
constexpr uint32_t PSRAM_HEADROOM_BYTES = 1024 * 1024;
// Final IMU model input: 20 Hz * 3 seconds = 60 samples.
constexpr uint32_t IMU_SAMPLE_RATE_HZ = 20;
constexpr uint32_t IMU_WINDOW_SECONDS = 3;
constexpr uint32_t IMU_PERIOD_MS = 1000 / IMU_SAMPLE_RATE_HZ;
constexpr uint32_t IMU_WINDOW_SAMPLES = IMU_SAMPLE_RATE_HZ * IMU_WINDOW_SECONDS;
// Allow small scheduling/I2C jitter while rejecting missed acquisition slots
// and windows that have drifted materially beyond their intended duration.
constexpr uint32_t IMU_SAMPLE_INTERVAL_TOLERANCE_US = 10000;
constexpr uint32_t IMU_WINDOW_SPAN_TOLERANCE_US = 150000;
constexpr uint32_t RISK_ROUND_MAX_IMU_ENDPOINT_SKEW_US =
    IMU_PERIOD_MS * 1000 + IMU_SAMPLE_INTERVAL_TOLERANCE_US;
constexpr uint32_t DEPTH_PERIOD_MS = DEPTH_SAMPLE_PERIOD_MS;
constexpr uint32_t DEPTH_REGRESSION_WINDOW_MS = 1000;
constexpr uint32_t DEPTH_STALE_AFTER_MS = 1000;
// Sample-time tolerance; the 200 ms processing delay is handled by the depth
// finalized-through watermark, so it is not added to this skew allowance.
constexpr uint32_t RISK_ROUND_MAX_DEPTH_ENDPOINT_SKEW_US =
    DEPTH_SAMPLE_PERIOD_MS * 1000 + 100000;
static_assert(DEPTH_STALE_AFTER_MS > DEPTH_FILTER_DELAY_MS +
                                         2 * DEPTH_SAMPLE_PERIOD_MS,
              "Depth freshness limit must cover filter delay and cadence slack.");

// Initial model/gate and fusion settings. CNN controls the gate only; it is
// not added numerically to the depth/PPG weighted score.
constexpr float CNN_SCORE_THRESHOLD = kImuGateThreshold;
constexpr float DEPTH_FUSION_WEIGHT = 0.5f;
constexpr float PPG_FUSION_WEIGHT = 0.5f;
// UNSET disables automatic actuator starts. Configure a combined-risk threshold
// in the valid 0..1 range before automatic starts can be enabled.
constexpr float FUSION_TRIGGER_THRESHOLD = -1.0f;
constexpr bool FUSION_TRIGGER_THRESHOLD_CONFIGURED =
    FUSION_TRIGGER_THRESHOLD >= 0.0f && FUSION_TRIGGER_THRESHOLD <= 1.0f;

// MAX30102 setup: 100 Hz internally with four-sample averaging produces about
// 25 FIFO readings/second. Risk analysis uses a 100-reading window advanced
// by 25 readings each time it is evaluated.
constexpr uint16_t PPG_INTERNAL_SAMPLE_RATE_HZ = 100;
constexpr uint8_t PPG_SAMPLE_AVERAGING = 4;
constexpr uint8_t PPG_FIFO_READINGS_PER_SECOND =
    PPG_INTERNAL_SAMPLE_RATE_HZ / PPG_SAMPLE_AVERAGING;
constexpr uint16_t PPG_ANALYSIS_WINDOW_READINGS = 100;
constexpr uint16_t PPG_ANALYSIS_ADVANCE_READINGS = 25;
constexpr uint32_t RISK_ROUND_MAX_PPG_WINDOW_SPAN_US = 4110000;
constexpr uint32_t RISK_ROUND_MAX_PPG_ENDPOINT_SKEW_US = 100000;
constexpr float PPG_DUMMY_RISK_SCORE = 0.0f; // Explicit placeholder; not a sensor-derived score.
constexpr uint32_t IMU_BUFFER_CAPACITY = IMU_WINDOW_SAMPLES * 2;
// 32 finalized samples span 3.1 s at 10 Hz, exceeding the 1 s regression
// history plus filter delay, endpoint tolerance, and one cadence of slack.
constexpr uint32_t DEPTH_BUFFER_CAPACITY = 32;
constexpr uint32_t DEPTH_BUFFER_RETENTION_MS =
    (DEPTH_BUFFER_CAPACITY - 1) * DEPTH_SAMPLE_PERIOD_MS;
constexpr uint32_t DEPTH_BUFFER_REQUIRED_MS =
    DEPTH_REGRESSION_WINDOW_MS + DEPTH_FILTER_DELAY_MS +
    RISK_ROUND_MAX_DEPTH_ENDPOINT_SKEW_US / 1000 + DEPTH_SAMPLE_PERIOD_MS;
static_assert(DEPTH_BUFFER_RETENTION_MS >= DEPTH_BUFFER_REQUIRED_MS,
              "Depth buffer must retain aligned regression history.");
constexpr uint32_t PPG_BUFFER_CAPACITY = PPG_ANALYSIS_WINDOW_READINGS * 2;
constexpr uint32_t PPG_SERVICE_PERIOD_MS = 5;
constexpr uint32_t ACQUISITION_TASK_STACK_BYTES = 8192;
constexpr uint32_t PROCESSING_TASK_STACK_BYTES = 8192;
constexpr uint8_t ACQUISITION_TASK_PRIORITY = 4;
constexpr uint8_t PROCESSING_TASK_PRIORITY = 2;
constexpr uint8_t ACQUISITION_TASK_CORE = 1;
constexpr uint8_t PROCESSING_TASK_CORE = 0;
constexpr char DEVICE_NAME[] = "KappaBeta-RevF";
constexpr char SERVICE_UUID[] = "e81b6200-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char DEPTH_UUID[] = "e81b6210-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char IMU_UUID[] = "e81b6211-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char PPG_UUID[] = "e81b6212-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char SYSTEM_UUID[] = "e81b6213-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char RISK_UUID[] = "e81b6214-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char ZERO_DEPTH_UUID[] = "e81b6220-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_INFO_UUID[] = "e81b6230-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_STATUS_UUID[] = "e81b6231-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_DATA_UUID[] = "e81b6232-7e57-4d8e-b6f4-889c1d4e8270";
constexpr char HISTORY_COMMAND_UUID[] = "e81b6233-7e57-4d8e-b6f4-889c1d4e8270";
}
