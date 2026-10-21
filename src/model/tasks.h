/*
  tasks.h - Pending tasks pushed by the PC task page
  --------------------------------------------------
  No hardware access: the sketch feeds it the body of each push and the UI reads the list.
  Sync is one way, PC => board: each push replaces the whole list.

  Push format (UTF-8 text, one record per line, fields separated by tabs), already in
  display order (the order set on the PC page):
    v1    <pc_now_epoch>
    <id>  <priority 0|1>  <text>
*/
#pragma once

#include <stdint.h>
#include <time.h>

#include "../../config.h"

struct Task {
  uint32_t id = 0;
  bool high = false;  // High priority
  char text[64] = "";
};

class TaskList {
public:
  // Replaces the list with the pushed tasks (latest version wins). Returns the number
  // of tasks pushed, or -1 if the body is malformed (the old list is kept).
  int update(const char *body, time_t receivedAt);

  // Tasks pushed beyond TASKS_MAX_ITEMS in the last update.
  int dropped() const { return _dropped; }

  int count() const { return _count; }
  const Task &at(int i) const { return _tasks[i]; }
  bool hasData() const { return _lastUpdate != 0; }
  time_t lastUpdate() const { return _lastUpdate; }

private:
  Task _tasks[TASKS_MAX_ITEMS];
  int _count = 0;
  int _dropped = 0;
  time_t _lastUpdate = 0;
};
