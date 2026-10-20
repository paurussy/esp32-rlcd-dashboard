#include "shtc3.h"

namespace {

constexpr uint16_t CMD_WAKEUP     = 0x3517;
constexpr uint16_t CMD_SLEEP      = 0xB098;
constexpr uint16_t CMD_SOFT_RESET = 0x805D;
constexpr uint16_t CMD_READ_ID    = 0xEFC8;
// Normal mode, temperature first, no clock stretching
constexpr uint16_t CMD_MEASURE_T_FIRST = 0x7866;

constexpr uint32_t MEASUREMENT_TIME_MS = 15;

}  // namespace

Shtc3::Shtc3(TwoWire &wire, uint8_t address) : _wire(wire), _address(address) {}

bool Shtc3::sendCommand(uint16_t cmd)
{
  _wire.beginTransmission(_address);
  _wire.write(cmd >> 8);
  _wire.write(cmd & 0xFF);
  return _wire.endTransmission() == 0;
}

// CRC-8, polynomial 0x31, init 0xFF (datasheet section 5.9)
uint8_t Shtc3::crc8(const uint8_t *data, size_t len)
{
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
  }
  return crc;
}

bool Shtc3::begin()
{
  _present = false;

  sendCommand(CMD_WAKEUP);
  delay(1);
  sendCommand(CMD_SOFT_RESET);
  delay(5);
  sendCommand(CMD_WAKEUP);
  delay(1);

  if (sendCommand(CMD_READ_ID)) {
    uint8_t buf[3];
    if (_wire.requestFrom(_address, static_cast<uint8_t>(sizeof(buf))) == sizeof(buf)) {
      for (uint8_t &b : buf) b = _wire.read();
      if (crc8(buf, 2) == buf[2]) {
        _id = (buf[0] << 8) | buf[1];
        _present = true;
      }
    }
  }

  sendCommand(CMD_SLEEP);
  return _present;
}

bool Shtc3::read(float &temperatureC, float &humidityPct)
{
  if (!_present) return false;

  sendCommand(CMD_WAKEUP);
  delay(1);
  if (!sendCommand(CMD_MEASURE_T_FIRST)) {
    sendCommand(CMD_SLEEP);
    return false;
  }
  delay(MEASUREMENT_TIME_MS);

  bool ok = false;
  uint8_t buf[6];
  if (_wire.requestFrom(_address, static_cast<uint8_t>(sizeof(buf))) == sizeof(buf)) {
    for (uint8_t &b : buf) b = _wire.read();
    if (crc8(buf, 2) == buf[2] && crc8(buf + 3, 2) == buf[5]) {
      uint16_t rawT = (buf[0] << 8) | buf[1];
      uint16_t rawH = (buf[3] << 8) | buf[4];
      temperatureC = 175.0f * rawT / 65536.0f - 45.0f;
      humidityPct = 100.0f * rawH / 65536.0f;
      ok = true;
    }
  }

  sendCommand(CMD_SLEEP);
  return ok;
}
