#include "FirmwareShared.h"

bool readRegisters(uint8_t address, uint8_t reg, uint8_t* data, size_t count) {
  Wire.beginTransmission(address);
  const bool written = Wire.write(reg) == 1;
  const uint8_t result = Wire.endTransmission(!written);
  if (!written || result != 0) {
    logMessage(LogLevel::DEBUG, "I2C", "Read setup failed addr=0x%02X reg=0x%02X queued=%u status=%u",
               unsigned(address), unsigned(reg), unsigned(written), unsigned(result));
    return false;
  }
  const size_t received = Wire.requestFrom(address, count, true);
  if (received != count) {
    logMessage(LogLevel::DEBUG, "I2C", "Short read addr=0x%02X reg=0x%02X received=%u expected=%u",
               unsigned(address), unsigned(reg), unsigned(received), unsigned(count));
    while (Wire.available()) Wire.read();
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!Wire.available()) {
      logMessage(LogLevel::DEBUG, "I2C", "Read buffer empty addr=0x%02X reg=0x%02X byte=%u/%u",
                 unsigned(address), unsigned(reg), unsigned(i), unsigned(count));
      return false;
    }
    const int value = Wire.read();
    if (value < 0) {
      logMessage(LogLevel::DEBUG, "I2C", "Read returned %d addr=0x%02X reg=0x%02X byte=%u",
                 value, unsigned(address), unsigned(reg), unsigned(i));
      return false;
    }
    data[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  const bool written = Wire.write(reg) == 1 && Wire.write(value) == 1;
  const uint8_t result = Wire.endTransmission();
  if (!written || result != 0)
    logMessage(LogLevel::DEBUG, "I2C", "Write failed addr=0x%02X reg=0x%02X value=0x%02X queued=%u status=%u",
               unsigned(address), unsigned(reg), unsigned(value), unsigned(written), unsigned(result));
  return written && result == 0;
}
