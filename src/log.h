/* log.h: append-only log file with millisecond timestamps. Only the logger thread and MPC's setup
 * threads call it; the raw MIDI read hook never does (it does file I/O). */
#ifndef MPCSF_LOG_H
#define MPCSF_LOG_H

void mpcsf_log_open(const char *path);   /* "" or NULL disables logging */
void mpcsf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
