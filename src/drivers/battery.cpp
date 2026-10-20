#include "battery.h"

#include "../../config.h"

namespace {

// Typical resting discharge curve of an 18650 Li-ion cell (voltage -> %)
constexpr float CURVE_V[]   = {3.30f, 3.50f, 3.68f, 3.74f, 3.77f, 3.79f, 3.82f, 3.87f, 3.92f, 3.98f, 4.06f, 4.15f};
constexpr int   CURVE_PCT[] = {    0,     5,    10,    20,    30,    40,    50,    60,    70,    80,    90,   100};
constexpr int   CURVE_POINTS = sizeof(CURVE_V) / sizeof(CURVE_V[0]);

}  // namespace

Battery::Battery(int adcPin) : _pin(adcPin) {}

void Battery::begin()
{
  analogReadResolution(12);
  analogSetPinAttenuation(_pin, ADC_11db);
}

void Battery::update()
{
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(_pin);
  }
  float raw = (sum / BATTERY_ADC_SAMPLES) * BATTERY_DIVIDER_RATIO / 1000.0f;

  // Exponential moving average keeps the percentage from jittering
  bool firstReading = _voltage < 0.1f;
  _voltage = firstReading ? raw : (1.0f - BATTERY_SMOOTHING) * _voltage + BATTERY_SMOOTHING * raw;
  _percent = voltageToPercent(_voltage);
}

bool Battery::isPresent() const
{
  return _voltage >= BATTERY_ABSENT_BELOW_V;
}

bool Battery::isCharging() const
{
  return isPresent() && _voltage >= BATTERY_FULL_ABOVE_V;
}

int Battery::voltageToPercent(float v)
{
  if (v <= CURVE_V[0]) return 0;
  if (v >= CURVE_V[CURVE_POINTS - 1]) return 100;

  for (int i = 1; i < CURVE_POINTS; i++) {
    if (v <= CURVE_V[i]) {
      float t = (v - CURVE_V[i - 1]) / (CURVE_V[i] - CURVE_V[i - 1]);
      return CURVE_PCT[i - 1] + static_cast<int>(t * (CURVE_PCT[i] - CURVE_PCT[i - 1]));
    }
  }
  return 100;
}
