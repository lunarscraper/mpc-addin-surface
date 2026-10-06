/* log.c: see log.h. One O_APPEND fd; each line goes out in a single write(). The file is
 * truncated when it grows past 512 KiB so /data never fills up. */
#include "log.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_MAX (512 * 1024)

static int log_fd = -1;

void mpcsf_log_open(const char *path) {
  if (!path || !*path || log_fd >= 0) return;
  log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

void mpcsf_log(const char *fmt, ...) {
  if (log_fd < 0) return;
  struct stat st;
  if (fstat(log_fd, &st) == 0 && st.st_size > LOG_MAX && ftruncate(log_fd, 0) != 0) return;
  char line[512];
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm;
  gmtime_r(&ts.tv_sec, &tm);
  int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                   (int)(ts.tv_nsec / 1000000));
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  n += m < (int)(sizeof line - (size_t)n - 1) ? m : (int)(sizeof line - (size_t)n - 2);
  line[n++] = '\n';
  ssize_t w = write(log_fd, line, (size_t)n);
  (void)w;
}
