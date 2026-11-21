#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <cmath>
// Primary header lets Arduino discover the library before resolving nested Bosch headers.
#include <SparkFun_BMI270_Arduino_Library.h>
// Bosch's driver/configuration image is provided by SparkFun BMI270 1.0.3.
#include <bmi270_api/bmi270.h>

// Small transport adapter: reuse Bosch initialization, compensation and decoding,
// but reject truncated transfers instead of SparkFun's success-on-short-read path.
// Polls the latest registers; does not promise a new sample on every read/FIFO capture.
class CheckedBMI270 {
 public:
  CheckedBMI270() = default;
  CheckedBMI270(const CheckedBMI270&) = delete;
  CheckedBMI270& operator=(const CheckedBMI270&) = delete;

  bool begin(TwoWire& bus, uint8_t address) {
    ready = false;
    if (address != 0x68 && address != 0x69) return false;
    transport = {&bus, address};
    device = {};
    device.intf = BMI2_I2C_INTF;
    device.intf_ptr = &transport; // Owned for the full lifetime of this adapter.
    device.read = readBytes;
    device.write = writeBytes;
    device.delay_us = delayUs;
    device.read_write_len = 32;
    if (bmi270_init(&device) != BMI2_OK) return false;
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    if (bmi270_sensor_enable(sensors, 2, &device) != BMI2_OK) return false;
    bmi2_sens_config config[2] = {};
    config[0].type = BMI2_ACCEL;
    config[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    config[0].cfg.acc.range = BMI2_ACC_RANGE_8G;
    config[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    config[1].type = BMI2_GYRO;
    config[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    config[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    config[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    if (bmi270_set_sensor_config(config, 2, &device) != BMI2_OK) return false;
    config[0] = {}; config[1] = {};
    config[0].type = BMI2_ACCEL; config[1].type = BMI2_GYRO;
    if (bmi270_get_sensor_config(config, 2, &device) != BMI2_OK) return false;
    ready = config[0].cfg.acc.odr == BMI2_ACC_ODR_100HZ &&
            config[0].cfg.acc.range == BMI2_ACC_RANGE_8G &&
            config[0].cfg.acc.bwp == BMI2_ACC_NORMAL_AVG4 &&
            config[0].cfg.acc.filter_perf == BMI2_PERF_OPT_MODE &&
            config[1].cfg.gyr.odr == BMI2_GYR_ODR_100HZ &&
            config[1].cfg.gyr.range == BMI2_GYR_RANGE_2000 &&
            config[1].cfg.gyr.bwp == BMI2_GYR_NORMAL_MODE &&
            config[1].cfg.gyr.filter_perf == BMI2_PERF_OPT_MODE &&
            config[1].cfg.gyr.noise_perf == BMI2_PERF_OPT_MODE;
    return ready;
  }

  bool read(float axes[6]) {
    for (size_t i = 0; i < 6; ++i) axes[i] = NAN;
    if (!ready) return false;
    bmi2_sens_data raw = {};
    if (bmi2_get_sensor_data(&raw, &device) != BMI2_OK) {
      ready = false;
      return false;
    }
    constexpr float accelScale = 8.0f / 32768.0f;
    constexpr float gyroScale = 2000.0f / 32768.0f;
    axes[0] = raw.acc.x * accelScale; axes[1] = raw.acc.y * accelScale;
    axes[2] = raw.acc.z * accelScale; axes[3] = raw.gyr.x * gyroScale;
    axes[4] = raw.gyr.y * gyroScale; axes[5] = raw.gyr.z * gyroScale;
    return true;
  }

 private:
  struct Transport { TwoWire* bus = nullptr; uint8_t address = 0; } transport;
  bmi2_dev device = {};
  bool ready = false;

  static BMI2_INTF_RETURN_TYPE readBytes(uint8_t reg, uint8_t* bytes, uint32_t count, void* context) {
    auto& connection = *static_cast<Transport*>(context);
    TwoWire& bus = *connection.bus;
    bus.beginTransmission(connection.address);
    const bool written = bus.write(reg) == 1;
    const uint8_t status = bus.endTransmission(!written);
    if (!written || status != 0) return BMI2_E_COM_FAIL;
    if (bus.requestFrom(connection.address, static_cast<size_t>(count), true) != count) {
      while (bus.available()) bus.read();
      return BMI2_E_COM_FAIL;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (!bus.available()) return BMI2_E_COM_FAIL;
      const int value = bus.read();
      if (value < 0) return BMI2_E_COM_FAIL;
      bytes[i] = static_cast<uint8_t>(value);
    }
    return BMI2_OK;
  }

  static BMI2_INTF_RETURN_TYPE writeBytes(uint8_t reg, const uint8_t* bytes, uint32_t count, void* context) {
    auto& connection = *static_cast<Transport*>(context);
    TwoWire& bus = *connection.bus;
    bus.beginTransmission(connection.address);
    const bool addressWritten = bus.write(reg) == 1;
    const bool dataWritten = bus.write(bytes, count) == count;
    const uint8_t status = bus.endTransmission();
    return addressWritten && dataWritten && status == 0 ? BMI2_OK : BMI2_E_COM_FAIL;
  }

  static void delayUs(uint32_t microseconds, void*) { delayMicroseconds(microseconds); }
};
