#include "network.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "../../config.h"

namespace {

// Access point of the last connection, kept in RAM (it survives light sleep):
// connecting to a known channel/BSSID skips the full scan.
struct KnownAp {
  bool valid = false;
  uint8_t bssid[6];
  int32_t channel = 0;
} g_knownAp;

bool waitConnected(uint32_t timeoutMs)
{
  uint32_t start = millis();
  while (!wifiConnected() && millis() - start < timeoutMs) {
    delay(100);
  }
  return wifiConnected();
}

}  // namespace

bool wifiConfigured()
{
  return strlen(WIFI_SSID) > 0;
}

bool wifiConnect(uint32_t timeoutMs)
{
  if (!wifiConfigured()) return false;

  WiFi.persistent(false);  // No flash write on every connection
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);

  if (g_knownAp.valid) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, g_knownAp.channel, g_knownAp.bssid);
    if (waitConnected(timeoutMs / 2)) return true;
    WiFi.disconnect();
    g_knownAp.valid = false;  // Moved or gone: scan next
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (!waitConnected(timeoutMs)) return false;

  g_knownAp.channel = WiFi.channel();
  memcpy(g_knownAp.bssid, WiFi.BSSID(), sizeof(g_knownAp.bssid));
  g_knownAp.valid = true;
  return true;
}

bool wifiConnected()
{
  return WiFi.status() == WL_CONNECTED;
}

void wifiOff()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool ntpFetchLocalTime(DateTime &out, uint32_t timeoutMs)
{
  if (!wifiConnected()) return false;

  // getLocalTime() succeeds at once if the system clock is already set, which
  // would "sync" the RTC to its own drift: invalidate it so it waits for SNTP.
  struct timeval invalid = {0, 0};
  settimeofday(&invalid, nullptr);
  configTzTime(TIMEZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);

  struct tm t;
  bool ok = getLocalTime(&t, timeoutMs);
  esp_sntp_stop();  // No background polling: the next sync is on demand
  if (!ok) return false;

  out.year = t.tm_year + 1900;
  out.month = t.tm_mon + 1;
  out.day = t.tm_mday;
  out.hour = t.tm_hour;
  out.minute = t.tm_min;
  out.second = t.tm_sec;
  out.weekday = t.tm_wday;
  return true;
}
