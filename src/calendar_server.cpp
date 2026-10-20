#include "calendar_server.h"

#include <ESPmDNS.h>
#include <WebServer.h>

#include "../config.h"

namespace {

// A full day is a few KB; anything bigger is not from the bridge
constexpr int MAX_BODY_BYTES = 8192;

WebServer server(CALENDAR_HTTP_PORT);
Calendar *g_calendar = nullptr;
time_t (*g_now)() = nullptr;
bool g_updated = false;

// Both endpoints need the token: the events include meeting subjects.
bool authorized()
{
  if (strlen(CALENDAR_TOKEN) == 0) return true;
  if (server.header("X-Token") == CALENDAR_TOKEN) return true;
  server.send(401, "text/plain", "bad token\n");
  return false;
}

void handlePost()
{
  if (!authorized()) return;

  // Raw (non-form) bodies are exposed as the "plain" argument
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "empty body\n");
    return;
  }
  const String &body = server.arg("plain");
  if (body.length() > MAX_BODY_BYTES) {
    server.send(413, "text/plain", "body too large\n");
    return;
  }

  int count = g_calendar->update(body.c_str(), g_now());
  if (count < 0) {
    server.send(400, "text/plain", "malformed body\n");
    return;
  }

  g_updated = true;
  Serial.printf("[cal] Received %d events\n", count);
  if (g_calendar->dropped() > 0) {
    Serial.printf("[cal] %d more events left out (CALENDAR_MAX_EVENTS = %d)\n",
                  g_calendar->dropped(), CALENDAR_MAX_EVENTS);
  }
  server.send(200, "text/plain", String("ok ") + count + "\n");
}

void handleGet()
{
  if (!authorized()) return;

  String out = "last_update\t" + String(static_cast<long>(g_calendar->lastUpdate())) + "\n";
  for (int i = 0; i < g_calendar->count(); i++) {
    const CalendarEvent &e = g_calendar->at(i);
    out += String(e.id) + "\t" + String(static_cast<long>(e.start)) + "\t" +
           String(static_cast<long>(e.end)) + "\t" + e.subject + "\t" + e.location + "\t" +
           (e.alertedEarly ? "E" : "-") + (e.alertedStart ? "S" : "-") + "\n";
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

}  // namespace

void calendarServerBegin(Calendar &calendar, time_t (*now)())
{
  g_calendar = &calendar;
  g_now = now;

  const char *headers[] = {"X-Token"};
  server.collectHeaders(headers, 1);
  server.on("/calendar", HTTP_POST, handlePost);
  server.on("/calendar", HTTP_GET, handleGet);
  server.onNotFound([] { server.send(404, "text/plain", "not found\n"); });
  server.begin();

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", CALENDAR_HTTP_PORT);
  } else {
    Serial.println("[cal] mDNS failed to start");
  }
  if (strlen(CALENDAR_TOKEN) == 0) {
    Serial.println("[cal] CALENDAR_TOKEN is empty: anyone on the network can read and push events");
  }
}

bool calendarServerLoop()
{
  server.handleClient();
  bool updated = g_updated;
  g_updated = false;
  return updated;
}
