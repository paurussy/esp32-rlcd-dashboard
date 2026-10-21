/*
  main.cpp - Renders the screens on the host and saves them as PNG files
  ----------------------------------------------------------------------
  Uses the real src/ui/ (and src/model/ for the meetings and tasks) with U8g2 drawing into an in-memory frame buffer,
  so the images match what the device shows pixel for pixel.

  Usage: screenshots <output-dir>
*/
#include <U8g2lib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "model/calendar.h"
#include "model/tasks.h"
#include "ui/ui.h"

namespace {

constexpr int WIDTH = 400;
constexpr int HEIGHT = 300;
constexpr int SCALE = 2;  // Upscale for crisp images on high-DPI screens

// Reflective LCD look: light grey-green background, near-black ink
constexpr uint8_t PALETTE[] = {0xD4, 0xD8, 0xCC,   // Pixel off
                               0x1E, 0x22, 0x20};  // Pixel on

// ============================================================
//                    U8G2 OFF-SCREEN TARGET
// ============================================================
constexpr uint8_t TILE_WIDTH = WIDTH / 8;
constexpr uint8_t TILE_HEIGHT = (HEIGHT + 7) / 8;

u8x8_display_info_t g_displayInfo = {
  /* chip_enable_level = */ 0,
  /* chip_disable_level = */ 1,
  /* post_chip_enable_wait_ns = */ 0,
  /* pre_chip_disable_wait_ns = */ 0,
  /* reset_pulse_width_ms = */ 0,
  /* post_reset_wait_ms = */ 0,
  /* sda_setup_time_ns = */ 0,
  /* sck_pulse_width_ns = */ 0,
  /* sck_clock_hz = */ 0,
  /* spi_mode = */ 0,
  /* i2c_bus_clock_100kHz = */ 4,
  /* data_setup_time_ns = */ 0,
  /* write_pulse_width_ns = */ 0,
  /* tile_width = */ TILE_WIDTH,
  /* tile_height = */ TILE_HEIGHT,
  /* default_x_offset = */ 0,
  /* flip_mode_x_offset = */ 0,
  /* pixel_width = */ WIDTH,
  /* pixel_height = */ HEIGHT
};

uint8_t displayCallback(u8x8_t *u8x8, uint8_t msg, uint8_t, void *)
{
  if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
    u8x8_d_helper_display_setup_memory(u8x8, &g_displayInfo);
  }
  return 1;  // Nothing to send: the frame buffer is read directly
}

uint8_t byteCallback(u8x8_t *, uint8_t, uint8_t, void *) { return 1; }

