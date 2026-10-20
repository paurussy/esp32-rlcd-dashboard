/*
  ui.h - Dashboard rendering (header, clock, sensor cards, footer)
  ----------------------------------------------------------------
  Depends only on U8g2 and never touches hardware: the sketch fills a
  DashboardState and calls the ui*() functions.
*/
#pragma once

#include <U8g2lib.h>
#include <math.h>

#include "date_time.h"
#include "history.h"

struct DashboardState {
  // Network
  bool wifiConnected = false;
  int wifiRssi = -100;
  char ipAddress[16] = "";

  // Clock
  bool rtcPresent = false;
  bool timeSynced = false;      // Set from NTP at least once
  bool rtcTimeInvalid = false;  // RTC lost power and was never re-synced
  DateTime now;

  // Environment
  bool sensorPresent = false;
  float temperature = NAN;
  float humidity = NAN;
  const History *history = nullptr;
  uint32_t historyIntervalMin = 15;

  // Power
  bool batteryPresent = false;
  bool batteryCharging = false;
  int batteryPercent = 0;

  uint32_t uptimeSec = 0;
};

void uiDrawSplash(U8G2 &g, const char *message);
void uiDrawDashboard(U8G2 &g, const DashboardState &s);
