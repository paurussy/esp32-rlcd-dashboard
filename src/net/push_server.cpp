#include "push_server.h"

#include <ESPmDNS.h>
#include <WebServer.h>

#include "../../config.h"

namespace {

// A full day is a few KB; anything bigger is not from the PC
constexpr int MAX_BODY_BYTES = 8192;

WebServer server(PUSH_HTTP_PORT);
Calendar *g_calendar = nullptr;
TaskList *g_tasks = nullptr;
time_t (*g_now)() = nullptr;
uint8_t g_updated = 0;

// Both features need a token: the events and tasks are private.
bool authorized(const char *token)
{
  if (server.header("X-Token") == token) return true;
  server.send(401, "text/plain", "bad token\n");
  return false;
}

// Copies the raw body of a POST into `body`. False after answering with the error.
// (arg() returns a String by value: keep the copy, not a reference into it.)
bool readBody(String &body)
{
  // Raw (non-form) bodies are exposed as the "plain" argument
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "empty body\n");
    return false;
  }
  body = server.arg("plain");
  if (body.length() > MAX_BODY_BYTES) {
    server.send(413, "text/plain", "body too large\n");
    return false;
  }
  return true;
}

void handleCalendarPost()
{
  if (!authorized(CALENDAR_TOKEN)) return;
  String body;
  if (!readBody(body)) return;

  int count = g_calendar->update(body.c_str(), g_now());
  if (count < 0) {
    server.send(400, "text/plain", "malformed body\n");
    return;
  }

  g_updated |= PUSH_CALENDAR;
  Serial.printf("[cal] Received %d events\n", count);
  if (g_calendar->dropped() > 0) {
    Serial.printf("[cal] %d more events left out (CALENDAR_MAX_EVENTS = %d)\n",
                  g_calendar->dropped(), CALENDAR_MAX_EVENTS);
  }
  server.send(200, "text/plain", String("ok ") + count + "\n");
}

void handleCalendarGet()
{
  if (!authorized(CALENDAR_TOKEN)) return;

  String out = "last_update\t" + String(static_cast<long>(g_calendar->lastUpdate())) + "\n";
  for (int i = 0; i < g_calendar->count(); i++) {
    const CalendarEvent &e = g_calendar->at(i);
    out += String(e.id) + "\t" + String(static_cast<long>(e.start)) + "\t" +
           String(static_cast<long>(e.end)) + "\t" + e.subject + "\t" + e.location + "\t" +
           (e.alertedEarly ? "E" : "-") + (e.alertedStart ? "S" : "-") + "\n";
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

void handleTasksPost()
{
  if (!authorized(TASKS_TOKEN)) return;
  String body;
  if (!readBody(body)) return;

  int count = g_tasks->update(body.c_str(), g_now());
  if (count < 0) {
    server.send(400, "text/plain", "malformed body\n");
    return;
  }

  g_updated |= PUSH_TASKS;
  Serial.printf("[tasks] Received %d tasks\n", count);
  if (g_tasks->dropped() > 0) {
    Serial.printf("[tasks] %d more tasks left out (TASKS_MAX_ITEMS = %d)\n",
                  g_tasks->dropped(), TASKS_MAX_ITEMS);
  }
  server.send(200, "text/plain", String("ok ") + count + "\n");
}

void handleTasksGet()
{
  if (!authorized(TASKS_TOKEN)) return;

  String out = "last_update\t" + String(static_cast<long>(g_tasks->lastUpdate())) + "\n";
  for (int i = 0; i < g_tasks->count(); i++) {
    const Task &t = g_tasks->at(i);
    out += String(t.id) + "\t" + (t.high ? "1" : "0") + "\t" + t.text + "\n";
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

}  // namespace

void pushServerBegin(Calendar *calendar, TaskList *tasks, time_t (*now)())
{
  g_calendar = calendar;
  g_tasks = tasks;
  g_now = now;

  const char *headers[] = {"X-Token"};
  server.collectHeaders(headers, 1);
  if (g_calendar) {
    server.on("/calendar", HTTP_POST, handleCalendarPost);
    server.on("/calendar", HTTP_GET, handleCalendarGet);
  }
  if (g_tasks) {
    server.on("/tasks", HTTP_POST, handleTasksPost);
    server.on("/tasks", HTTP_GET, handleTasksGet);
  }
  server.onNotFound([] { server.send(404, "text/plain", "not found\n"); });
}

void pushServerStart()
{
  server.begin();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", PUSH_HTTP_PORT);
  } else {
    Serial.println("[push] mDNS failed to start");
  }
}

void pushServerStop()
{
  MDNS.end();
  server.stop();
}

uint8_t pushServerLoop()
{
  server.handleClient();
  uint8_t updated = g_updated;
  g_updated = 0;
  return updated;
}
