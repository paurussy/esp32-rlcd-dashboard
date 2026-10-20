#include "network.h"

#include <WiFi.h>
#include <time.h>

#include "../config.h"

bool wifiConfigured()
{
  return strlen(WIFI_SSID) > 0;
}

bool wifiConnect(uint32_t timeoutMs)
{
  if (!wifiConfigured()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (!wifiConnected() && millis() - start < timeoutMs) {
    delay(200);
  }
  return wifiConnected();
}

bool wifiConnected()
{
  return WiFi.status() == WL_CONNECTED;
}

void wifiReconnect()
{
  if (wifiConfigured()) {
    WiFi.reconnect();
  }
}

bool ntpFetchLocalTime(DateTime &out, uint32_t timeoutMs)
{
  if (!wifiConnected()) return false;

  configTzTime(TIMEZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);

  struct tm t;
  if (!getLocalTime(&t, timeoutMs)) return false;

  out.year = t.tm_year + 1900;
  out.month = t.tm_mon + 1;
  out.day = t.tm_mday;
  out.hour = t.tm_hour;
  out.minute = t.tm_min;
  out.second = t.tm_sec;
  out.weekday = t.tm_wday;
  return true;
}
