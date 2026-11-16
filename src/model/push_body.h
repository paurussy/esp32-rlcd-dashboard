/*
  push_body.h - Helpers to parse the tab-separated bodies the PC pushes
  ----------------------------------------------------------------------
  Shared by calendar.cpp and tasks.cpp. No hardware access.
*/
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Copies at most dstSize - 1 bytes without cutting a UTF-8 sequence in half.
inline void copyUtf8(char *dst, size_t dstSize, const char *src)
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
inline int splitFields(char *line, char *fields[], int maxFields)
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
inline bool nextLine(const char *&text, char *buf, size_t bufSize)
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

// First line of every body:
//   v1  <pc_now_epoch>
//   v2  <pc_now_epoch>  <content_hash>  [<extra>]
// The hash is computed by the PC over the lines that follow; the board only keeps it and
// reports it in GET /status, so the PC can tell whether the board holds its current
// content. <extra> depends on the body (tasks: the number of pending tasks on the PC).
struct PushHeader {
  int version = 0;
  time_t pcNow = 0;   // PC clock when it sent the body (0 if unknown)
  char hash[17] = "";
  long extra = -1;    // -1 when absent
};

// Parses the first line (modified in place). False if it is not a known header.
inline bool parseHeader(char *line, PushHeader &h)
{
  char *fields[4];
  const int n = splitFields(line, fields, 4);
  if (strcmp(fields[0], "v1") == 0) h.version = 1;
  else if (strcmp(fields[0], "v2") == 0) h.version = 2;
  else return false;

  if (n > 1) h.pcNow = static_cast<time_t>(strtoll(fields[1], nullptr, 10));
  if (h.version >= 2 && n > 2) {
    strncpy(h.hash, fields[2], sizeof(h.hash) - 1);
    h.hash[sizeof(h.hash) - 1] = '\0';
  }
  if (h.version >= 2 && n > 3 && fields[3][0]) h.extra = strtol(fields[3], nullptr, 10);
  return true;
}

// The PC clock in the header of `body`, without parsing the rest (0 if none).
inline time_t peekPcNow(const char *body)
{
  if (!body) return 0;
  char line[64];
  if (!nextLine(body, line, sizeof(line))) return 0;
  PushHeader h;
  return parseHeader(line, h) ? h.pcNow : 0;
}
