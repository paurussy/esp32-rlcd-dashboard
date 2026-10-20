/*
  calendar_screen.cpp - Calendar screen: the meeting in progress or the next one, then the rest of the day
*/
#include <Arduino.h>

#include "common.h"
#include "ui.h"

using namespace ui;

namespace {

// "12 min", "1 h 05 min", "2 h". Rounds up so "0 min" never shows.
void formatDuration(time_t seconds, char *out, size_t size)
{
  long minutes = (static_cast<long>(seconds) + 59) / 60;
  if (minutes < 60)           snprintf(out, size, "%ld min", minutes);
  else if (minutes % 60 == 0) snprintf(out, size, "%ld h", minutes / 60);
  else                        snprintf(out, size, "%ld h %02ld min", minutes / 60, minutes % 60);
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

void drawCalendarFooter(U8G2 &g, const DashboardState &s, const CalendarView &c)
{
  char left[48], hm[8];
  formatHM(c.lastUpdate, hm, sizeof(hm));
  if (!c.hasData)    strcpy(left, "Waiting for the PC calendar bridge");
  else if (c.stale)  snprintf(left, sizeof(left), "No update from the PC since %s", hm);
  else               snprintf(left, sizeof(left), "Synced %s", hm);
  drawFooterText(g, left, s.alertsMuted ? "KEY: screen  BOOT: unmute" : "KEY: screen  BOOT: mute");
}

}  // namespace

void uiDrawCalendar(U8G2 &g, const DashboardState &s, const CalendarView &c)
{
  g.clearBuffer();
  drawHeader(g, s);
  drawCalendarTitle(g, c);

  if (c.count == 0) {
    if (c.hasData) drawMessage(g, 160, "No more meetings today", nullptr);
    else           drawMessage(g, 150, "No calendar yet", "Run calendar_bridge on your PC");
  } else {
    drawMainEvent(g, *c.events[0], c.now, 6, 60, SCREEN_W - 12, 100);
    for (int i = 1; i < c.count; i++) {
      drawEventRow(g, *c.events[i], c.now, 166 + (i - 1) * 28, i < c.count - 1);
    }
  }

  drawCalendarFooter(g, s, c);
  g.sendBuffer();
}
