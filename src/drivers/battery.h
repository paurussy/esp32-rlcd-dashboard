/*
  battery.h - Li-ion battery monitor (ADC through a resistor divider)
*/
#pragma once

#include <Arduino.h>

class Battery {
public:
  explicit Battery(int adcPin);

  void begin();

  // Samples the ADC and updates the smoothed voltage and charge estimate.
  void update();

  float voltage() const { return _voltage; }
  int percent() const { return _percent; }

  // No battery connected: the board runs from USB only.
  bool isPresent() const;
  // High voltage means the charger is active or the cell is full.
  bool isCharging() const;

private:
  int _pin;
  float _voltage = 0.0f;
  int _percent = 0;

  static int voltageToPercent(float v);
};
