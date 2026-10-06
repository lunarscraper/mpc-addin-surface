/* fake_mpc.c: stands in for /usr/bin/MPC (the binary must be named MPC for the addin to activate).
 * Makes the raw MIDI calls MPC makes and checks that the bytes arrive unchanged and get logged.
 * Usage: MPC <log file> [inert] */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "t.h"

typedef struct fake_rawmidi fake_rawmidi;
int snd_rawmidi_open(fake_rawmidi **, fake_rawmidi **, const char *, int);
int snd_rawmidi_close(fake_rawmidi *);
ssize_t snd_rawmidi_read(fake_rawmidi *, void *, size_t);
extern int fake_open_calls, fake_close_calls, fake_read_calls;

static char logtext[16384];
static void slurp(const char *path) {
  FILE *f = fopen(path, "r");
  logtext[0] = 0;
  if (f) { size_t r = fread(logtext, 1, sizeof logtext - 1, f); logtext[r] = 0; fclose(f); }
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  int (*t_active)(void);
  unsigned long (*t_chunks)(void);
  *(void **)&t_active = dlsym(RTLD_DEFAULT, "mpcsf_test_active");
  *(void **)&t_chunks = dlsym(RTLD_DEFAULT, "mpcsf_test_chunks");
  CHECK(t_active && t_chunks);
  if (t_fail) T_DONE("hooks: test helpers present");
  int inert = argc > 2;
  CHECK_EQ(t_active(), !inert);

  fake_rawmidi *in = NULL, *out = NULL, *out2 = NULL, *bad = NULL;
  CHECK_EQ(snd_rawmidi_open(&bad, NULL, "hw:9,0,0", 0), -ENOENT);   /* a failing open stays a failing open */
  CHECK_EQ(snd_rawmidi_open(&in, NULL, "hw:0,0,0", 0), 0);
  CHECK_EQ(snd_rawmidi_open(NULL, &out, "hw:0,0,0", 0), 0);
  CHECK_EQ(snd_rawmidi_open(NULL, &out2, "hw:0,0,0", 0), 0);
  CHECK_EQ(fake_open_calls, 4);

  /* MPC gets exactly the device's bytes, chunk by chunk */
  unsigned char buf[64];
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), 3);
  CHECK(!memcmp(buf, "\x90\x03\x7F", 3));
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), 2);
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), 6);
  CHECK(!memcmp(buf, "\x00\xA0\x24\x33\xB0\x10", 6));
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), 1);
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), -EAGAIN);
  CHECK_EQ(snd_rawmidi_read(out, buf, sizeof buf), -EAGAIN);        /* not an input: passed through */
  CHECK_EQ(fake_read_calls, 6);
  CHECK_EQ(t_chunks(), inert ? 0 : 4);

  usleep(300 * 1000);   /* the logger thread wakes every 10 ms */
  CHECK_EQ(snd_rawmidi_close(in), 0);
  CHECK_EQ(snd_rawmidi_close(out), 0);
  CHECK_EQ(snd_rawmidi_close(out2), 0);
  CHECK_EQ(fake_close_calls, 3);

  slurp(argv[1]);
  if (inert) {
    CHECK(logtext[0] == 0);
    T_DONE("hooks: inert outside MPC");
  }
  CHECK(strstr(logtext, "active: monitoring only") != NULL);
  CHECK(strstr(logtext, "input opened: hw:0,0,0 (logging)") != NULL);
  CHECK(strstr(logtext, "output opened: hw:0,0,0") != NULL);
  CHECK(strstr(logtext, "in hw:0,0,0   90 03 7F  note-on  ch1 note 3 vel 127") != NULL);
  CHECK(strstr(logtext, "90 03 00  note-off(on, vel 0)  ch1 note 3 vel 0") != NULL);   /* split over two reads */
  CHECK(strstr(logtext, "B0 10 41  cc  ch1 cc 16 value 65") != NULL);
  CHECK(strstr(logtext, "poly-pressure") == NULL);                                    /* log_pressure=0 */
  CHECK(strstr(logtext, "input closed: hw:0,0,0") != NULL);
  if (t_fail) fputs(logtext, stderr);
  T_DONE("hooks: MPC process");
}
