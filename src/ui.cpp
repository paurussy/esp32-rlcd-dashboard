#include "ui.h"

#include <Arduino.h>

namespace {

constexpr int SCREEN_W = 400;
constexpr int SCREEN_H = 300;
constexpr int HEADER_H = 24;
constexpr int FOOTER_H = 16;

const char *const WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday"};
const char *const MONTHS[] = {"January", "February", "March", "April", "May", "June", "July",
                              "August", "September", "October", "November", "December"};

enum class Metric { Temperature, Humidity };

// ============================================================
//                           ICONS
// ============================================================
// WiFi signal strength: `bars` active bars out of 4.
void drawWifiIcon(U8G2 &g, int x, int y, int bars)
{
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;  // 4, 7, 10, 13
    int bx = x + i * 5;
    if (i < bars) g.drawBox(bx, y + 13 - h, 3, h);
    else          g.drawBox(bx, y + 12, 3, 1);  // Baseline only
  }
}

// Horizontal battery filled proportionally to `pct`, drawn in the current color.
void drawBatteryIcon(U8G2 &g, int x, int y, int pct, bool bolt)
{
  g.drawFrame(x, y, 27, 14);
  g.drawBox(x + 27, y + 4, 3, 6);  // Terminal
  int fill = 23 * constrain(pct, 0, 100) / 100;
  if (fill > 0) g.drawBox(x + 2, y + 2, fill, 10);

  if (bolt) {
    // XOR so the bolt is visible over both the fill and the background
    g.setDrawColor(2);
    g.drawLine(x + 15, y + 2, x + 10, y + 7);
    g.drawLine(x + 10, y + 7, x + 16, y + 7);
    g.drawLine(x + 16, y + 7, x + 11, y + 12);
    g.drawLine(x + 16, y + 2, x + 11, y + 7);
    g.setDrawColor(0);
  }
}

void drawThermometerIcon(U8G2 &g, int x, int y)
{
  g.drawRFrame(x + 3, y, 6, 13, 3);
  g.drawDisc(x + 6, y + 14, 5);
  g.setDrawColor(0);
  g.drawBox(x + 5, y + 5, 2, 9);
  g.setDrawColor(1);
  g.drawBox(x + 5, y + 7, 2, 7);
  g.drawDisc(x + 6, y + 14, 3);
}

void drawDropIcon(U8G2 &g, int x, int y)
{
  g.drawTriangle(x + 6, y, x + 1, y + 9, x + 11, y + 9);
  g.drawDisc(x + 6, y + 11, 5);
}

// ============================================================
//                      HEADER AND FOOTER
// ============================================================
int wifiBars(const DashboardState &s)
{
  if (!s.wifiConnected) return 0;
  if (s.wifiRssi > -55) return 4;
  if (s.wifiRssi > -65) return 3;
  if (s.wifiRssi > -75) return 2;
  return 1;
}

// Layout: [wifi + status]                         [% + battery]
void drawHeader(U8G2 &g, const DashboardState &s)
{
  g.setDrawColor(1);
  g.drawBox(0, 0, SCREEN_W, HEADER_H);
  g.setDrawColor(0);

  drawWifiIcon(g, 8, 5, wifiBars(s));

  g.setFont(u8g2_font_helvB12_tf);
  if (!s.wifiConnected) {
    g.drawUTF8(34, 18, "No WiFi");
  }

  char label[8];
  if (s.batteryPresent) snprintf(label, sizeof(label), "%d%%", s.batteryPercent);
  else                  strcpy(label, "USB");

  int iconX = SCREEN_W - 8 - 30;
  drawBatteryIcon(g, iconX, 5, s.batteryPresent ? s.batteryPercent : 0,
                  s.batteryCharging || !s.batteryPresent);
  g.drawUTF8(iconX - 5 - g.getUTF8Width(label), 18, label);

  g.setDrawColor(1);
}

void drawFooter(U8G2 &g, const DashboardState &s)
{
  g.setDrawColor(1);
  g.drawHLine(0, SCREEN_H - FOOTER_H, SCREEN_W);
  g.setFont(u8g2_font_5x8_tf);

  char left[24];
  if (s.wifiConnected) snprintf(left, sizeof(left), "IP %s", s.ipAddress);
  else                 strcpy(left, "offline");
  g.drawUTF8(6, SCREEN_H - 5, left);

  char uptime[24];
  uint32_t t = s.uptimeSec;
  snprintf(uptime, sizeof(uptime), "uptime %02lu:%02lu:%02lu", static_cast<unsigned long>(t / 3600),
           static_cast<unsigned long>((t / 60) % 60), static_cast<unsigned long>(t % 60));
  g.drawUTF8(SCREEN_W - 6 - g.getUTF8Width(uptime), SCREEN_H - 5, uptime);
}

