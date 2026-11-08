#include <Arduino.h>
#include <Wire.h>
#include <cstring>
#include <esp_timer.h>

#include "DepthSensor.h"

namespace Config {
constexpr int I2C_SDA = 8;
constexpr int I2C_SCL = 9;
constexpr uint8_t DEPTH_ADDRESS = 0x76;
constexpr uint32_t I2C_HZ = 400000;
constexpr uint32_t DEPTH_SAMPLE_PERIOD_MS = 200; // PROVISIONAL/TBD.
constexpr float DEPTH_WATER_DENSITY_KG_M3 = 1000.0f; // PROVISIONAL/TBD.
}

DepthSensor depthSensor(DepthSensorConfig{
    Config::DEPTH_SAMPLE_PERIOD_MS, Config::DEPTH_WATER_DENSITY_KG_M3});

void serviceSerial() {
  static char command[24] = {};
  static size_t length = 0;
  while (Serial.available()) {
    const char character = static_cast<char>(Serial.read());
    if (character == '\r') continue;
    if (character != '\n') {
      if (length + 1 < sizeof(command)) command[length++] = character;
      continue;
    }
    command[length] = '\0';
    if (strcmp(command, "start dive") == 0) {
      Serial.println(depthSensor.startDive() ? "Dive started; surface baseline captured."
                                             : "Dive start failed; depth remains invalid.");
    } else if (strcmp(command, "stop dive") == 0) {
      depthSensor.stopDive();
      Serial.println("Dive stopped; depth and rate invalidated.");
    } else if (length != 0) {
      Serial.println("Commands: start dive | stop dive");
    }
    length = 0;
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(Config::I2C_SDA, Config::I2C_SCL, Config::I2C_HZ);
  Wire.setTimeOut(20);
  Serial.println(depthSensor.begin(Wire, Config::DEPTH_ADDRESS)
                    ? "MS5837-30BA ready; use 'start dive' at the surface."
                    : "MS5837-30BA initialization failed.");
}

void loop() {
  serviceSerial();
  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  if (depthSensor.due(nowUs)) {
    if (depthSensor.update()) {
      const DepthReading& reading = depthSensor.reading();
      Serial.printf("P=%.1f Pa T=%.2f C raw=%.3f m filtered=%.3f m rate=%.3f m/s quality=0x%04x\n",
                    reading.pressurePa, reading.temperatureC, reading.rawDepthM,
                    reading.filteredDepthM, reading.depthRateMps,
                    unsigned(reading.qualityFlags));
    } else {
      Serial.println("Depth sample invalid; inspect quality flags.");
    }
  }
  delay(1);
}
