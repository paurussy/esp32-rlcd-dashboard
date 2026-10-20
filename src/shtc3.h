/*
  shtc3.h - Minimal driver for the Sensirion SHTC3 temperature/humidity sensor
*/
#pragma once

#include <Arduino.h>
#include <Wire.h>

class Shtc3 {
public:
  explicit Shtc3(TwoWire &wire, uint8_t address = 0x70);

  // Wakes the sensor, resets it and validates its ID. Returns true if found.
  bool begin();

  // Performs one measurement. Outputs are left untouched on failure.
  bool read(float &temperatureC, float &humidityPct);

  bool isPresent() const { return _present; }
  uint16_t id() const { return _id; }

private:
  TwoWire &_wire;
  uint8_t _address;
  bool _present = false;
  uint16_t _id = 0;

  bool sendCommand(uint16_t cmd);
  static uint8_t crc8(const uint8_t *data, size_t len);
};
