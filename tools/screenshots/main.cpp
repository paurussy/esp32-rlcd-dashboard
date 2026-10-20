/*
  main.cpp - Renders the dashboard on the host and saves it as PNG files
  ----------------------------------------------------------------------
  Uses the real src/ui.cpp with U8g2 drawing into an in-memory frame buffer,
  so the images match what the device shows pixel for pixel.

  Usage: screenshots <output-dir>
*/
#include <U8g2lib.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "ui.h"

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
  for (int i = 0; i < samples; i++) {
    int minute = endMinuteOfDay - (samples - 1 - i) * HISTORY_INTERVAL_MIN;
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
  s.historyIntervalMin = HISTORY_INTERVAL_MIN;
  return s;
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

  auto save = [&](const char *name, const DashboardState &s) {
    uiDrawDashboard(display.u8g2(), s);
    std::string path = outDir + "/" + name;
    bool ok = writePng(path, display);
    printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    failures += ok ? 0 : 1;
  };

  // Running for a day on battery, connected to WiFi
  History dayHistory;
  fillHistory(dayHistory, HISTORY_SIZE, 14 * 60 + 15);
  DashboardState running = baseState(dayHistory);
  running.wifiConnected = true;
  running.wifiRssi = -58;
  snprintf(running.ipAddress, sizeof(running.ipAddress), "192.168.1.42");
  running.now = {2026, 10, 20, 14, 27, 36, 2};
  running.temperature = 22.8f;
  running.humidity = 46.0f;
  running.batteryPresent = true;
  running.batteryPercent = 76;
  running.uptimeSec = 22 * 3600 + 41 * 60 + 9;
  save("dashboard.png", running);

  // First minutes after boot: USB power, no WiFi, chart still filling up
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

  return failures == 0 ? 0 : 1;
}
