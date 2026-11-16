#include <Arduino.h>
#include <stdlib.h>

// Arduino IDE concatenates these tabs automatically. Include them here to
// build the same firmware sources as an ESP-IDF application.
#include "../../microcontrollerFinal.ino"
#include "../../Actuators.ino"
#include "../../Bluetooth.ino"
#include "../../Depth.ino"
#include "../../DepthSensor.ino"
#include "../../History.ino"
#include "../../I2c.ino"
#include "../../Imu.ino"
#include "../../ImuSensor.ino"
#include "../../ImuWindow.ino"
#include "../../Logging.ino"
#include "../../Ppg.ino"
#include "../../SerialCommands.ino"
#include "../../SinkRateEstimator.ino"
#include "../../ImuRiskEvaluator.ino"

namespace {

void firmwareMainTask(void*) {
  setup();
  for (;;) {
    loop();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

}  // namespace

extern "C" void app_main(void) {
  initArduino();
  const BaseType_t taskStarted = xTaskCreatePinnedToCore(
      firmwareMainTask, "arduino-main", Config::PROCESSING_TASK_STACK_BYTES,
      nullptr, 1, nullptr, Config::PROCESSING_TASK_CORE);
  if (taskStarted != pdPASS) abort();
  vTaskDelete(nullptr);
}
