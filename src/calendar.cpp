#include "calendar.h"

#include <stdlib.h>
#include <string.h>

namespace {

constexpr int MAX_LINE = 256;
constexpr int MAX_FIELDS = 5;

// Copies at most dstSize - 1 bytes without cutting a UTF-8 sequence in half.
void copyUtf8(char *dst, size_t dstSize, const char *src)
{
  size_t len = strlen(src);
  if (len >= dstSize) {
    len = dstSize - 1;
    while (len > 0 && (static_cast<uint8_t>(src[len]) & 0xC0) == 0x80) len--;
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

// Splits `line` in place on tabs. Returns the number of fields.
int splitFields(char *line, char *fields[], int maxFields)
{
  int n = 0;
  char *p = line;
  while (n < maxFields) {
    fields[n++] = p;
    p = strchr(p, '\t');
    if (!p) break;
    *p++ = '\0';
  }
  return n;
}

// Reads the next line of `text` into `buf` (without CR/LF) and advances `text`.
bool nextLine(const char *&text, char *buf, size_t bufSize)
{
  if (*text == '\0') return false;
  const char *eol = strchr(text, '\n');
  size_t len = eol ? static_cast<size_t>(eol - text) : strlen(text);
  if (len > 0 && text[len - 1] == '\r') len--;
  if (len >= bufSize) len = bufSize - 1;  // Overlong lines are truncated
  memcpy(buf, text, len);
  buf[len] = '\0';
  text = eol ? eol + 1 : text + strlen(text);
  return true;
}

}  // namespace

int Calendar::update(const char *body, time_t receivedAt)
{
  char line[MAX_LINE];
  char *fields[MAX_FIELDS];

  if (!body || !nextLine(body, line, sizeof(line))) return -1;
  if (splitFields(line, fields, MAX_FIELDS) < 1 || strcmp(fields[0], "v1") != 0) return -1;

  CalendarEvent parsed[CALENDAR_MAX_EVENTS];
  int count = 0;
  int dropped = 0;
  while (nextLine(body, line, sizeof(line))) {
    if (line[0] == '\0') continue;
    if (count == CALENDAR_MAX_EVENTS) {  // The PC sends them in start order: drop the latest
      dropped++;
      continue;
    }

    int n = splitFields(line, fields, MAX_FIELDS);
    if (n < 3) return -1;

    CalendarEvent &e = parsed[count];
    e.id = strtoul(fields[0], nullptr, 10);
    e.start = static_cast<time_t>(strtoll(fields[1], nullptr, 10));
    e.end = static_cast<time_t>(strtoll(fields[2], nullptr, 10));
    if (e.start <= 0 || e.end < e.start) return -1;
    copyUtf8(e.subject, sizeof(e.subject), n > 3 ? fields[3] : "");
    copyUtf8(e.location, sizeof(e.location), n > 4 ? fields[4] : "");

    // Keep the alerts already played for this event
    for (int i = 0; i < _count; i++) {
      if (_events[i].id == e.id) {
        e.alertedEarly = _events[i].alertedEarly;
        e.alertedStart = _events[i].alertedStart;
        break;
      }
    }
    count++;
  }

  // Insertion sort by start time (tiny list)
  for (int i = 1; i < count; i++) {
    CalendarEvent e = parsed[i];
    int j = i - 1;
    for (; j >= 0 && parsed[j].start > e.start; j--) parsed[j + 1] = parsed[j];
    parsed[j + 1] = e;
  }

  // Latest version wins: events missing from this push are gone
  for (int i = 0; i < count; i++) _events[i] = parsed[i];
  _count = count;
  _dropped = dropped;
  _lastUpdate = receivedAt;
  return count;
}

int Calendar::upcoming(time_t now, const CalendarEvent **out, int maxCount) const
{
  int n = 0;
  for (int i = 0; i < _count && n < maxCount; i++) {
    if (_events[i].end > now) out[n++] = &_events[i];
  }
  return n;
}

CalendarAlert Calendar::pollAlerts(time_t now, const CalendarEvent **event)
{
  const time_t early = CALENDAR_ALERT_EARLY_MIN * 60;

  for (int i = 0; i < _count; i++) {
    CalendarEvent &e = _events[i];
    if (event) *event = &e;

    if (!e.alertedStart && now >= e.start) {
      // Started: the early alert is pointless now. Stay silent if we are
      // well past the start (e.g. booted or got the push mid-meeting).
      e.alertedEarly = e.alertedStart = true;
      if (now - e.start <= static_cast<time_t>(CALENDAR_ALERT_LATE_SEC)) return CalendarAlert::Start;
    } else if (!e.alertedEarly && now >= e.start - early) {
      e.alertedEarly = true;
      return CalendarAlert::Early;
    }
  }
  if (event) *event = nullptr;
  return CalendarAlert::None;
}
