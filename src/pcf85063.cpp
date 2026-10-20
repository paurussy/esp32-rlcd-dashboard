#include "pcf85063.h"

namespace {

constexpr uint8_t REG_SECONDS = 0x04;  // Followed by minutes, hours, days, weekdays, months, years
constexpr uint8_t SECONDS_OS_FLAG = 0x80;  // Oscillator stopped

uint8_t bcdToDec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t decToBcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

}  // namespace

Pcf85063::Pcf85063(TwoWire &wire, uint8_t address) : _wire(wire), _address(address) {}

bool Pcf85063::begin()
{
  _wire.beginTransmission(_address);
  _present = _wire.endTransmission() == 0;
  return _present;
}

bool Pcf85063::readRegisters(uint8_t reg, uint8_t *buf, size_t len)
{
  _wire.beginTransmission(_address);
  _wire.write(reg);
  if (_wire.endTransmission(false) != 0) return false;
  if (_wire.requestFrom(_address, static_cast<uint8_t>(len)) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = _wire.read();
  return true;
}

bool Pcf85063::lostPower()
{
  uint8_t seconds;
  return readRegisters(REG_SECONDS, &seconds, 1) && (seconds & SECONDS_OS_FLAG);
}

bool Pcf85063::read(DateTime &out)
{
  if (!_present) return false;

  uint8_t r[7];
  if (!readRegisters(REG_SECONDS, r, sizeof(r))) return false;

  out.second  = bcdToDec(r[0] & 0x7F);
  out.minute  = bcdToDec(r[1] & 0x7F);
  out.hour    = bcdToDec(r[2] & 0x3F);
  out.day     = bcdToDec(r[3] & 0x3F);
  out.weekday = r[4] & 0x07;
  out.month   = bcdToDec(r[5] & 0x1F);
  out.year    = 2000 + bcdToDec(r[6]);
  return true;
}

bool Pcf85063::write(const DateTime &dt)
{
  if (!_present) return false;

  _wire.beginTransmission(_address);
  _wire.write(REG_SECONDS);
  _wire.write(decToBcd(dt.second));  // OS flag = 0
  _wire.write(decToBcd(dt.minute));
  _wire.write(decToBcd(dt.hour));
  _wire.write(decToBcd(dt.day));
  _wire.write(dt.weekday);
  _wire.write(decToBcd(dt.month));
  _wire.write(decToBcd(dt.year % 100));
  return _wire.endTransmission() == 0;
}
