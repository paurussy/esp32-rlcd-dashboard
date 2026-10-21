/*
  tasks_screen.cpp - Tasks screen: pending tasks with the selected one highlighted
*/
#include <Arduino.h>

#include "common.h"
#include "ui.h"

using namespace ui;

namespace {

constexpr int TASK_ROWS = 6;
constexpr int TASK_ROW_H = 32;
constexpr int TASK_LIST_TOP = 58;

// "Tasks · 4 pending" on the left, HH:MM on the right.
void drawTasksTitle(U8G2 &g, const TasksView &t)
{
  char title[40];
  const int pending = t.list->count();
  if (pending > 0) snprintf(title, sizeof(title), "Tasks  \xC2\xB7  %d pending", pending);
  else             strcpy(title, "Tasks");
  g.setFont(u8g2_font_helvB12_tf);
  g.drawUTF8(10, 47, title);

  char clock[8];
  formatHM(t.now, clock, sizeof(clock));
  g.setFont(u8g2_font_helvB14_tf);
  g.drawUTF8(SCREEN_W - 10 - g.getUTF8Width(clock), 48, clock);
}

// One-line row: a bar for high priority and the text. The selected row is inverted.
void drawTaskRow(U8G2 &g, const Task &task, int y, bool selected)
{
  g.setDrawColor(1);
  if (selected) {
    g.drawRBox(4, y, SCREEN_W - 8, TASK_ROW_H - 2, 5);
    g.setDrawColor(0);
  }
  if (task.high) g.drawBox(10, y + 6, 3, 18);

  const int textX = 20;
  g.setFont(u8g2_font_helvB12_tf);
  drawUTF8Fit(g, textX, y + 21, SCREEN_W - textX - 12, task.text[0] ? task.text : "(no text)");
  g.setDrawColor(1);
}

// Tasks scrolled out of view above and below the list.
void drawTasksOverflow(U8G2 &g, int above, int below)
{
  if (above == 0 && below == 0) return;
  char text[48];
  if (above > 0 && below > 0) snprintf(text, sizeof(text), "%d more above, %d more below", above, below);
  else if (above > 0)         snprintf(text, sizeof(text), "%d more above", above);
  else                        snprintf(text, sizeof(text), "%d more below", below);
  g.setFont(u8g2_font_6x13_tf);
  g.drawUTF8((SCREEN_W - g.getUTF8Width(text)) / 2, 270, text);
}

void drawTasksFooter(U8G2 &g, const TasksView &t)
{
  char left[48], hm[8];
  formatHM(t.lastUpdate, hm, sizeof(hm));
  if (!t.hasData)    strcpy(left, "Waiting for the PC task page");
  else if (t.stale)  snprintf(left, sizeof(left), "No update from the PC since %s", hm);
  else               snprintf(left, sizeof(left), "Synced %s", hm);
  drawFooterText(g, left, t.list->count() > 0 ? "BOOT: next" : "KEY: screen");
}

}  // namespace

void uiDrawTasks(U8G2 &g, const DashboardState &s, const TasksView &t)
{
  g.clearBuffer();
  drawHeader(g, s);
  drawTasksTitle(g, t);

  const int count = t.list->count();
  if (count == 0) {
    if (t.hasData) drawMessage(g, 160, "Nothing pending", nullptr);
    else           drawMessage(g, 150, "No tasks yet", "Open the task page on your PC");
  } else {
    const int selected = constrain(t.selected, 0, count - 1);
    const int first = selected >= TASK_ROWS ? selected - TASK_ROWS + 1 : 0;  // Scrolls only when needed
    const int shown = min(TASK_ROWS, count - first);
    for (int i = 0; i < shown; i++) {
      drawTaskRow(g, t.list->at(first + i), TASK_LIST_TOP + i * TASK_ROW_H, first + i == selected);
    }
    drawTasksOverflow(g, first, count - first - shown);
  }

  drawTasksFooter(g, t);
  g.sendBuffer();
}
