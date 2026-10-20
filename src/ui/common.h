/*
  common.h - What the screens share: the layout constants, the header and footer,
  and a few text helpers. Internal to src/ui/; the public API is ui.h.
*/
#pragma once

#include <U8g2lib.h>
#include <time.h>

#include "ui.h"

namespace ui {

constexpr int SCREEN_W = 400;
constexpr int SCREEN_H = 300;
constexpr int HEADER_H = 24;
constexpr int FOOTER_H = 16;

inline constexpr const char *const WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                                  "Thursday", "Friday", "Saturday"};
inline constexpr const char *const MONTHS[] = {"January", "February", "March", "April", "May", "June",
                                                "July", "August", "September", "October", "November",
                                                "December"};

// Black bar with WiFi (only while connected), muted speaker (only while muted), page
// dots and battery. Leaves the draw color at 1.
void drawHeader(U8G2 &g, const DashboardState &s);

// Line above the bottom edge with a small text on each side.
void drawFooterText(U8G2 &g, const char *left, const char *right);

// "HH:MM" of a Unix time, in the local time zone.
void formatHM(time_t t, char *out, size_t size);

// Draws `text`, cut at a UTF-8 character boundary and ended with "..." if wider than maxW.
void drawUTF8Fit(U8G2 &g, int x, int y, int maxW, const char *text);

// Centered message with an optional smaller line under it.
void drawMessage(U8G2 &g, int centerY, const char *title, const char *detail);

}  // namespace ui