// ============================================================
//                        CLOCK AND DATE
// ============================================================
// Draws one character centered in a fixed-width slot so the clock doesn't jitter.
void drawDigit(U8G2 &g, int slotX, int slotW, int baseY, char c)
{
  char str[2] = {c, '\0'};
  g.drawStr(slotX + (slotW - g.getStrWidth(str)) / 2, baseY, str);
}

int maxDigitWidth(U8G2 &g)
{
  int widest = 0;
  for (char c = '0'; c <= '9'; c++) {
    char str[2] = {c, '\0'};
    widest = max(widest, static_cast<int>(g.getStrWidth(str)));
  }
  return widest;
}

// Large HH:MM with small seconds on the right; the colon blinks every second.
void drawClock(U8G2 &g, const DashboardState &s, int top)
{
  const uint8_t *bigFont = u8g2_font_logisoso78_tn;
  const uint8_t *smallFont = u8g2_font_logisoso24_tn;

  g.setFont(bigFont);
  int bigDigitW = maxDigitWidth(g);
  int colonW = g.getStrWidth(":");
  int baseY = top + g.getAscent();
  g.setFont(smallFont);
  int smallDigitW = maxDigitWidth(g);

  int bigW = 4 * bigDigitW + colonW + 6;
  int totalW = bigW + 8 + 2 * smallDigitW;
  int x0 = (SCREEN_W - totalW) / 2;

  char hh[4] = "--", mm[4] = "--", ss[4] = "--";
  if (s.rtcPresent) {
    snprintf(hh, sizeof(hh), "%02u", s.now.hour % 100);
    snprintf(mm, sizeof(mm), "%02u", s.now.minute % 100);
    snprintf(ss, sizeof(ss), "%02u", s.now.second % 100);
  }

  g.setFont(bigFont);
  drawDigit(g, x0, bigDigitW, baseY, hh[0]);
  drawDigit(g, x0 + bigDigitW, bigDigitW, baseY, hh[1]);
  if (!s.rtcPresent || s.now.second % 2 == 0) {
    g.drawStr(x0 + 2 * bigDigitW + 3, baseY - 4, ":");
  }
  drawDigit(g, x0 + 2 * bigDigitW + colonW + 6, bigDigitW, baseY, mm[0]);
  drawDigit(g, x0 + 3 * bigDigitW + colonW + 6, bigDigitW, baseY, mm[1]);

  g.setFont(smallFont);
  int secX = x0 + bigW + 8;
  drawDigit(g, secX, smallDigitW, baseY, ss[0]);
  drawDigit(g, secX + smallDigitW, smallDigitW, baseY, ss[1]);
}

void drawDate(U8G2 &g, const DashboardState &s, int baseY)
{
  char text[64];
  if (!s.rtcPresent) {
    strcpy(text, "RTC not detected");
  } else if (s.rtcTimeInvalid && !s.timeSynced) {
    strcpy(text, "Time not synced (check WiFi settings)");
  } else {
    snprintf(text, sizeof(text), "%s, %s %u, %u", WEEKDAYS[s.now.weekday % 7],
             MONTHS[(s.now.month + 11) % 12], s.now.day, s.now.year);
  }

  g.setFont(u8g2_font_helvB12_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(text)) / 2, baseY, text);
}

// ============================================================
//                        SENSOR CARDS
// ============================================================
// Sparkline of the recent history (2 px thick line, newest sample on the right).
// `minRange` keeps small fluctuations from being exaggerated.
void drawSparkline(U8G2 &g, int x, int y, int w, int h, const float *data, int n, float minRange)
{
  g.drawVLine(x, y, h);
  g.drawHLine(x, y + h - 1, w);
  if (n < 1) return;

  float lo = data[0], hi = data[0];
  for (int i = 1; i < n; i++) {
    lo = min(lo, data[i]);
    hi = max(hi, data[i]);
  }
  if (hi - lo < minRange) {
    float mid = (hi + lo) / 2;
    lo = mid - minRange / 2;
    hi = mid + minRange / 2;
  }

  float step = static_cast<float>(w - 4) / (HISTORY_SIZE - 1);
  int prevX = 0, prevY = 0;
  for (int i = 0; i < n; i++) {
    int px = x + w - 2 - static_cast<int>((n - 1 - i) * step);
    int py = y + h - 4 - static_cast<int>((data[i] - lo) / (hi - lo) * (h - 8));
    if (i > 0) {
      g.drawLine(prevX, prevY, px, py);
      g.drawLine(prevX, prevY + 1, px, py + 1);
    } else {
      g.drawBox(px, py, 2, 2);
    }
    prevX = px;
    prevY = py;
  }
}

