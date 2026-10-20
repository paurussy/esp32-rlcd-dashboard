/*
  history.h - Fixed-size history of temperature/humidity samples for the charts

  The chart always spans its full width. It starts at one point per minute and,
  each time the buffer fills up, merges points (averaging) into the next
  resolution in HISTORY_LEVELS_MIN, so the time span grows with the data.
*/
#pragma once

#include <string.h>

#include "../../config.h"

struct History {
  // Oldest sample first, newest at index count - 1
  float temperature[HISTORY_SIZE];
  float humidity[HISTORY_SIZE];
  int count = 0;
  uint32_t intervalMin = HISTORY_LEVELS_MIN[0];  // Minutes covered by each point

  // Records a one-minute reading; every intervalMin readings become one point.
  void add(float temperatureC, float humidityPct)
  {
    pendingT += temperatureC;
    pendingH += humidityPct;
    if (++pending < intervalMin) return;

    push(pendingT / pending, pendingH / pending);
    pending = 0;
    pendingT = pendingH = 0;
  }

  // Appends a point, coarsening the resolution (or dropping the oldest point at the coarsest one) when full.
  void push(float temperatureC, float humidityPct)
  {
    if (count == HISTORY_SIZE && !coarsen()) {
      memmove(temperature, temperature + 1, sizeof(float) * (HISTORY_SIZE - 1));
      memmove(humidity, humidity + 1, sizeof(float) * (HISTORY_SIZE - 1));
      count--;
    }
    temperature[count] = temperatureC;
    humidity[count] = humidityPct;
    count++;
  }

private:
  float pendingT = 0, pendingH = 0;
  uint32_t pending = 0;

  // Merges the points into the next level. False when already at the coarsest.
  bool coarsen()
  {
    uint32_t next = 0;
    for (int i = 0; i + 1 < HISTORY_LEVELS; i++) {
      if (HISTORY_LEVELS_MIN[i] == intervalMin) next = HISTORY_LEVELS_MIN[i + 1];
    }
    if (next == 0) return false;

    const int factor = next / intervalMin;
    int out = 0;
    for (int i = 0; i < count; i += factor, out++) {
      const int n = factor < count - i ? factor : count - i;
      float t = 0, h = 0;
      for (int j = 0; j < n; j++) {
        t += temperature[i + j];
        h += humidity[i + j];
      }
      temperature[out] = t / n;
      humidity[out] = h / n;
    }
    count = out;
    intervalMin = next;
    return true;
  }
};
