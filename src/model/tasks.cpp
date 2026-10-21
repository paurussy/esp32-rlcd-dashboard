#include "tasks.h"

#include <stdlib.h>
#include <string.h>

#include "push_body.h"

namespace {

constexpr int MAX_LINE = 256;
constexpr int MAX_FIELDS = 3;

}  // namespace

int TaskList::update(const char *body, time_t receivedAt)
{
  char line[MAX_LINE];
  char *fields[MAX_FIELDS];

  if (!body || !nextLine(body, line, sizeof(line))) return -1;
  if (splitFields(line, fields, MAX_FIELDS) < 1 || strcmp(fields[0], "v1") != 0) return -1;

  Task parsed[TASKS_MAX_ITEMS];
  int count = 0;
  int pushed = 0;
  int dropped = 0;
  while (nextLine(body, line, sizeof(line))) {
    if (line[0] == '\0') continue;

    int n = splitFields(line, fields, MAX_FIELDS);
    if (n < 3) return -1;

    pushed++;
    if (count == TASKS_MAX_ITEMS) {  // The PC sends them in display order: drop the last
      dropped++;
      continue;
    }

    Task &t = parsed[count++];
    t.id = strtoul(fields[0], nullptr, 10);
    t.high = fields[1][0] == '1';
    copyUtf8(t.text, sizeof(t.text), fields[2]);
  }

  for (int i = 0; i < count; i++) _tasks[i] = parsed[i];
  _count = count;
  _dropped = dropped;
  _lastUpdate = receivedAt;
  return pushed;
}
