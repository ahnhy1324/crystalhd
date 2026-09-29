/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CRYSTALHD_PHASE1_PROGRESS_H
#define CRYSTALHD_PHASE1_PROGRESS_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  FILE *stream;
} Phase1Progress;

static inline int
phase1_progress_open(Phase1Progress *progress)
{
  const char *path = getenv("PHASE1_PROGRESS_FILE");

  progress->stream = NULL;
  if (path == NULL || *path == '\0')
    return 1;
  progress->stream = fopen(path, "wb");
  if (progress->stream == NULL)
    return 0;
  (void)setvbuf(progress->stream, NULL, _IONBF, 0);
  return 1;
}

static inline void
phase1_progress_write(Phase1Progress *progress, const char *format, ...)
{
  char buffer[512];
  va_list arguments;
  int formatted;
  size_t length;

  if (progress->stream == NULL)
    return;
  va_start(arguments, format);
  formatted = vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  if (formatted < 0)
    return;
  length = (size_t)formatted < sizeof(buffer) ? (size_t)formatted :
      sizeof(buffer) - 1;
  if (fwrite(buffer, 1, length, progress->stream) == length)
    (void)fflush(progress->stream);
}

static inline void
phase1_progress_close(Phase1Progress *progress)
{
  if (progress->stream != NULL)
    (void)fclose(progress->stream);
  progress->stream = NULL;
}

#endif
