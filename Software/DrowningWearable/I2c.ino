#include "FirmwareShared.h"

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* data, size_t count) {
  Wire.beginTransmission(address);
  const bool written = Wire.write(reg) == 1;
  const uint8_t result = Wire.endTransmission(!written);
  if (!written || result != 0) return false;
  if (Wire.requestFrom(address, count, true) != count) {
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!Wire.available()) return false;
    const int value = Wire.read();
    if (value < 0) return false;
    data[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  const bool written = Wire.write(reg) == 1 && Wire.write(value) == 1;
  const uint8_t result = Wire.endTransmission();
  return written && result == 0;
}
