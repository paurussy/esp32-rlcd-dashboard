/*
  ui.h - Screen rendering: dashboard (clock, sensor cards) and calendar
  ----------------------------------------------------------------
  Depends only on U8g2 and never touches hardware: the sketch fills a
  DashboardState (and a CalendarView) and calls the ui*() functions.
*/
#pragma once

#include <U8g2lib.h>
#include <math.h>

#include "calendar.h"
#include "date_time.h"
#include "history.h"

struct DashboardState {
  // Network
  bool wifiConnected = false;
  bool wifiSleeping = false;    // Radio intentionally off (low-power mode)
  int wifiRssi = -100;
  char ipAddress[16] = "";

  // Display
  bool showSeconds = true;      // False when refreshed only once per minute

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

  // Page indicator in the header (no dots with a single page)
  int page = 0;
  int pageCount = 1;
};

struct CalendarView {
  time_t now = 0;                   // Unix time; formatted with the local time zone
  const CalendarEvent *events[CALENDAR_VISIBLE_EVENTS] = {};  // Not finished yet, current first
  int count = 0;
  bool hasData = false;             // Received at least one push from the PC
  time_t lastUpdate = 0;
  bool stale = false;               // No push for CALENDAR_STALE_MIN minutes
};

void uiDrawSplash(U8G2 &g, const char *message);
void uiDrawDashboard(U8G2 &g, const DashboardState &s);

// Header and footer status come from `s`; the clock is not drawn.
void uiDrawCalendar(U8G2 &g, const DashboardState &s, const CalendarView &c);