class OffscreenDisplay {
public:
  OffscreenDisplay() : _buffer(TILE_WIDTH * 8 * TILE_HEIGHT, 0)
  {
    u8g2_t *u = _u8g2.getU8g2();
    u8x8_Setup(u8g2_GetU8x8(u), displayCallback, u8x8_dummy_cb, byteCallback, u8x8_dummy_cb);
    u8g2_SetupBuffer(u, _buffer.data(), TILE_HEIGHT, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(u);
    _u8g2.enableUTF8Print();
  }

  U8G2 &u8g2() { return _u8g2; }

  // Same layout as the device: vertical bytes, LSB on top
  bool pixel(int x, int y) const
  {
    return (_buffer[(y / 8) * TILE_WIDTH * 8 + x] >> (y % 8)) & 1;
  }

private:
  U8G2 _u8g2;
  std::vector<uint8_t> _buffer;
};

// ============================================================
//                         PNG WRITER
// ============================================================
// 1-bit palette PNG with uncompressed (stored) deflate blocks: no zlib needed.
uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0)
{
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

void putU32(std::vector<uint8_t> &out, uint32_t v)
{
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void putChunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
  putU32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  out.insert(out.end(), body.begin(), body.end());
  putU32(out, crc32(body.data(), body.size()));
}

std::vector<uint8_t> zlibStored(const std::vector<uint8_t> &raw)
{
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    size_t len = std::min<size_t>(raw.size() - pos, 65535);
    bool last = pos + len == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(len & 0xFF);
    z.push_back(len >> 8);
    z.push_back(~len & 0xFF);
    z.push_back((~len >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
    pos += len;
  } while (pos < raw.size());

  uint32_t a = 1, b = 0;  // Adler-32
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  putU32(z, (b << 16) | a);
  return z;
}

bool writePng(const std::string &path, const OffscreenDisplay &display)
{
  const int w = WIDTH * SCALE;
  const int h = HEIGHT * SCALE;
  const int rowBytes = (w + 7) / 8;

  std::vector<uint8_t> raw;
  raw.reserve((rowBytes + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push_back(0);  // Filter: none
    for (int byteX = 0; byteX < rowBytes; byteX++) {
      uint8_t packed = 0;
      for (int bit = 0; bit < 8; bit++) {
        int x = byteX * 8 + bit;
        if (x < w && display.pixel(x / SCALE, y / SCALE)) packed |= 0x80 >> bit;
      }
      raw.push_back(packed);
    }
  }

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  putU32(ihdr, w);
  putU32(ihdr, h);
  ihdr.insert(ihdr.end(), {1, 3, 0, 0, 0});  // 1 bit, palette, deflate, no filter, no interlace
  putChunk(png, "IHDR", ihdr);
  putChunk(png, "PLTE", std::vector<uint8_t>(PALETTE, PALETTE + sizeof(PALETTE)));
  putChunk(png, "IDAT", zlibStored(raw));
  putChunk(png, "IEND", {});

  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
  return fclose(f) == 0 && ok;
}

// ============================================================
//                           SCENES
// ============================================================
// Plausible indoor day cycle: warmest in the late afternoon, humidity inverse.
void fillHistory(History &history, int samples, int endMinuteOfDay)
{
  constexpr double PI = 3.14159265358979323846;
  history.intervalMin = HISTORY_LEVELS_MIN[HISTORY_LEVELS - 1];
  for (int i = 0; i < samples; i++) {
    int minute = endMinuteOfDay - (samples - 1 - i) * static_cast<int>(history.intervalMin);
    double phase = 2 * PI * (minute - 17 * 60) / (24 * 60.0);
    double ripple = std::sin(i * 1.7) * 0.15;
    history.push(static_cast<float>(21.6 + 1.5 * std::cos(phase) + ripple),
                 static_cast<float>(49.0 - 6.0 * std::cos(phase) + ripple * 4));
  }
}

DashboardState baseState(const History &history)
{
  DashboardState s;
  s.rtcPresent = true;
  s.timeSynced = true;
  s.sensorPresent = true;
  s.history = &history;
  return s;
}

// A Tuesday of meetings, pushed to `calendar` the way the PC bridge does it.
void fillCalendar(Calendar &calendar, time_t receivedAt)
{
  struct Meeting {
    uint8_t startH, startM, endH, endM;
    const char *subject, *location;
  };
  const Meeting meetings[] = {
    {9, 0, 9, 15, "Daily stand-up", "Microsoft Teams Meeting"},
    {10, 0, 10, 45, "Sprint planning · Dashboard v2", "Microsoft Teams Meeting"},
    {11, 0, 11, 30, "Design review", "Room Agora"},
    {12, 30, 13, 30, "Lunch with the team", ""},
    {15, 0, 16, 0, "Client demo: calendar integration with the ESP32 board", "Microsoft Teams Meeting"},
    {17, 30, 18, 0, "Weekly retro", "Microsoft Teams Meeting"},
  };

  std::string body = "v1\t" + std::to_string(static_cast<long>(receivedAt)) + "\n";
  uint32_t id = 1000;
  for (const Meeting &m : meetings) {
    time_t start = dateTimeToEpoch({2026, 10, 20, m.startH, m.startM, 0, 2});
    time_t end = dateTimeToEpoch({2026, 10, 20, m.endH, m.endM, 0, 2});
    body += std::to_string(id++) + "\t" + std::to_string(static_cast<long>(start)) + "\t" +
            std::to_string(static_cast<long>(end)) + "\t" + m.subject + "\t" + m.location + "\n";
  }
  if (calendar.update(body.c_str(), receivedAt) != 6) fprintf(stderr, "calendar parse failed\n");
}

// Pending tasks as the PC sends them, in the order set on its page.
void fillTasks(TaskList &tasks, time_t receivedAt)
{
  struct Item {
    bool high;
    const char *text;
  };
  const Item items[] = {
    {true, "Send the invoice to the accountant"},
    {true, "Call Ana about the contract"},
    {false, "Buy milk and bread"},
    {false, "Book the dentist"},
    {false, "Renew the car insurance"},
    {false, "Water the plants"},
    {false, "Reply to the landlord about the flat"},
  };

  std::string body = "v1\t" + std::to_string(static_cast<long>(receivedAt)) + "\n";
  uint32_t id = 1;
  for (const Item &item : items) {
    body += std::to_string(id++) + "\t" + (item.high ? "1" : "0") + "\t" + item.text + "\n";
  }
  if (tasks.update(body.c_str(), receivedAt) != 7) fprintf(stderr, "tasks parse failed\n");
}

TasksView tasksView(const TaskList &tasks, const DateTime &now, int selected)
{
  TasksView t;
  t.now = dateTimeToEpoch(now);
  t.list = &tasks;
  t.selected = selected;
  t.hasData = tasks.hasData();
  t.lastUpdate = tasks.lastUpdate();
  t.stale = t.hasData && t.now - t.lastUpdate > static_cast<time_t>(TASKS_STALE_MIN * 60);
  return t;
}

CalendarView calendarView(const Calendar &calendar, const DateTime &now)
{
  CalendarView c;
  c.now = dateTimeToEpoch(now);
  c.count = calendar.upcoming(c.now, c.events, CALENDAR_VISIBLE_EVENTS);
  c.hasData = calendar.hasData();
  c.lastUpdate = calendar.lastUpdate();
  c.stale = c.hasData && c.now - c.lastUpdate > static_cast<time_t>(CALENDAR_STALE_MIN * 60);
  return c;
}

}  // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
    return 1;
  }
  const std::string outDir = argv[1];
  OffscreenDisplay display;
  int failures = 0;

  // Same time zone as the device, for the calendar times
  setenv("TZ", TIMEZONE, 1);
  tzset();

  auto write = [&](const char *name) {
    std::string path = outDir + "/" + name;
    bool ok = writePng(path, display);
    printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    failures += ok ? 0 : 1;
  };
  // Three pages, as on the device with the calendar and tasks enabled
  auto save = [&](const char *name, DashboardState s) {
    s.page = 0;
    s.pageCount = 3;
    uiDrawDashboard(display.u8g2(), s);
    write(name);
  };
  auto saveCalendar = [&](const char *name, DashboardState s, const CalendarView &c) {
    s.page = 1;
    s.pageCount = 3;
    uiDrawCalendar(display.u8g2(), s, c);
    write(name);
  };
  auto saveTasks = [&](const char *name, DashboardState s, const TasksView &t) {
    s.page = 2;
    s.pageCount = 3;
    uiDrawTasks(display.u8g2(), s, t);
    write(name);
  };

  // Running for a day on battery. WiFi is off between windows: no WiFi icon
  History dayHistory;
  fillHistory(dayHistory, HISTORY_SIZE, 14 * 60 + 15);
  DashboardState running = baseState(dayHistory);
  running.now = {2026, 10, 20, 14, 27, 36, 2};
  running.lastNtpSync = dateTimeToEpoch({2026, 10, 20, 13, 59, 8, 2});
  running.temperature = 22.8f;
  running.humidity = 46.0f;
  running.batteryPresent = true;
  running.batteryPercent = 76;
  running.uptimeSec = 22 * 3600 + 41 * 60 + 9;
  save("dashboard.png", running);

  // BOOT on the dashboard: one chart gets the full width, the other reading moves to its title
  running.chartView = ChartView::Temperature;
  save("dashboard-temperature.png", running);
  running.chartView = ChartView::Both;

  // First minutes after boot: USB power, clock never synced, chart still filling up
  History bootHistory;
  bootHistory.push(18.9f, 60.0f);
  bootHistory.push(19.1f, 59.0f);
  bootHistory.push(19.3f, 58.0f);
  DashboardState booting = baseState(bootHistory);
  booting.now = {2026, 10, 21, 9, 5, 12, 3};
  booting.temperature = 19.4f;
  booting.humidity = 58.0f;
  booting.uptimeSec = 31 * 60 + 12;
  save("dashboard-offline.png", booting);

  // Calendar: in a meeting, with the rest of the day below
  Calendar calendar;
  fillCalendar(calendar, dateTimeToEpoch({2026, 10, 20, 10, 26, 0, 2}));
  running.now = {2026, 10, 20, 10, 27, 0, 2};
  saveCalendar("calendar.png", running, calendarView(calendar, running.now));

  // Between meetings, and the PC has gone quiet: the next one is framed instead of inverted
  running.now = {2026, 10, 20, 10, 52, 0, 2};
  saveCalendar("calendar-next.png", running, calendarView(calendar, running.now));

  // BOOT on the calendar: beeps muted (icon in the header; WiFi is on, as in a window)
  running.now = {2026, 10, 20, 10, 27, 0, 2};
  running.alertsMuted = true;
  running.wifiConnected = true;
  running.wifiRssi = -58;
  saveCalendar("calendar-muted.png", running, calendarView(calendar, running.now));
  running.alertsMuted = false;
  running.wifiConnected = false;
  running.now = {2026, 10, 20, 10, 52, 0, 2};

  // Before the PC bridge has pushed anything
  Calendar empty;
  saveCalendar("calendar-empty.png", running, calendarView(empty, running.now));

  // Tasks: two high-priority ones on top, the second one selected
  TaskList tasks;
  fillTasks(tasks, dateTimeToEpoch({2026, 10, 20, 14, 15, 0, 2}));
  running.now = {2026, 10, 20, 14, 27, 0, 2};
  saveTasks("tasks.png", running, tasksView(tasks, running.now, 1));

  // Before the PC task page has pushed anything
  TaskList noTasks;
  saveTasks("tasks-empty.png", running, tasksView(noTasks, running.now, 0));

  return failures == 0 ? 0 : 1;
}
