/*
  history.h - Fixed-size history of temperature/humidity samples for the charts

  The chart always spans its full width. It starts at one point per minute and,
  each time the buffer fills up, merges points (averaging) into the next
  resolution in HISTORY_LEVELS_MIN, so the time span grows with the data.

  A point may be NAN: a gap (the board was off). The sketch saves the whole struct
  in flash now and then and restores it after a restart, filling the time it was off
  with gaps (see addGap).
*/
#pragma once

#include <math.h>
#include <string.h>
#include <type_traits>

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

  // `minutes` without readings (the board was off): gap points, so the chart keeps
  // its time scale. More than the whole span just leaves the chart blank.
  void addGap(uint32_t minutes)
  {
    minutes += pending;  // The readings of an unfinished point are lost with it
    pending = 0;
    pendingT = pendingH = 0;
    // Each point takes intervalMin minutes, which grows as the history coarsens
    for (int i = 0; minutes >= intervalMin && i < HISTORY_SIZE * HISTORY_LEVELS; i++) {
      push(NAN, NAN);
      minutes -= intervalMin;
    }
  }

  // Minutes the points cover.
  uint32_t spanMin() const { return count * intervalMin; }

  // Whether any point is a real reading (not a gap).
  bool hasReadings() const
  {
    for (int i = 0; i < count; i++) {
      if (!isnan(temperature[i])) return true;
    }
    return false;
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

  // Sanity check of a copy restored from flash.
  bool valid() const
  {
    if (count < 0 || count > HISTORY_SIZE || pending >= intervalMin) return false;
    for (int i = 0; i < HISTORY_LEVELS; i++) {
      if (HISTORY_LEVELS_MIN[i] == intervalMin) return true;
    }
    return false;
  }

private:
  float pendingT = 0, pendingH = 0;
  uint32_t pending = 0;

  // Average of the points that are not gaps (a gap if all of them are).
  static float mean(const float *v, int n)
  {
    float sum = 0;
    int used = 0;
    for (int j = 0; j < n; j++) {
      if (isnan(v[j])) continue;
      sum += v[j];
      used++;
    }
    return used ? sum / used : NAN;
  }

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
      temperature[out] = mean(temperature + i, n);
      humidity[out] = mean(humidity + i, n);
    }
    count = out;
    intervalMin = next;
    return true;
  }
};

// Saved to flash byte for byte
static_assert(std::is_trivially_copyable<History>::value, "History must be trivially copyable");
