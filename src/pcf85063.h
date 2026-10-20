/*
  pcf85063.h - Minimal driver for the NXP PCF85063 real-time clock
*/
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "date_time.h"

class Pcf85063 {
public:
  explicit Pcf85063(TwoWire &wire, uint8_t address = 0x51);

  // Returns true if the RTC answers on the bus.
  bool begin();

  // True if the oscillator stopped (power loss), so the stored time is invalid.
  bool lostPower();

  bool read(DateTime &out);
  bool write(const DateTime &dt);  // Also clears the power-loss flag

  bool isPresent() const { return _present; }

private:
  TwoWire &_wire;
  uint8_t _address;
  bool _present = false;

  bool readRegisters(uint8_t reg, uint8_t *buf, size_t len);
};