// Inverted rounded label, right-aligned at `rightX`.
void drawTag(U8G2 &g, int rightX, int y, const char *text)
{
  g.setFont(u8g2_font_helvB10_tf);
  int w = g.getUTF8Width(text) + 12;
  g.drawRBox(rightX - w, y, w, 17, 4);
  g.setDrawColor(0);
  g.drawUTF8(rightX - w + 6, y + 13, text);
  g.setDrawColor(1);
}

const char *comfortLabel(Metric metric, float v)
{
  if (metric == Metric::Temperature) {
    if (v < 17) return "Cold";
    if (v < 20) return "Cool";
    if (v < 25) return "Pleasant";
    if (v < 28) return "Warm";
    return "Hot";
  }
  if (v < 30) return "Dry";
  if (v <= 60) return "Comfort";
  return "Humid";
}

// Card with icon, comfort tag, large value, sparkline and min/max.
void drawCard(U8G2 &g, int x, int y, int w, int h, Metric metric, const DashboardState &s)
{
  const bool isTemp = metric == Metric::Temperature;
  const float value = isTemp ? s.temperature : s.humidity;
  const bool valid = s.sensorPresent && !isnan(value);
  const float *hist = nullptr;
  int histCount = 0;
  if (s.history) {
    hist = isTemp ? s.history->temperature : s.history->humidity;
    histCount = s.history->count;
  }

  g.setDrawColor(1);
  g.drawRFrame(x, y, w, h, 7);

  // Icon + title
  if (isTemp) drawThermometerIcon(g, x + 9, y + 5);
  else        drawDropIcon(g, x + 9, y + 6);
  g.setFont(u8g2_font_helvB10_tf);
  g.drawUTF8(x + 28, y + 19, isTemp ? "TEMP" : "HUMIDITY");

  if (valid) {
    drawTag(g, x + w - 8, y + 6, comfortLabel(metric, value));
  }

  // Large value + unit
  char text[40];
  if (!valid)      strcpy(text, "--");
  else if (isTemp) snprintf(text, sizeof(text), "%.1f", value);
  else             snprintf(text, sizeof(text), "%.0f", value);

  const int valueY = y + 70;
  g.setFont(u8g2_font_logisoso42_tf);
  g.drawUTF8(x + 12, valueY, text);
  int unitX = x + 12 + g.getUTF8Width(text) + 4;
  g.setFont(u8g2_font_helvB18_tf);
  g.drawUTF8(unitX, valueY - 18, isTemp ? "°C" : "%");

  // Chart
  const int chartY = y + 80;
  const int chartH = h - 80 - 22;
  drawSparkline(g, x + 10, chartY, w - 20, chartH, hist, histCount, isTemp ? 2.0f : 8.0f);

  // Min / max and time span
  g.setFont(u8g2_font_6x10_tf);
  const int footY = y + h - 7;
  if (histCount == 0) {
    g.drawUTF8(x + 10, footY, valid ? "measuring..." : "SHTC3 not detected");
    return;
  }

  float lo = hist[0], hi = hist[0];
  for (int i = 1; i < histCount; i++) {
    lo = min(lo, hist[i]);
    hi = max(hi, hist[i]);
  }
  if (valid) {
    lo = min(lo, value);
    hi = max(hi, value);
  }
  if (isTemp) snprintf(text, sizeof(text), "min %.1f°  max %.1f°", lo, hi);
  else        snprintf(text, sizeof(text), "min %.0f%%  max %.0f%%", lo, hi);
  g.drawUTF8(x + 10, footY, text);

  uint32_t spanMin = histCount * s.historyIntervalMin;
  if (spanMin >= 60) snprintf(text, sizeof(text), "%lu h", static_cast<unsigned long>(spanMin / 60));
  else               snprintf(text, sizeof(text), "%lu min", static_cast<unsigned long>(spanMin));
  g.drawUTF8(x + w - 10 - g.getUTF8Width(text), footY, text);
}

}  // namespace

// ============================================================
//                         PUBLIC API
// ============================================================
void uiDrawSplash(U8G2 &g, const char *message)
{
  g.clearBuffer();
  g.setDrawColor(1);
  g.drawFrame(60, 100, 280, 80);

  const char *title = "ESP32-S3-RLCD-4.2";
  g.setFont(u8g2_font_helvB14_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(title)) / 2, 130, title);

  g.setFont(u8g2_font_6x13_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(message)) / 2, 160, message);
  g.sendBuffer();
}

void uiDrawDashboard(U8G2 &g, const DashboardState &s)
{
  g.clearBuffer();
  drawHeader(g, s);
  drawClock(g, s, 31);
  drawDate(g, s, 134);
  drawCard(g, 6, 144, 190, 136, Metric::Temperature, s);
  drawCard(g, 204, 144, 190, 136, Metric::Humidity, s);
  drawFooter(g, s);
  g.sendBuffer();
}
