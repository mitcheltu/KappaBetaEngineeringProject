#include "FirmwareShared.h"

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override { historyConnected(server, info); }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int) override {
    historyDisconnected(info);
    NimBLEDevice::startAdvertising();
  }
};
class ZeroDepthCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const auto value = characteristic->getValue();
    if (value.size() == 1 && static_cast<uint8_t>(value[0]) == 1) zeroRequested.store(true);
  }
};
ServerCallbacks serverCallbacks;
ZeroDepthCallbacks zeroCallbacks;

void initializeBle() {
  NimBLEDevice::init(Config::DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&serverCallbacks, false);
  NimBLEService* service = bleServer->createService(Config::SERVICE_UUID);
  const uint32_t properties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY;
  depthCharacteristic = service->createCharacteristic(Config::DEPTH_UUID, properties, 20);
  imuCharacteristic = service->createCharacteristic(Config::IMU_UUID, properties, 20);
  ppgCharacteristic = service->createCharacteristic(Config::PPG_UUID, properties, 20);
  systemCharacteristic = service->createCharacteristic(Config::SYSTEM_UUID, properties, 20);
  NimBLECharacteristic* zero = service->createCharacteristic(Config::ZERO_DEPTH_UUID, NIMBLE_PROPERTY::WRITE, 1);
  zero->setCallbacks(&zeroCallbacks);
  createHistoryCharacteristics(service);
  publishTelemetry(millis());
  bleServer->start();
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->enableScanResponse(true);
  advertising->addServiceUUID(Config::SERVICE_UUID);
  advertising->setName(Config::DEVICE_NAME);
  advertising->start();
}

void sendPacket(NimBLECharacteristic* characteristic, const void* packet) {
  characteristic->setValue(static_cast<const uint8_t*>(packet), 20);
  const uint16_t connection = historyConnectionHandle();
  if (connection != BLE_HS_CONN_HANDLE_NONE && historyLiveEnabled()) characteristic->notify(connection);
}

void publishTelemetry(uint32_t now) {
  static uint16_t sequence = 0;
  const PacketHeader header{1, 0, sequence++, now};
  const bool freshPressure = pressureValid && now - pressureMs < 1000;
  const bool calibrated = freshPressure && std::isfinite(surfacePa);
  DepthPacket depth{};
  depth.header = header;
  depth.header.flags = (freshPressure ? 1 : 0) | (calibrated ? 2 : 0);
  depth.pressurePa = freshPressure ? pressurePa : NAN;
  depth.temperatureC = freshPressure ? temperatureC : NAN;
  depth.depthM = calibrated ? (pressurePa - surfacePa) / (Config::FLUID_DENSITY_KG_M3 * 9.80665f) : NAN;
  ImuPacket motion{};
  motion.header = header;
  if (imuValid && now - imuMs < 300) {
    motion.header.flags = 1;
    for (size_t i = 0; i < 6; ++i) motion.axes[i] = lroundf(imuAxes[i] * (i < 3 ? 1000.0f : 10.0f));
  }
  PpgPacket optical{};
  optical.header = header;
  optical.hrTenths = optical.spo2Hundredths = UINT16_MAX;
  if (rawPpgValid && now - ppgMs < 300) {
    optical.header.flags = 1; optical.red = redValue; optical.ir = irValue;
    if (now - estimateMs < 2000) {
      if (hrValid) { optical.header.flags |= 2; optical.hrTenths = heartRate * 10; }
      if (spo2Valid) { optical.header.flags |= 4; optical.spo2Hundredths = oxygenSaturation * 100; }
    }
  }
  SystemPacket system{};
  const uint32_t controls = actuatorState.load();
  system.header = header; system.batteryMv = batteryMv.load();
  system.header.flags = (system.batteryMv != UINT16_MAX ? 1 : 0) |
      (controlReady && Config::ENABLE_ACTUATOR_TESTS ? 2 : 0) |
      static_cast<uint8_t>(controls);
  system.countdownRemainingMs = static_cast<uint16_t>(controls >> 8);
  system.ppgOverflowCount = ppgOverflows; system.sensorErrors = sensorErrors;
  recordHistory(4, system.header.flags, esp_timer_get_time(), &system.batteryMv, 12);
  sendPacket(depthCharacteristic, &depth); sendPacket(imuCharacteristic, &motion);
  sendPacket(ppgCharacteristic, &optical); sendPacket(systemCharacteristic, &system);
}
