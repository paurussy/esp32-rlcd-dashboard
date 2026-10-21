/*
  calendar.h - Today's meetings pushed by the PC bridge, and their beep alerts
  ----------------------------------------------------------------------------
  No hardware access: the sketch feeds it the body of each push and asks it,
  once per minute, whether an alert is due.

  Push format (UTF-8 text, one record per line, fields separated by tabs):
    v1    <pc_now_epoch>
    <id>  <start_epoch>  <end_epoch>  <subject>  <location>
  Epochs are UTC seconds; <id> is a 32-bit number that stays the same across
  pushes, so alerts already played are not repeated.
*/
#pragma once

#include <stdint.h>
#include <time.h>

#include "../../config.h"

struct CalendarEvent {
  uint32_t id = 0;
  time_t start = 0;
  time_t end = 0;
  char subject[64] = "";
  char location[32] = "";
  bool alertedEarly = false;  // CALENDAR_ALERT_EARLY_MIN before start
  bool alertedStart = false;
};

enum class CalendarAlert { None, Early, Start };

class Calendar {
public:
  // Replaces all events with the pushed ones (latest version wins: events not
  // in the body are removed). Alerts already played are remembered only for
  // identical events (same id: same meeting at the same start time), so a
  // moved meeting beeps again at its new time. Returns the number of events
  // stored, or -1 if the body is malformed (the old events are kept).
  int update(const char *body, time_t receivedAt);

  // Events left out of the last update because there were more than
  // CALENDAR_MAX_EVENTS.
  int dropped() const { return _dropped; }

  // Events not finished yet (the current one first), in start order.
  // Returns how many pointers were written to `out`.
  int upcoming(time_t now, const CalendarEvent **out, int maxCount) const;

  // Returns the next alert due at `now` (at most one per call) and marks it
  // as played. `event` receives the event the alert belongs to.
  CalendarAlert pollAlerts(time_t now, const CalendarEvent **event = nullptr);

  bool hasData() const { return _lastUpdate != 0; }
  time_t lastUpdate() const { return _lastUpdate; }
  int count() const { return _count; }
  const CalendarEvent &at(int i) const { return _events[i]; }

private:
  CalendarEvent _events[CALENDAR_MAX_EVENTS];
  int _count = 0;
  int _dropped = 0;
  time_t _lastUpdate = 0;
};
