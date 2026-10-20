/*
  secrets.example.h - Template for secrets.h
  ------------------------------------------
  Copy this file to secrets.h and fill in your values.
  secrets.h is listed in .gitignore and must never be committed.
*/
#pragma once

// 2.4 GHz network used to set the clock via NTP. Leave empty to disable WiFi.
#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"

// Shared secrets sent by the PC programs (X-Token header). Each one turns its
// screen on: leave it out (or empty) and that feature is ignored.
// Use the same values in pc/config.ini.

// Teams/Outlook calendar (pc/calendar_bridge) and meeting beeps
#define CALENDAR_TOKEN "change-me"

// Task list page (pc/tasks_server)
#define TASKS_TOKEN "change-me-too"
