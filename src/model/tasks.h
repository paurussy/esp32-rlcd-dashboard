/*
  tasks.h - Pending tasks pushed by the PC task page, and what was done with them on the board
  ---------------------------------------------------------------------------------------------
  No hardware access: the sketch feeds it the body of each push and the UI reads the list.

  Push format (UTF-8 text, one record per line, fields separated by tabs), already in
  display order (high priority first, then oldest first):
    v1    <pc_now_epoch>
    <id>  <priority 0|1>  <text>

  The board can finish or delete a task (BOOT). The PC cannot hear about it until its
  next push, so the answer to each push lists the pending actions ("done <id>",
  "deleted <id>"). An action stays pending until a push no longer contains its id,
  which means the PC applied it: a lost answer is simply sent again in the next window.
  Meanwhile the task stays hidden here even if the PC still sends it.
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
  // Replaces the list with the pushed tasks (latest version wins), skipping those
  // with a pending action. Returns the number of tasks pushed, or -1 if the body is
  // malformed (the old list is kept).
  int update(const char *body, time_t receivedAt);

  // Take task `index` off the list and queue the action for the PC.
  void markDone(int index) { queue(index, false); }
  void remove(int index) { queue(index, true); }

  // Actions the PC has not applied yet.
  int actionCount() const { return _actionCount; }
  uint32_t actionId(int i) const { return _actions[i].id; }
  bool actionIsDelete(int i) const { return _actions[i].deleted; }

  // Tasks pushed beyond TASKS_MAX_ITEMS in the last update.
  int dropped() const { return _dropped; }

  int count() const { return _count; }
  const Task &at(int i) const { return _tasks[i]; }
  bool hasData() const { return _lastUpdate != 0; }
  time_t lastUpdate() const { return _lastUpdate; }

private:
  struct Action {
    uint32_t id = 0;
    bool deleted = false;
    bool seen = false;  // Its id came in the push being parsed
  };

  void queue(int index, bool deleted);
  bool hasAction(uint32_t id, bool markSeen);

  Task _tasks[TASKS_MAX_ITEMS];
  int _count = 0;
  int _dropped = 0;
  time_t _lastUpdate = 0;

  Action _actions[2 * TASKS_MAX_ITEMS];
  int _actionCount = 0;
};
