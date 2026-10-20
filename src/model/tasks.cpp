#include "tasks.h"

#include <stdlib.h>
#include <string.h>

#include "push_body.h"

namespace {

constexpr int MAX_LINE = 256;
constexpr int MAX_FIELDS = 3;

}  // namespace

bool TaskList::hasAction(uint32_t id, bool markSeen)
{
  for (int i = 0; i < _actionCount; i++) {
    if (_actions[i].id == id) {
      if (markSeen) _actions[i].seen = true;
      return true;
    }
  }
  return false;
}

int TaskList::update(const char *body, time_t receivedAt)
{
  char line[MAX_LINE];
  char *fields[MAX_FIELDS];

  if (!body || !nextLine(body, line, sizeof(line))) return -1;
  if (splitFields(line, fields, MAX_FIELDS) < 1 || strcmp(fields[0], "v1") != 0) return -1;

  for (int i = 0; i < _actionCount; i++) _actions[i].seen = false;

  Task parsed[TASKS_MAX_ITEMS];
  int count = 0;
  int pushed = 0;
  int dropped = 0;
  while (nextLine(body, line, sizeof(line))) {
    if (line[0] == '\0') continue;

    int n = splitFields(line, fields, MAX_FIELDS);
    if (n < 3) return -1;

    const uint32_t id = strtoul(fields[0], nullptr, 10);
    pushed++;
    if (hasAction(id, true)) continue;  // Done or deleted here, the PC has not caught up
    if (count == TASKS_MAX_ITEMS) {     // The PC sends them in display order: drop the last
      dropped++;
      continue;
    }

    Task &t = parsed[count++];
    t.id = id;
    t.high = fields[1][0] == '1';
    copyUtf8(t.text, sizeof(t.text), fields[2]);
  }

  // Actions whose task is no longer pushed were applied by the PC
  int kept = 0;
  for (int i = 0; i < _actionCount; i++) {
    if (_actions[i].seen) _actions[kept++] = _actions[i];
  }
  _actionCount = kept;

  for (int i = 0; i < count; i++) _tasks[i] = parsed[i];
  _count = count;
  _dropped = dropped;
  _lastUpdate = receivedAt;
  return pushed;
}

void TaskList::queue(int index, bool deleted)
{
  if (index < 0 || index >= _count) return;
  if (_actionCount == static_cast<int>(sizeof(_actions) / sizeof(_actions[0]))) return;

  _actions[_actionCount].id = _tasks[index].id;
  _actions[_actionCount].deleted = deleted;
  _actions[_actionCount].seen = true;
  _actionCount++;

  for (int i = index; i + 1 < _count; i++) _tasks[i] = _tasks[i + 1];
  _count--;
}
