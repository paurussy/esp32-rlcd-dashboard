/*
  history.h - Fixed-size history of temperature/humidity samples for the charts
*/
#pragma once

#include <string.h>

#include "../config.h"

struct History {
  // Oldest sample first, newest at index count - 1
  float temperature[HISTORY_SIZE];
  float humidity[HISTORY_SIZE];
  int count = 0;

  // Appends a sample, dropping the oldest one when full.
  void push(float temperatureC, float humidityPct)
  {
    if (count == HISTORY_SIZE) {
      memmove(temperature, temperature + 1, sizeof(float) * (HISTORY_SIZE - 1));
      memmove(humidity, humidity + 1, sizeof(float) * (HISTORY_SIZE - 1));
      count--;
    }
    temperature[count] = temperatureC;
    humidity[count] = humidityPct;
    count++;
  }
};
