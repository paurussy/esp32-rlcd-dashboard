#include "common.h"

#include <Arduino.h>

namespace ui {

namespace {

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

// ============================================================
//                          HEADER
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

}  // namespace

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

void formatHM(time_t t, char *out, size_t size)
{
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(out, size, "%02d:%02d", lt.tm_hour, lt.tm_min);
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

void drawMessage(U8G2 &g, int centerY, const char *title, const char *detail)
{
  g.setFont(u8g2_font_helvB14_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(title)) / 2, centerY, title);
  if (!detail) return;
  g.setFont(u8g2_font_6x13_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(detail)) / 2, centerY + 22, detail);
}

}  // namespace ui
