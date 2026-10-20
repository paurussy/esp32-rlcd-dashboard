/*
  push_server.h - HTTP endpoints that receive what the PC programs push
  ---------------------------------------------------------------------
    POST /calendar   Body in the format described in calendar.h.
                     Header X-Token must match CALENDAR_TOKEN (secrets.h).
    GET  /calendar   Current events, for debugging (same header).
    POST /tasks      Body in the format described in tasks.h. The answer lists the
                     tasks finished or deleted on the board. Header X-Token must
                     match TASKS_TOKEN.
    GET  /tasks      Current tasks and pending actions, for debugging (same header).
  Also announces the board as <MDNS_HOSTNAME>.local. Only reachable while the
  WiFi window is open (see WIFI_WINDOW_PERIOD_MIN).
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

// Registers the routes of the features whose object is not null. `now` returns the
// current Unix time (UTC seconds).
void pushServerBegin(Calendar *calendar, TaskList *tasks, time_t (*now)());

// Listen and announce the mDNS name while WiFi is up (a WiFi window); stop
// before switching the radio off.
void pushServerStart();
void pushServerStop();

// Handles pending requests. Returns the PUSH_* flags of what was updated.
uint8_t pushServerLoop();
