#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ScenarioProtocol.h"

// Scripted input for on-device inference during a demo scenario. This is the
// ONLY input the demo inference reads; live sensor data never reaches it.
//
// Sample conventions (a source must document how it meets them):
//   ir(i)    100 Hz infrared PPG, sample i at i*10 ms after playback starts,
//            already in the pulse model's training polarity (clinical PLETH:
//            larger = more blood). PULSE_INVERT_IR is NOT applied to scripted data.
//   imu(i)   20 Hz IMU row i at i*50 ms: ax ay az [m/s^2], gx gy gz [rad/s]
//            (the IMU CNN's training units; NOT the BMI270 driver's g / deg/s).
//   spo2(s)  1 Hz SpO2 percent at s*1000 ms, with an explicit validity flag
//            (an invalid reading models "estimator unavailable", e.g. no pulse).
// Storage is behind this interface so a later uploader (LittleFS/BLE) can replace
// compiled-in data without changing the controller or the inference.
namespace Scenario {
class Source {
 public:
  virtual ~Source() = default;
  virtual size_t count() const = 0;
  virtual bool descriptor(size_t index, Descriptor& out) const = 0;
  // Display name for the catalog (may be empty).
  virtual const char* name(size_t) const { return ""; }
  // Select the scenario for playback; false if the id is unknown.
  virtual bool select(uint8_t id) = 0;
  virtual uint32_t irCount() const = 0;
  virtual uint32_t spo2Count() const = 0;
  virtual uint32_t imuCount() const { return 0; }
  virtual bool ir(uint32_t index, float& value) const = 0;
  virtual bool spo2(uint32_t second, float& value, bool& valid) const = 0;
  virtual bool imu(uint32_t, float*) const { return false; }
};

// Firmware fallback with no scenario data: an empty catalog. Every START is
// rejected as UNKNOWN_SCENARIO, so inference stays idle and never actuates.
class NullSource final : public Source {
 public:
  size_t count() const override { return 0; }
  bool descriptor(size_t, Descriptor&) const override { return false; }
  bool select(uint8_t) override { return false; }
  uint32_t irCount() const override { return 0; }
  uint32_t spo2Count() const override { return 0; }
  bool ir(uint32_t, float&) const override { return false; }
  bool spo2(uint32_t, float&, bool&) const override { return false; }
};

inline uint16_t catalogCrc(const Source& source) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < source.count(); ++i) {
    Descriptor d{};
    if (source.descriptor(i, d)) crc = crc32Update(crc, &d, sizeof(d));
  }
  return static_cast<uint16_t>((crc ^ 0xffffffffu) & 0xffffu);
}

inline bool findScenario(const Source& source, uint8_t id, Descriptor& out) {
  for (size_t i = 0; i < source.count(); ++i) {
    Descriptor d{};
    if (source.descriptor(i, d) && d.id == id) { out = d; return true; }
  }
  return false;
}
}  // namespace Scenario
