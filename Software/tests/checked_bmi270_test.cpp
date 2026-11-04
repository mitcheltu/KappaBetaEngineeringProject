// Host tests exercise the actual adapter/callbacks against controlled I2C faults.
// Bosch API calls are faked here; the ESP32 build links the genuine Bosch driver.
#include "CheckedBMI270.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <type_traits>

static bmi2_sens_config configured[2];
static bool corruptReadback = false;
static int failApi = -1;
int8_t bmi270_init(bmi2_dev* dev) {
  assert(dev->intf == BMI2_I2C_INTF && dev->read_write_len == 32);
  uint8_t id = 0;
  if (dev->read(0, &id, 1, dev->intf_ptr) != BMI2_OK) return BMI2_E_COM_FAIL;
  const uint8_t image[] = {1, 2, 3, 4};
  if (dev->write(0x5e, image, 4, dev->intf_ptr) != BMI2_OK) return BMI2_E_COM_FAIL;
  return failApi == 0 ? BMI2_E_COM_FAIL : BMI2_OK;
}
int8_t bmi270_sensor_enable(const uint8_t* sensors, uint8_t count, bmi2_dev*) {
  assert(count == 2 && sensors[0] == BMI2_ACCEL && sensors[1] == BMI2_GYRO);
  return failApi == 1 ? BMI2_E_COM_FAIL : BMI2_OK;
}
int8_t bmi270_set_sensor_config(bmi2_sens_config* config, uint8_t count, bmi2_dev* dev) {
  assert(count == 2);
  assert(config[0].cfg.acc.range == BMI2_ACC_RANGE_8G && config[0].cfg.acc.odr == BMI2_ACC_ODR_100HZ);
  assert(config[1].cfg.gyr.range == BMI2_GYR_RANGE_2000 && config[1].cfg.gyr.odr == BMI2_GYR_ODR_100HZ);
  configured[0] = config[0]; configured[1] = config[1];
  const uint8_t regs[] = {0, 1, 2, 3};
  if (dev->write(0x40, regs, 4, dev->intf_ptr) != BMI2_OK) return BMI2_E_COM_FAIL;
  return failApi == 2 ? BMI2_E_COM_FAIL : BMI2_OK;
}
int8_t bmi270_get_sensor_config(bmi2_sens_config* config, uint8_t, bmi2_dev* dev) {
  uint8_t regs[4];
  if (dev->read(0x40, regs, 4, dev->intf_ptr) != BMI2_OK) return BMI2_E_COM_FAIL;
  config[0] = configured[0]; config[1] = configured[1];
  if (corruptReadback) config[0].cfg.acc.range = BMI2_ACC_RANGE_2G;
  return failApi == 3 ? BMI2_E_COM_FAIL : BMI2_OK;
}
int8_t bmi2_get_sensor_data(bmi2_sens_data* data, bmi2_dev* dev) {
  uint8_t bytes[12];
  if (dev->read(0x0c, bytes, 12, dev->intf_ptr) != BMI2_OK) return BMI2_E_COM_FAIL;
  auto axis = [&](size_t i) { return static_cast<int16_t>(static_cast<uint16_t>(bytes[i]) | (static_cast<uint16_t>(bytes[i+1]) << 8)); };
  data->acc.x = axis(0); data->acc.y = axis(2); data->acc.z = axis(4);
  data->gyr.x = axis(6); data->gyr.y = axis(8); data->gyr.z = axis(10);
  return BMI2_OK;
}
void initializationResponses(TwoWire& wire) { wire.responses = {{0x24}, {0, 1, 2, 3}}; }
void sampleResponse(TwoWire& wire) { wire.responses.push_back({0, 0x10, 0, 0xf0, 0, 0, 0, 0x40, 0, 0xc0, 0, 0x80}); }
void unknown(const float* axes) { for (size_t i = 0; i < 6; ++i) assert(std::isnan(axes[i])); }
int main() {
  static_assert(!std::is_copy_constructible<CheckedBMI270>::value, "Interface pointer must not be copied");
  float axes[6];
  {
    CheckedBMI270 imu;
    assert(!imu.read(axes)); unknown(axes);
    TwoWire wire;
    initializationResponses(wire);
    assert(imu.begin(wire, 0x68));
    sampleResponse(wire);
    assert(imu.read(axes));
    assert(axes[0] == 1 && axes[1] == -1 && axes[2] == 0);
    assert(axes[3] == 1000 && axes[4] == -1000 && axes[5] == -2000);
    sampleResponse(wire); wire.shortRead = wire.requests;
    assert(!imu.read(axes)); unknown(axes);
    // A failed read stays unavailable until a fresh initialization succeeds.
    assert(!imu.read(axes)); unknown(axes);
    wire.shortRead = -1; initializationResponses(wire);
    assert(imu.begin(wire, 0x68)); sampleResponse(wire);
    assert(imu.read(axes));
  }
  for (int fault = 0; fault < 4; ++fault) {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    failApi = fault;
    assert(!imu.begin(wire, 0x68)); assert(!imu.read(axes)); unknown(axes);
  }
  failApi = -1;
  for (int stage = 0; stage < 6; ++stage) {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    wire.failWrite = stage;
    assert(!imu.begin(wire, 0x68)); assert(!imu.read(axes));
  }
  for (int stage = 0; stage < 4; ++stage) {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    wire.failAck = stage;
    assert(!imu.begin(wire, 0x68));
  }
  for (int stage = 0; stage < 2; ++stage) {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    wire.shortRead = stage;
    assert(!imu.begin(wire, 0x68));
  }
  for (int index = 0; index < 12; ++index) {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    assert(imu.begin(wire, 0x68)); sampleResponse(wire);
    wire.missingByte = wire.byteReads + index;
    assert(!imu.read(axes)); unknown(axes);
  }
  {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    corruptReadback = true;
    assert(!imu.begin(wire, 0x68));
    corruptReadback = false;
  }
  {
    TwoWire wire; CheckedBMI270 imu; initializationResponses(wire);
    wire.expectedAddress = 0x69;
    assert(imu.begin(wire, 0x69));
    assert(!imu.begin(wire, 0x70)); assert(!imu.read(axes));
  }
  std::cout << "BMI270 checked transport, configuration, scaling and recovery tests passed\n";
}
