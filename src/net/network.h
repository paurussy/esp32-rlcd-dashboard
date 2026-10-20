/*
  network.h - WiFi connection and NTP time retrieval
*/
#pragma once

#include <Arduino.h>

#include "../model/date_time.h"

// Returns true if WiFi credentials are configured in secrets.h.
bool wifiConfigured();

// Starts the connection and waits up to timeoutMs. Returns true once connected.
bool wifiConnect(uint32_t timeoutMs);

bool wifiConnected();

// Disconnects and powers the radio down.
void wifiOff();

// Fetches the current local time (TIMEZONE in config.h) from NTP.
// Requires an active WiFi connection.
bool ntpFetchLocalTime(DateTime &out, uint32_t timeoutMs);
