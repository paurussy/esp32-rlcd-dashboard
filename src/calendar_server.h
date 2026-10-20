/*
  calendar_server.h - HTTP endpoint that receives the calendar from the PC bridge
  -------------------------------------------------------------------------------
    POST /calendar   Body in the format described in calendar.h.
                     Header X-Token must match CALENDAR_TOKEN (secrets.h).
    GET  /calendar   Current events, for debugging (same header).
  Also announces the board as <MDNS_HOSTNAME>.local. Only reachable while the
  WiFi window is open (see WIFI_WINDOW_PERIOD_MIN).
*/
#pragma once

#include <time.h>

#include "calendar.h"

// Registers the routes. `now` returns the current Unix time (UTC seconds).
void calendarServerBegin(Calendar &calendar, time_t (*now)());

// Listen and announce the mDNS name while WiFi is up (a WiFi window); stop
// before switching the radio off.
void calendarServerStart();
void calendarServerStop();

// Handles pending requests. Returns true if the calendar was updated.
bool calendarServerLoop();
