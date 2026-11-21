#include "FirmwareShared.h"
#include "PumpRamp.h"

namespace PumpDriver {
PumpRamp ramp;
uint32_t lastDuty = UINT32_MAX;
bool attached = false;

void failOff() {
  pumpOutputReady.store(false); // Latch until reboot; never fall back to full power.
  if (attached) {
    ledcWrite(Config::PUMP_IN1, 0);
    ledcDetach(Config::PUMP_IN1);
    attached = false;
  }
  pinMode(Config::PUMP_IN1, OUTPUT);
  digitalWrite(Config::PUMP_IN1, LOW);
  ramp.update(millis(), false);
  lastDuty = 0;
  logMessage(LogLevel::ERROR, "PUMP", "PWM failed; outputs disabled until restart");
}
} // namespace PumpDriver

void initializePumpOutput() {
  using namespace PumpDriver;
  attached = ledcAttach(Config::PUMP_IN1, Config::PUMP_PWM_HZ, Config::PUMP_PWM_BITS);
  if (!attached || !ledcWrite(Config::PUMP_IN1, 0)) { failOff(); return; }
  lastDuty = 0;
  pumpOutputReady.store(true);
  logMessage(LogLevel::INFO, "PUMP", "DRV8871 IN1=GPIO%d: %lu Hz PWM, 0..100%% over %lu ms",
      Config::PUMP_IN1, static_cast<unsigned long>(Config::PUMP_PWM_HZ),
      static_cast<unsigned long>(Config::PUMP_RAMP_MS));
}

// Setup initializes LEDC before the task starts. Thereafter only controlTask
// owns this driver, including the immediate raw-Cancel stop before ADC reads.
bool writePumpOutput(uint32_t now, bool requested) {
  using namespace PumpDriver;
  if (!pumpOutputReady.load()) return false;
  const uint32_t duty = ramp.update(now, requested);
  if (duty != lastDuty) {
    if (!ledcWrite(Config::PUMP_IN1, duty)) { failOff(); return false; }
    lastDuty = duty;
  }
  // ESP32 core 3.x maps MAX_DUTY to a full timer period: steady HIGH with
  // no low pulse. Keep LEDC ownership so stopping always writes duty zero.
  return requested;
}
