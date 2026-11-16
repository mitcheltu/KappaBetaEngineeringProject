#pragma once
#include <cstddef>
#include <cstdint>
#include "OxygenConcern.h"
#include "PulseLoss.h"

namespace Config {
constexpr int I2C_SDA = 8, I2C_SCL = 9;
constexpr uint32_t I2C_HZ = 400000;
constexpr uint8_t PPG_ADDRESS = 0x57, LED_CURRENT = 60;
constexpr uint32_t PPG_SAMPLE_HZ = 100, SAMPLE_US = 1000000 / PPG_SAMPLE_HZ;
constexpr uint8_t DECIMATION = 4;
constexpr uint32_t MIN_IR = 50000, MAX_ADC = 262000;
constexpr uint32_t SENSOR_POLL_MS = 5, FIFO_DEADLINE_MS = 250;
constexpr uint32_t NO_SAMPLE_MS = 200, RETRY_MS = 5000;
constexpr uint32_t ESTIMATE_STALE_US = 1500000, PULSE_STALE_US = 500000;
constexpr size_t RAW_QUEUE_SAMPLES = 512, MAXIM_SAMPLES = 100;
constexpr size_t PULSE_STEP_SAMPLES = 200;
// Every notebook 04 model was trained on the clinical PLETH convention: raw optical
// intensity negated (rises with blood volume). MAX30102 IR counts are raw reflected
// intensity, so they are negated before pulse analysis. Bench-check once: the
// negated trace must show a fast systolic upstroke, then a slower decline.
constexpr bool PULSE_INVERT_IR = true;
constexpr uint32_t ANALYSIS_BUDGET_US = 2000000;
constexpr uint32_t QUALIFICATION_MAX_SECONDS = 3600;
constexpr int SENSOR_CORE = 1, ANALYSIS_CORE = 1;
constexpr uint32_t SENSOR_STACK_BYTES = 4096, ANALYSIS_STACK_BYTES = 16384;
constexpr unsigned SENSOR_PRIORITY = 4, ANALYSIS_PRIORITY = 2;
constexpr int ALARM_LED_PIN = 48; // DevKitC-1 v1.0; v1.1 uses 38.
constexpr int ACK_BUTTON = 5;     // Normally open button to GND, INPUT_PULLUP.
constexpr int PUMP_GATE = 16, VIBRATION_GATE = 7; // Always held LOW here.
// cnn_v5_h (notebook 04 v5) drives only the local research LED; the pump stays LOW.
// Post-hoc choice: the predeclared v5 rule picked cnn_1d (PulseModel::Cnn).
constexpr Ppg::PulseModel PULSE_MODEL = Ppg::PulseModel::CnnV5H;
constexpr Ppg::PulseOperatingPoint OPERATING_POINT = Ppg::PulseOperatingPoint::HighSpecificity;
constexpr float OXYGEN_START = 80.0f, OXYGEN_FULL = 60.0f;
constexpr float OXYGEN_TAU_SECONDS = 3.0f, OXYGEN_ENTER = 0.5f, OXYGEN_EXIT = 0.25f;
constexpr uint32_t OXYGEN_CONFIRM_SECONDS = 3, OXYGEN_RECOVERY_SECONDS = 5;
inline Ppg::OxygenConfig oxygenConfig() {
  Ppg::OxygenConfig config;
  config.start = OXYGEN_START; config.full = OXYGEN_FULL;
  config.tauSeconds = OXYGEN_TAU_SECONDS;
  config.enterScore = OXYGEN_ENTER; config.exitScore = OXYGEN_EXIT;
  config.confirmSeconds = OXYGEN_CONFIRM_SECONDS;
  config.recoverySeconds = OXYGEN_RECOVERY_SECONDS;
  config.autoClear = false; // A local device alert always requires acknowledgment.
  return config;
}
static_assert(PPG_SAMPLE_HZ / DECIMATION == 25, "Maxim routine requires 25 Hz");
}
