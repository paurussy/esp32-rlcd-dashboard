/*
  push_server.h - HTTP endpoints that receive what the PC programs push
  ---------------------------------------------------------------------
    POST /calendar   Body in the format described in calendar.h.
                     Header X-Token must match CALENDAR_TOKEN (secrets.h).
    GET  /calendar   Current events, for debugging (same header).
    POST /tasks      Body in the format described in tasks.h. Header X-Token must
                     match TASKS_TOKEN.
    GET  /tasks      Current tasks, for debugging (same header).
    GET  /status     What the board holds, one "key<TAB>value" per line (either token):
                       boot_id       Random number picked at boot (changes on restart)
                       uptime        Seconds since boot
                       calendar      Hash of the calendar it holds ("" = none), if enabled
                       tasks         Hash of the tasks it holds ("" = none), if enabled
                       clock_valid   1 when the clock is trustworthy (NTP or a kept RTC)
                       power_saving  POWER_SAVING in config.h (1/0)
                       windows       WIFI_WINDOW_PERIOD_MIN, _OPEN_BEFORE_MIN, _CLOSE_AFTER_MIN,
                                     WIFI_ACTIVE_FROM_HOUR, _UNTIL_HOUR (tab separated)
                       free_heap     Bytes
                     The PC compares the hashes with its own and only pushes what differs.
  Every authorized request also counts as a sign of life from that PC program (see
  pushServerLastContact). Also announces the board as <MDNS_HOSTNAME>.local. Only
  reachable while the WiFi window is open (see WIFI_WINDOW_PERIOD_MIN), or always with
  POWER_SAVING off.
*/
#pragma once

#include <stdint.h>
#include <time.h>

#include "../model/calendar.h"
#include "../model/tasks.h"

enum : uint8_t {
  PUSH_CALENDAR = 1,
  PUSH_TASKS = 2,
};

struct PushHooks {
  time_t (*now)() = nullptr;           // Current Unix time (UTC seconds). Required.
  void (*pcTime)(time_t) = nullptr;    // PC clock from the header of a push, before it is stored
  bool (*clockValid)() = nullptr;      // For GET /status
};

// Registers the routes of the features whose object is not null.
void pushServerBegin(Calendar *calendar, TaskList *tasks, const PushHooks &hooks);

// Listen and announce the mDNS name while WiFi is up (a WiFi window); stop
// before switching the radio off.
void pushServerStart();
void pushServerStop();

// Handles pending requests (nothing when not started). Returns the PUSH_* flags of what
// was updated.
uint8_t pushServerLoop();

// Unix time of the last authorized request with the token of `feature` (PUSH_CALENDAR or
// PUSH_TASKS), 0 if none since boot.
time_t pushServerLastContact(uint8_t feature);
