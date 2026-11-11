#include "FirmwareShared.h"

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    logMessage(LogLevel::INFO, "BLE", "Connected: handle=%u; waiting for history/live handshake.",
        static_cast<unsigned>(info.getConnHandle()));
    historyConnected(server, info);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    logMessage(LogLevel::INFO, "BLE", "Disconnected: handle=%u reason=%d; restarting advertising.",
        static_cast<unsigned>(info.getConnHandle()), reason);
    historyDisconnected(info);
    NimBLEDevice::startAdvertising();
  }
};
class ZeroDepthCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const auto value = characteristic->getValue();
    if (value.size() == 1 && static_cast<uint8_t>(value[0]) == 1) {
      zeroRequested.store(true);
      logMessage(LogLevel::INFO, "BLE", "Surface-zero request received.");
    } else {
      logMessage(LogLevel::DEBUG, "BLE", "Surface-zero request rejected: expected one byte equal to 1, length=%u.",
          static_cast<unsigned>(value.size()));
    }
  }
};
ServerCallbacks serverCallbacks;
ZeroDepthCallbacks zeroCallbacks;

void initializeBle() {
  logMessage(LogLevel::INFO, "BLE", "Initializing name=%s requestedMTU=247. Wokwi does not simulate Bluetooth.",
      Config::DEVICE_NAME);
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
  const bool advertised = advertising->start();
  logMessage(advertised ? LogLevel::INFO : LogLevel::ERROR, "BLE", "Advertising %s; service=%s.",
      advertised ? "started" : "failed", Config::SERVICE_UUID);
}

void sendPacket(NimBLECharacteristic* characteristic, const void* packet) {
  characteristic->setValue(static_cast<const uint8_t*>(packet), 20);
  const uint16_t connection = historyConnectionHandle();
  if (connection != BLE_HS_CONN_HANDLE_NONE && historyLiveEnabled()) {
    if (!characteristic->notify(connection)) {
      // The loop owns this counter. Report congestion without logging every packet.
      static uint32_t failures = 0, lastReport = 0;
      ++failures;
      const uint32_t now = millis();
      if (lastReport == 0 || now - lastReport >= 5000) {
        logMessage(LogLevel::WARN, "BLE", "Live notify enqueue failed: handle=%u failuresSinceReport=%lu; check subscription/link.",
            static_cast<unsigned>(connection), static_cast<unsigned long>(failures));
        failures = 0; lastReport = now;
      }
    }
  }
}

void publishTelemetry(uint32_t now) {
  static uint16_t sequence = 0;
  const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
  const PacketHeader header{1, 0, sequence++, now};
  DepthReading depthReading{};
  const bool haveDepth = sensorBuffers.latestDepth(nowUs, depthReading);
  const bool depthFresh = haveDepth && nowUs >= depthReading.timestampUs &&
      nowUs - depthReading.timestampUs <
          static_cast<uint64_t>(Config::DEPTH_STALE_AFTER_MS) * 1000ULL;
  if (haveDepth && !depthFresh) {
    depthReading.qualityFlags |= DepthQuality::STALE;
    depthReading.sensorValid = false;
    depthReading.depthValid = false;
    depthReading.rateValid = false;
  }
  const bool freshPressure = depthFresh && depthReading.sensorValid;
  const bool calibrated = freshPressure && depthReading.depthValid;
  DepthPacket depth{};
  depth.header = header;
  depth.header.flags = (freshPressure ? 1 : 0) | (calibrated ? 2 : 0);
  depth.pressurePa = freshPressure ? depthReading.pressurePa : NAN;
  depth.temperatureC = freshPressure ? depthReading.temperatureC : NAN;
  depth.depthM = calibrated ? depthReading.filteredDepthM : NAN;
  ImuPacket motion{};
  motion.header = header;
  ImuReading imuReading{};
  const bool haveImu = sensorBuffers.latestImu(nowUs, imuReading);
  if (haveImu && imuReading.valid && nowUs >= imuReading.timestampUs &&
      nowUs - imuReading.timestampUs < 300000ULL) {
    motion.header.flags = 1;
    for (size_t i = 0; i < 3; ++i)
      motion.axes[i] = lroundf(imuReading.acceleration[i] * 1000.0f);
    for (size_t i = 0; i < 3; ++i)
      motion.axes[i + 3] = lroundf(imuReading.gyro[i] * 10.0f);
  }
  PpgPacket optical{};
  optical.header = header;
  optical.hrTenths = optical.spo2Hundredths = UINT16_MAX;
  PpgReading ppgReading{};
  PpgAnalysis ppgAnalysis{};
  const bool havePpg = sensorBuffers.latestPpg(nowUs, ppgReading);
  const bool ppgFresh = havePpg && ppgReading.valid &&
      nowUs >= ppgReading.timestampUs && nowUs - ppgReading.timestampUs < 300000ULL;
  if (ppgFresh) {
    optical.header.flags = 1;
    optical.red = ppgReading.red;
    optical.ir = ppgReading.infrared;
    const bool haveAnalysis = sensorBuffers.latestPpgAnalysis(nowUs, ppgAnalysis);
    const bool analysisFresh = haveAnalysis && nowUs >= ppgAnalysis.timestampUs &&
        nowUs - ppgAnalysis.timestampUs < 2000000ULL;
    if (analysisFresh) {
      if (ppgAnalysis.heartRateValid) {
        optical.header.flags |= 2;
        optical.hrTenths = ppgAnalysis.heartRateBpm * 10;
      }
      if (ppgAnalysis.oxygenSaturationValid) {
        optical.header.flags |= 4;
        optical.spo2Hundredths = ppgAnalysis.oxygenSaturationPercent * 100;
      }
    }
  }
  SystemPacket system{};
  const uint32_t controls = actuatorState.load();
  system.header = header; system.batteryMv = batteryMv.load();
  system.header.flags = (system.batteryMv != UINT16_MAX ? 1 : 0) |
      (controlReady && Config::ENABLE_ACTUATOR_TESTS ? 2 : 0) |
      static_cast<uint8_t>(controls);
  system.countdownRemainingMs = static_cast<uint16_t>(controls >> 8);
  system.ppgOverflowCount = ppgOverflows.load();
  system.sensorErrors = sensorErrors.load();
  recordHistory(4, system.header.flags, esp_timer_get_time(), &system.batteryMv, 12);
  sendPacket(depthCharacteristic, &depth); sendPacket(imuCharacteristic, &motion);
  sendPacket(ppgCharacteristic, &optical); sendPacket(systemCharacteristic, &system);
}
