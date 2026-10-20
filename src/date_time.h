/*
  date_time.h - Plain calendar date/time value and helpers
*/
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct DateTime {
  uint16_t year = 2000;
  uint8_t month = 1;     // 1..12
  uint8_t day = 1;       // 1..31
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint8_t weekday = 6;   // 0 = Sunday .. 6 = Saturday
};

// Day of the week (0 = Sunday) using Zeller's congruence.
inline uint8_t dayOfWeek(int year, int month, int day)
{
  if (month < 3) {
    month += 12;
    year--;
  }
  return (day + 13 * (month + 1) / 5 + year + year / 4 - year / 100 + year / 400 + 6) % 7;
}

// Date/time at which the calling translation unit was compiled.
// Used as a fallback when the RTC has lost power and NTP is unavailable.
// Inline so that __DATE__/__TIME__ expand in the caller's file.
inline DateTime compileDateTime()
{
  // __DATE__ = "Oct 19 2026", __TIME__ = "22:11:05"
  static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *date = __DATE__;
  const char *time = __TIME__;

  DateTime dt;
  for (int i = 0; i < 12; i++) {
    if (strncmp(date, MONTHS + i * 3, 3) == 0) {
      dt.month = i + 1;
      break;
    }
  }
  dt.day = atoi(date + 4);
  dt.year = atoi(date + 7);
  dt.hour = atoi(time);
  dt.minute = atoi(time + 3);
  dt.second = atoi(time + 6);
  dt.weekday = dayOfWeek(dt.year, dt.month, dt.day);
  return dt;
}
