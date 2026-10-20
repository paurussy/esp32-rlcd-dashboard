/*
  calendar_server.h - HTTP endpoint that receives the calendar from the PC bridge
  -------------------------------------------------------------------------------
    POST /calendar   Body in the format described in calendar.h.
                     Header X-Token must match CALENDAR_TOKEN (secrets.h).
    GET  /calendar   Current events, for debugging (same header).
  Also announces the board as <MDNS_HOSTNAME>.local.
*/
#pragma once

#include <time.h>

#include "calendar.h"

// `now` returns the current Unix time (UTC seconds).
void calendarServerBegin(Calendar &calendar, time_t (*now)());

// Handles pending requests. Returns true if the calendar was updated.
bool calendarServerLoop();
