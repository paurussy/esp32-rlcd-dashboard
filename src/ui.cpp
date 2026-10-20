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

// Speaker crossed out: meeting beeps are muted. Drawn in the current color.
void drawMutedIcon(U8G2 &g, int x, int y)
{
  g.drawBox(x, y + 4, 4, 6);
  g.drawTriangle(x + 4, y + 4, x + 9, y, x + 9, y + 13);
  g.drawTriangle(x + 4, y + 9, x + 9, y, x + 9, y + 13);
  g.drawLine(x + 12, y + 4, x + 18, y + 10);
  g.drawLine(x + 12, y + 10, x + 18, y + 4);
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

// One dot per page, centered; the current one filled. Drawn in the current color.
void drawPageDots(U8G2 &g, int centerX, int centerY, int page, int count)
{
  if (count <= 1) return;
  constexpr int SPACING = 14;
  constexpr int RADIUS = 4;
  int x = centerX - (count - 1) * SPACING / 2;
  for (int i = 0; i < count; i++, x += SPACING) {
    if (i == page) g.drawDisc(x, centerY, RADIUS);
    else           g.drawCircle(x, centerY, RADIUS);
  }
}

// Layout: [wifi, only when connected] [muted, only when muted]   [page dots]   [% + battery]
void drawHeader(U8G2 &g, const DashboardState &s)
{
  g.setDrawColor(1);
  g.drawBox(0, 0, SCREEN_W, HEADER_H);
  g.setDrawColor(0);

  if (s.wifiConnected) drawWifiIcon(g, 8, 5, wifiBars(s));
  if (s.alertsMuted) drawMutedIcon(g, s.wifiConnected ? 34 : 8, 5);

  g.setFont(u8g2_font_helvB12_tf);
  char label[8];
  if (s.batteryPresent) snprintf(label, sizeof(label), "%d%%", s.batteryPercent);
  else                  strcpy(label, "USB");

  int iconX = SCREEN_W - 8 - 30;
  drawBatteryIcon(g, iconX, 5, s.batteryPresent ? s.batteryPercent : 0,
                  s.batteryCharging || !s.batteryPresent);
  g.drawUTF8(iconX - 5 - g.getUTF8Width(label), 18, label);

  drawPageDots(g, SCREEN_W / 2, HEADER_H / 2, s.page, s.pageCount);

  g.setDrawColor(1);
}

void drawFooterText(U8G2 &g, const char *left, const char *right)
{
  g.setDrawColor(1);
  g.drawHLine(0, SCREEN_H - FOOTER_H, SCREEN_W);
  g.setFont(u8g2_font_5x8_tf);
  g.drawUTF8(6, SCREEN_H - 5, left);
  g.drawUTF8(SCREEN_W - 6 - g.getUTF8Width(right), SCREEN_H - 5, right);
}

void formatHM(time_t t, char *out, size_t size);  // Defined with the calendar helpers

void drawFooter(U8G2 &g, const DashboardState &s)
{
  char left[32];
  if (s.wifiConnected) {
    snprintf(left, sizeof(left), "IP %s", s.ipAddress);
  } else if (s.lastNtpSync > 0) {
    char hm[8];
    formatHM(s.lastNtpSync, hm, sizeof(hm));
    snprintf(left, sizeof(left), "clock synced %s", hm);
  } else {
    strcpy(left, "clock not synced");
  }

  char uptime[24];
  unsigned long hours = s.uptimeSec / 3600;
  unsigned long minutes = (s.uptimeSec / 60) % 60;
  snprintf(uptime, sizeof(uptime), "uptime %luh %02lum", hours, minutes);
  drawFooterText(g, left, uptime);
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

// Large HH:MM (the screen is only refreshed once per minute, so no seconds).
void drawClock(U8G2 &g, const DashboardState &s, int top)
{
  g.setFont(u8g2_font_logisoso78_tn);
  int bigDigitW = maxDigitWidth(g);
  int colonW = g.getStrWidth(":");
  int baseY = top + g.getAscent();

  int totalW = 4 * bigDigitW + colonW + 6;
  int x0 = (SCREEN_W - totalW) / 2;

  char hh[4] = "--", mm[4] = "--";
  if (s.rtcPresent) {
    snprintf(hh, sizeof(hh), "%02u", s.now.hour % 100);
    snprintf(mm, sizeof(mm), "%02u", s.now.minute % 100);
  }

  drawDigit(g, x0, bigDigitW, baseY, hh[0]);
  drawDigit(g, x0 + bigDigitW, bigDigitW, baseY, hh[1]);
  g.drawStr(x0 + 2 * bigDigitW + 3, baseY - 4, ":");
  drawDigit(g, x0 + 2 * bigDigitW + colonW + 6, bigDigitW, baseY, mm[0]);
  drawDigit(g, x0 + 3 * bigDigitW + colonW + 6, bigDigitW, baseY, mm[1]);
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

  // The points always span the full width
  float step = n > 1 ? static_cast<float>(w - 4) / (n - 1) : 0;
  int prevX = 0, prevY = 0;
  for (int i = 0; i < n; i++) {
    int px = x + w - 2 - static_cast<int>((n - 1 - i) * step + 0.5f);
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
// `other` (optional) is the reading of the other metric, shown after the title
// when this card is the only one on screen.
void drawCard(U8G2 &g, int x, int y, int w, int h, Metric metric, const DashboardState &s,
              const char *other = nullptr)
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
  const char *title = isTemp ? "TEMP" : "HUMIDITY";
  g.drawUTF8(x + 28, y + 19, title);
  if (other) g.drawUTF8(x + 28 + g.getUTF8Width(title) + 14, y + 19, other);

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

  uint32_t spanMin = histCount * s.history->intervalMin;
  if (spanMin >= 60) snprintf(text, sizeof(text), "%lu h", static_cast<unsigned long>(spanMin / 60));
  else               snprintf(text, sizeof(text), "%lu min", static_cast<unsigned long>(spanMin));
  g.drawUTF8(x + w - 10 - g.getUTF8Width(text), footY, text);
}


// ============================================================
//                          CALENDAR
// ============================================================
void formatHM(time_t t, char *out, size_t size)
{
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, size, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// "12 min", "1 h 05 min", "2 h". Rounds up so "0 min" never shows.
void formatDuration(time_t seconds, char *out, size_t size)
{
  long minutes = (static_cast<long>(seconds) + 59) / 60;
  if (minutes < 60)           snprintf(out, size, "%ld min", minutes);
  else if (minutes % 60 == 0) snprintf(out, size, "%ld h", minutes / 60);
  else                        snprintf(out, size, "%ld h %02ld min", minutes / 60, minutes % 60);
}

// Draws `text`, cut at a UTF-8 character boundary and ended with "..." if wider than maxW.
void drawUTF8Fit(U8G2 &g, int x, int y, int maxW, const char *text)
{
  if (static_cast<int>(g.getUTF8Width(text)) <= maxW) {
    g.drawUTF8(x, y, text);
    return;
  }
  char buf[96];
  snprintf(buf, sizeof(buf), "%s", text);
  size_t len = strlen(buf);
  while (len > 0) {
    do len--; while (len > 0 && (static_cast<uint8_t>(buf[len]) & 0xC0) == 0x80);
    strcpy(buf + len, "...");
    if (static_cast<int>(g.getUTF8Width(buf)) <= maxW) break;
  }
  g.drawUTF8(x, y, buf);
}

void drawDottedHLine(U8G2 &g, int x, int y, int w)
{
  for (int i = 0; i < w; i += 3) g.drawPixel(x + i, y);
}

bool isCurrent(const CalendarEvent &e, time_t now)
{
  return e.start <= now && now < e.end;
}

// Date on the left, HH:MM on the right.
void drawCalendarTitle(U8G2 &g, const CalendarView &c)
{
  struct tm lt;
  localtime_r(&c.now, &lt);

  char date[40];
  snprintf(date, sizeof(date), "%s, %s %d", WEEKDAYS[lt.tm_wday % 7], MONTHS[lt.tm_mon % 12], lt.tm_mday);
  g.setFont(u8g2_font_helvB12_tf);
  g.drawUTF8(10, 47, date);

  char clock[8];
  formatHM(c.now, clock, sizeof(clock));
  g.setFont(u8g2_font_helvB14_tf);
  g.drawUTF8(SCREEN_W - 10 - g.getUTF8Width(clock), 48, clock);
}

// The event happening now (inverted card with progress) or the next one (framed card).
void drawMainEvent(U8G2 &g, const CalendarEvent &e, time_t now, int x, int y, int w, int h)
{
  const bool current = isCurrent(e, now);
  const int pad = 12;

  g.setDrawColor(1);
  if (current) {
    g.drawRBox(x, y, w, h, 7);
    g.setDrawColor(0);
  } else {
    g.drawRFrame(x, y, w, h, 7);
    g.drawRFrame(x + 1, y + 1, w - 2, h - 2, 6);
  }

  // Label + time left / time to start
  char rel[32], tag[40];
  formatDuration(current ? e.end - now : e.start - now, rel, sizeof(rel));
  if (current) snprintf(tag, sizeof(tag), "%s left", rel);
  else         snprintf(tag, sizeof(tag), "in %s", rel);
  g.setFont(u8g2_font_helvB10_tf);
  g.drawUTF8(x + pad, y + 19, current ? "NOW" : "NEXT");
  g.setFont(u8g2_font_helvB12_tf);
  g.drawUTF8(x + w - pad - g.getUTF8Width(tag), y + 20, tag);

  // Subject
  g.setFont(u8g2_font_helvB18_tf);
  drawUTF8Fit(g, x + pad, y + 48, w - 2 * pad, e.subject[0] ? e.subject : "(no subject)");

  // Time range and location
  char start[8], end[8], line[96];
  formatHM(e.start, start, sizeof(start));
  formatHM(e.end, end, sizeof(end));
  if (e.location[0]) snprintf(line, sizeof(line), "%s - %s  \xC2\xB7  %s", start, end, e.location);
  else               snprintf(line, sizeof(line), "%s - %s", start, end);
  g.setFont(u8g2_font_helvB10_tf);
  drawUTF8Fit(g, x + pad, y + 69, w - 2 * pad, line);

  // Progress through the meeting
  if (current && e.end > e.start) {
    const int barW = w - 2 * pad;
    const int barY = y + h - 18;
    g.drawFrame(x + pad, barY, barW, 8);
    int fill = static_cast<int>((barW - 4) * (now - e.start) / (e.end - e.start));
    if (fill > 0) g.drawBox(x + pad + 2, barY + 2, fill, 4);
  }

  g.setDrawColor(1);
}

// One-line row: "10:30 - 11:00   Subject". A bar on the left marks overlapping current events.
void drawEventRow(U8G2 &g, const CalendarEvent &e, time_t now, int y, bool separator)
{
  char start[8], end[8], range[20];
  formatHM(e.start, start, sizeof(start));
  formatHM(e.end, end, sizeof(end));
  snprintf(range, sizeof(range), "%s - %s", start, end);

  if (isCurrent(e, now)) g.drawBox(2, y + 5, 3, 18);
  g.setFont(u8g2_font_helvB12_tf);
  g.drawUTF8(12, y + 19, range);
  const int subjectX = 124;
  drawUTF8Fit(g, subjectX, y + 19, SCREEN_W - subjectX - 10, e.subject[0] ? e.subject : "(no subject)");
  if (separator) drawDottedHLine(g, 12, y + 27, SCREEN_W - 24);
}

void drawCalendarMessage(U8G2 &g, int centerY, const char *title, const char *detail)
{
  g.setFont(u8g2_font_helvB14_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(title)) / 2, centerY, title);
  if (!detail) return;
  g.setFont(u8g2_font_6x13_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(detail)) / 2, centerY + 22, detail);
}

void drawCalendarFooter(U8G2 &g, const DashboardState &s, const CalendarView &c)
{
  char left[48], hm[8];
  formatHM(c.lastUpdate, hm, sizeof(hm));
  if (!c.hasData)    strcpy(left, "Waiting for the PC calendar bridge");
  else if (c.stale)  snprintf(left, sizeof(left), "No update from the PC since %s", hm);
  else               snprintf(left, sizeof(left), "Synced %s", hm);
  drawFooterText(g, left, s.alertsMuted ? "KEY: screen  BOOT: unmute" : "KEY: screen  BOOT: mute");
}

// Reading of a metric for the title row of the other card: "Humidity 48%".
void formatOther(Metric metric, const DashboardState &s, char *out, size_t size)
{
  if (metric == Metric::Temperature) {
    if (s.sensorPresent && !isnan(s.humidity)) snprintf(out, size, "Humidity %.0f%%", s.humidity);
    else                                       snprintf(out, size, "Humidity --");
  } else {
    if (s.sensorPresent && !isnan(s.temperature)) snprintf(out, size, "Temp %.1f\xC2\xB0""C", s.temperature);
    else                                          snprintf(out, size, "Temp --");
  }
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
  if (s.chartView == ChartView::Both) {
    drawCard(g, 6, 144, 190, 136, Metric::Temperature, s);
    drawCard(g, 204, 144, 190, 136, Metric::Humidity, s);
  } else {
    const Metric metric = s.chartView == ChartView::Temperature ? Metric::Temperature : Metric::Humidity;
    char other[24];
    formatOther(metric, s, other, sizeof(other));
    drawCard(g, 6, 144, 388, 136, metric, s, other);
  }
  drawFooter(g, s);
  g.sendBuffer();
}

void uiDrawCalendar(U8G2 &g, const DashboardState &s, const CalendarView &c)
{
  g.clearBuffer();
  drawHeader(g, s);
  drawCalendarTitle(g, c);

  if (c.count == 0) {
    if (c.hasData) drawCalendarMessage(g, 160, "No more meetings today", nullptr);
    else           drawCalendarMessage(g, 150, "No calendar yet",
                                       "Run tools/calendar-bridge on your PC");
  } else {
    drawMainEvent(g, *c.events[0], c.now, 6, 60, SCREEN_W - 12, 100);
    for (int i = 1; i < c.count; i++) {
      drawEventRow(g, *c.events[i], c.now, 166 + (i - 1) * 28, i < c.count - 1);
    }
  }

  drawCalendarFooter(g, s, c);
  g.sendBuffer();
}
