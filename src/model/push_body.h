/*
  push_body.h - Helpers to parse the tab-separated bodies the PC pushes
  ----------------------------------------------------------------------
  Shared by calendar.cpp and tasks.cpp. No hardware access.
*/
#pragma once

#include <stdint.h>
#include <string.h>

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
