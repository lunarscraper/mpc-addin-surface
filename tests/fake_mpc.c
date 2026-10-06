/* fake_mpc.c: stands in for /usr/bin/MPC (the binary must be named MPC for the addin to activate).
 * Makes the raw MIDI calls MPC makes and checks: device bytes arrive unchanged and get logged, and
 * a trigger from an external controller arrives as button messages, with a polling reader and with
 * a blocking one.
 * Usage: MPC <log file> [inert] */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "t.h"

typedef struct fake_rawmidi fake_rawmidi;
int snd_rawmidi_open(fake_rawmidi **, fake_rawmidi **, const char *, int);
int snd_rawmidi_close(fake_rawmidi *);
int snd_rawmidi_nonblock(fake_rawmidi *, int);
int snd_rawmidi_poll_descriptors_count(fake_rawmidi *);
int snd_rawmidi_poll_descriptors(fake_rawmidi *, struct pollfd *, unsigned int);
int snd_rawmidi_poll_descriptors_revents(fake_rawmidi *, struct pollfd *, unsigned int, unsigned short *);
ssize_t snd_rawmidi_read(fake_rawmidi *, void *, size_t);
void fake_dev_write(const void *, size_t);
void fake_seq_push(int type, int ch, int num, int val, int src);
extern int fake_open_calls, fake_close_calls, fake_connects[8][2], fake_n_connects, fake_port_caps;

static char logtext[32768];
static void slurp(const char *path) {
  FILE *f = fopen(path, "r");
  logtext[0] = 0;
  if (f) { size_t r = fread(logtext, 1, sizeof logtext - 1, f); logtext[r] = 0; fclose(f); }
}

static fake_rawmidi *in;

