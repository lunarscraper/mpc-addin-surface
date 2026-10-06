/* fake_libs.c: stands in for libasound's raw MIDI calls in the host test. Each input handle plays
 * back a fixed script, one chunk per read, then returns -EAGAIN. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

typedef struct { int is_input; int step; } fake_rawmidi;

int fake_open_calls, fake_close_calls, fake_read_calls;

int snd_rawmidi_open(fake_rawmidi **inp, fake_rawmidi **outp, const char *name, int mode) {
  (void)mode;
  fake_open_calls++;
  if (name && !strcmp(name, "hw:9,0,0")) return -ENOENT;
  if (inp) { *inp = calloc(1, sizeof **inp); (*inp)->is_input = 1; }
  if (outp) *outp = calloc(1, sizeof **outp);
  return 0;
}

int snd_rawmidi_close(fake_rawmidi *h) { fake_close_calls++; free(h); return 0; }

ssize_t snd_rawmidi_read(fake_rawmidi *h, void *buf, size_t size) {
  static const unsigned char s0[] = {0x90, 0x03, 0x7F};                    /* button down */
  static const unsigned char s1[] = {0x90, 0x03};                          /* split message ... */
  static const unsigned char s2[] = {0x00, 0xA0, 0x24, 0x33, 0xB0, 0x10};  /* ... its end, pressure, split cc */
  static const unsigned char s3[] = {0x41};
  static const struct { const unsigned char *p; size_t n; } script[] = {
      {s0, sizeof s0}, {s1, sizeof s1}, {s2, sizeof s2}, {s3, sizeof s3}};
  fake_read_calls++;
  if (!h || !h->is_input || h->step >= 4) return -EAGAIN;
  size_t n = script[h->step].n;
  if (n > size) return -EINVAL;
  memcpy(buf, script[h->step].p, n);
  h->step++;
  return (ssize_t)n;
}
