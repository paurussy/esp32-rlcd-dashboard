#include "push_server.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <esp_random.h>

#include "../../config.h"
#include "../model/push_body.h"

namespace {

// A full day is a few KB; anything bigger is not from the PC (the PC programs cut what
// they send to what the board keeps, well below this)
constexpr int MAX_BODY_BYTES = 8192;

WebServer server(PUSH_HTTP_PORT);
Calendar *g_calendar = nullptr;
TaskList *g_tasks = nullptr;
PushHooks g_hooks;
uint8_t g_updated = 0;
bool g_running = false;
uint32_t g_bootId = 0;
time_t g_lastContact[2] = {0, 0};  // Calendar, tasks

// Which features the X-Token header belongs to (PUSH_* flags; both if the tokens are equal).
uint8_t tokenOwner()
{
  const String sent = server.header("X-Token");
  if (sent.length() == 0) return 0;
  uint8_t owner = 0;
  if (g_calendar && sent == CALENDAR_TOKEN) owner |= PUSH_CALENDAR;
  if (g_tasks && sent == TASKS_TOKEN) owner |= PUSH_TASKS;
  return owner;
}

void noteContact(uint8_t owner)
{
  const time_t now = g_hooks.now();
  if (owner & PUSH_CALENDAR) g_lastContact[0] = now;
  if (owner & PUSH_TASKS) g_lastContact[1] = now;
}

// Both features need a token: the events and tasks are private.
bool authorized(uint8_t feature)
{
  const uint8_t owner = tokenOwner();
  if (owner & feature) {
    noteContact(owner);
    return true;
  }
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

// Lets the sketch take the PC clock (when its own is not trustworthy) before the body is
// stored, so the update is stamped with the corrected time.
void offerPcTime(const String &body)
{
  if (!g_hooks.pcTime) return;
  const time_t pcNow = peekPcNow(body.c_str());
  if (pcNow > 0) g_hooks.pcTime(pcNow);
}

void handleCalendarPost()
{
  if (!authorized(PUSH_CALENDAR)) return;
  String body;
  if (!readBody(body)) return;
  offerPcTime(body);

  int count = g_calendar->update(body.c_str(), g_hooks.now());
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
  if (!authorized(PUSH_CALENDAR)) return;

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
  if (!authorized(PUSH_TASKS)) return;
  String body;
  if (!readBody(body)) return;
  offerPcTime(body);

  int count = g_tasks->update(body.c_str(), g_hooks.now());
  if (count < 0) {
    server.send(400, "text/plain", "malformed body\n");
    return;
  }

  g_updated |= PUSH_TASKS;
  Serial.printf("[tasks] Received %d tasks (%d pending on the PC)\n", count, g_tasks->total());
  if (g_tasks->dropped() > 0) {
    Serial.printf("[tasks] %d more tasks left out (TASKS_MAX_ITEMS = %d)\n",
                  g_tasks->dropped(), TASKS_MAX_ITEMS);
  }
  server.send(200, "text/plain", String("ok ") + count + "\n");
}

void handleTasksGet()
{
  if (!authorized(PUSH_TASKS)) return;

  String out = "last_update\t" + String(static_cast<long>(g_tasks->lastUpdate())) + "\n";
  for (int i = 0; i < g_tasks->count(); i++) {
    const Task &t = g_tasks->at(i);
    out += String(t.id) + "\t" + (t.high ? "1" : "0") + "\t" + t.text + "\n";
  }
  server.send(200, "text/plain; charset=utf-8", out);
}

void handleStatus()
{
  const uint8_t owner = tokenOwner();
  if (owner == 0) {
    server.send(401, "text/plain", "bad token\n");
    return;
  }
  noteContact(owner);

  char line[96];
  String out;
  snprintf(line, sizeof(line), "boot_id\t%08lx\n", static_cast<unsigned long>(g_bootId));
  out += line;
  snprintf(line, sizeof(line), "uptime\t%lu\n", static_cast<unsigned long>(millis() / 1000));
  out += line;
  if (g_calendar) out += String("calendar\t") + g_calendar->hash() + "\n";
  if (g_tasks) out += String("tasks\t") + g_tasks->hash() + "\n";
  out += String("clock_valid\t") + (g_hooks.clockValid && g_hooks.clockValid() ? "1" : "0") + "\n";
  out += String("power_saving\t") + (POWER_SAVING ? "1" : "0") + "\n";
  snprintf(line, sizeof(line), "windows\t%lu\t%lu\t%lu\t%lu\t%lu\n",
           static_cast<unsigned long>(WIFI_WINDOW_PERIOD_MIN), static_cast<unsigned long>(WIFI_WINDOW_OPEN_BEFORE_MIN),
           static_cast<unsigned long>(WIFI_WINDOW_CLOSE_AFTER_MIN), static_cast<unsigned long>(WIFI_ACTIVE_FROM_HOUR),
           static_cast<unsigned long>(WIFI_ACTIVE_UNTIL_HOUR));
  out += line;
  snprintf(line, sizeof(line), "free_heap\t%lu\n", static_cast<unsigned long>(ESP.getFreeHeap()));
  out += line;
  server.send(200, "text/plain; charset=utf-8", out);
}

}  // namespace

void pushServerBegin(Calendar *calendar, TaskList *tasks, const PushHooks &hooks)
{
  g_calendar = calendar;
  g_tasks = tasks;
  g_hooks = hooks;
  g_bootId = esp_random();

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
  server.on("/status", HTTP_GET, handleStatus);
  server.onNotFound([] { server.send(404, "text/plain", "not found\n"); });
}

void pushServerStart()
{
  if (!g_hooks.now) return;  // pushServerBegin() not called (no WiFi credentials)
  server.begin();
  g_running = true;
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", PUSH_HTTP_PORT);
  } else {
    Serial.println("[push] mDNS failed to start");
  }
}

void pushServerStop()
{
  if (!g_running) return;
  MDNS.end();
  server.stop();
  g_running = false;
}

uint8_t pushServerLoop()
{
  if (!g_running) return 0;
  server.handleClient();
  uint8_t updated = g_updated;
  g_updated = 0;
  return updated;
}

time_t pushServerLastContact(uint8_t feature)
{
  return feature == PUSH_CALENDAR ? g_lastContact[0] : feature == PUSH_TASKS ? g_lastContact[1] : 0;
}