/* What MPC does with a non-blocking handle: poll its descriptors, ask for revents, read. */
static int poll_read(unsigned char *buf, size_t size, int timeout_ms) {
  struct pollfd p[4];
  int n = snd_rawmidi_poll_descriptors_count(in);
  if (n < 1 || n > 4 || snd_rawmidi_poll_descriptors(in, p, (unsigned)n) != n) return -1000;
  for (int tries = 0; tries < 200; tries++) {
    int pr = poll(p, (nfds_t)n, timeout_ms);
    if (pr <= 0) return -2000;
    unsigned short rev = 0;
    if (snd_rawmidi_poll_descriptors_revents(in, p, (unsigned)n, &rev) < 0) return -3000;
    if (!(rev & POLLIN)) continue;
    return (int)snd_rawmidi_read(in, buf, size);
  }
  return -4000;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  int (*t_active)(void);
  unsigned long (*t_delivered)(void);
  *(void **)&t_active = dlsym(RTLD_DEFAULT, "mpcsf_test_active");
  *(void **)&t_delivered = dlsym(RTLD_DEFAULT, "mpcsf_test_delivered");
  CHECK(t_active && t_delivered);
  if (t_fail) T_DONE("hooks: test helpers present");
  int inert = argc > 2;
  CHECK_EQ(t_active(), !inert);

  fake_rawmidi *out = NULL, *out2 = NULL, *bad = NULL;
  CHECK_EQ(snd_rawmidi_open(&bad, NULL, "hw:9,0,0", 0), -ENOENT);   /* a failing open stays a failing open */
  CHECK_EQ(snd_rawmidi_open(&in, NULL, "hw:0,0,1", 2), 0);          /* non-blocking, as a polling reader has it */
  CHECK_EQ(snd_rawmidi_open(NULL, &out, "hw:0,0,1", 0), 0);
  CHECK_EQ(snd_rawmidi_open(NULL, &out2, "hw:0,0,0", 0), 0);
  CHECK_EQ(fake_open_calls, 4);
  unsigned char buf[64];

  if (inert) {
    CHECK_EQ(snd_rawmidi_poll_descriptors_count(in), 1);
    fake_dev_write("\x90\x03\x7F", 3);
    CHECK_EQ(poll_read(buf, sizeof buf, 1000), 3);
    fake_seq_push(10, 15, 102, 127, 24);
    CHECK_EQ(poll_read(buf, sizeof buf, 300), -2000);               /* nothing is ever injected */
    snd_rawmidi_close(in); snd_rawmidi_close(out); snd_rawmidi_close(out2);
    slurp(argv[1]);
    CHECK(logtext[0] == 0);
    T_DONE("hooks: inert outside MPC");
  }

  /* 1. monitor: MPC gets exactly the device's bytes */
  CHECK_EQ(snd_rawmidi_poll_descriptors_count(in), 2);              /* the device and the wake-up pipe */
  fake_dev_write("\x90\x03\x7F", 3);
  CHECK_EQ(poll_read(buf, sizeof buf, 1000), 3);
  CHECK(!memcmp(buf, "\x90\x03\x7F", 3));
  fake_dev_write("\x90\x03", 2);                                    /* a message split over two reads */
  CHECK_EQ(poll_read(buf, sizeof buf, 1000), 2);
  fake_dev_write("\x00\xA0\x24\x33", 4);
  CHECK_EQ(poll_read(buf, sizeof buf, 1000), 4);
  CHECK(!memcmp(buf, "\x00\xA0\x24\x33", 4));
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), -EAGAIN);
  CHECK_EQ(snd_rawmidi_read(out, buf, sizeof buf), -EAGAIN);        /* not an input: passed through */

  /* 2. remote, polling reader: cc 102 on channel 16 from the controller taps Matrix */
  fake_seq_push(10, 15, 102, 0, 24);                                /* value 0: a release, no trigger */
  fake_seq_push(10, 15, 102, 127, 24);
  CHECK_EQ(poll_read(buf, sizeof buf, 2000), 3);
  CHECK(!memcmp(buf, "\x90\x03\x7F", 3));
  CHECK_EQ(poll_read(buf, sizeof buf, 2000), 3);
  CHECK(!memcmp(buf, "\x90\x03\x00", 3));

  /* a queued button waits while a real message is half read */
  fake_dev_write("\xB0\x10", 2);
  CHECK_EQ(poll_read(buf, sizeof buf, 1000), 2);
  fake_seq_push(10, 15, 103, 127, 24);                              /* Mixer */
  usleep(60 * 1000);
  fake_dev_write("\x41", 1);
  CHECK_EQ(poll_read(buf, sizeof buf, 1000), 1);                    /* the rest of the real message first */
  CHECK_EQ(buf[0], 0x41);
  CHECK_EQ(poll_read(buf, sizeof buf, 2000), 3);
  CHECK(!memcmp(buf, "\x90\x0B\x7F", 3));
  CHECK_EQ(poll_read(buf, sizeof buf, 2000), 3);
  CHECK(!memcmp(buf, "\x90\x0B\x00", 3));

  /* 3. remote, blocking reader: Track Edit = hold Edit, tap Track Select 0x60, release Edit */
  CHECK_EQ(snd_rawmidi_nonblock(in, 0), 0);
  fake_seq_push(10, 15, 104, 127, 24);
  static const unsigned char want[4][3] = {{0x90, 0x25, 0x7F}, {0x90, 0x60, 0x7F}, {0x90, 0x60, 0x00}, {0x90, 0x25, 0x00}};
  for (int i = 0; i < 4; i++) {
    CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), 3);
    CHECK(!memcmp(buf, want[i], 3));
  }
  fake_dev_write("\x90\x04\x7F", 3);                                /* the device still gets through */
  CHECK_EQ(snd_rawmidi_read(in, buf, sizeof buf), 3);
  CHECK(!memcmp(buf, "\x90\x04\x7F", 3));
  CHECK_EQ(t_delivered(), 8);

  /* 4. a note trigger nobody mapped does nothing; the listener only connected to the controller */
  CHECK_EQ(snd_rawmidi_nonblock(in, 1), 0);
  fake_seq_push(6, 0, 60, 100, 24);
  CHECK_EQ(poll_read(buf, sizeof buf, 300), -2000);
  CHECK_EQ(fake_n_connects, 2);
  CHECK_EQ(fake_connects[0][0], 0); CHECK_EQ(fake_connects[0][1], 1);     /* System:Announce */
  CHECK_EQ(fake_connects[1][0], 24); CHECK_EQ(fake_connects[1][1], 0);    /* the controller's readable port */
  CHECK_EQ(fake_port_caps, 0x82);                                         /* write + no-export */

  usleep(100 * 1000);   /* the logger thread wakes every 10 ms */
  CHECK_EQ(snd_rawmidi_close(in), 0);
  CHECK_EQ(snd_rawmidi_close(out), 0);
  CHECK_EQ(snd_rawmidi_close(out2), 0);
  CHECK_EQ(fake_close_calls, 3);

  slurp(argv[1]);
  CHECK(strstr(logtext, "active: monitor + remote, 3 trigger(s)") != NULL);
  CHECK(strstr(logtext, "input opened: hw:0,0,1 (logging) (control surface, non-blocking)") != NULL);
  CHECK(strstr(logtext, "output opened: hw:0,0,0") != NULL);
  CHECK(strstr(logtext, "in hw:0,0,1   90 03 7F  note-on  ch1 note 3 vel 127") != NULL);
  CHECK(strstr(logtext, "90 03 00  note-off(on, vel 0)  ch1 note 3 vel 0") != NULL);
  CHECK(strstr(logtext, "poly-pressure") == NULL);
  CHECK(strstr(logtext, "midi device 20 'Akai Pro Force': not listening (in exclude=)") != NULL);
  CHECK(strstr(logtext, "midi device 24 'Faderfox EC4': listening on 1 port(s)") != NULL);
  CHECK(strstr(logtext, "midi device 128 'Acid': not listening (a program, not a device)") != NULL);
  CHECK(strstr(logtext, "trigger cc ch16 104 from 'Faderfox EC4' -> trackedit:60") != NULL);
  CHECK(strstr(logtext, "ext 'Faderfox EC4'  note ch1 60 value 100") != NULL);
  CHECK(strstr(logtext, "not completed") == NULL);
  CHECK(strstr(logtext, "control surface closed") != NULL);
  if (t_fail) fputs(logtext, stderr);
  T_DONE("hooks: MPC process");
}
