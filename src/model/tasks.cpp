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
  PushHeader header;
  if (!parseHeader(line, header)) return -1;

  // Static: too big for the stack of the web server's handler on top of everything else
  static Task parsed[TASKS_MAX_ITEMS];
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
  _total = header.extra > pushed ? static_cast<int>(header.extra) : pushed;
  _lastUpdate = receivedAt;
  strcpy(_hash, header.hash);
  return pushed;
}
