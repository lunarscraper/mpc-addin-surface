/* test_midi.c: the stream parser and the log text. */
#include <string.h>
#include "../src/midi.h"
#include "t.h"

static int feed(mpcsf_parser *p, const uint8_t *b, int n, mpcsf_msg *out, int max) {
  int k = 0;
  mpcsf_msg m;
  for (int i = 0; i < n; i++)
    if (mpcsf_parser_feed(p, b[i], &m) && k < max) out[k++] = m;
  return k;
}

int main(void) {
  mpcsf_parser p;
  mpcsf_msg m[8];
  char t[256];

  /* note on, running status, realtime byte inside a message */
  mpcsf_parser_init(&p);
  const uint8_t a[] = {0x90, 0x03, 0x7F, 0x03, 0xF8, 0x00, 0xB0, 0x10, 0x41};
  CHECK_EQ(feed(&p, a, sizeof a, m, 8), 4);
  CHECK(!strcmp(mpcsf_msg_describe(&m[0], t, sizeof t), "90 03 7F  note-on  ch1 note 3 vel 127"));
  CHECK_EQ(m[1].bytes[0], 0xF8);
  CHECK(!strcmp(mpcsf_msg_describe(&m[2], t, sizeof t), "90 03 00  note-off(on, vel 0)  ch1 note 3 vel 0"));
  CHECK(!strcmp(mpcsf_msg_describe(&m[3], t, sizeof t), "B0 10 41  cc  ch1 cc 16 value 65"));

  /* a message split over two reads */
  mpcsf_parser_init(&p);
  const uint8_t b1[] = {0x99, 0x24}, b2[] = {0x40};
  CHECK_EQ(feed(&p, b1, 2, m, 8), 0);
  CHECK_EQ(feed(&p, b2, 1, m, 8), 1);
  CHECK(!strcmp(mpcsf_msg_describe(&m[0], t, sizeof t), "99 24 40  note-on  ch10 note 36 vel 64"));

  /* pressure is recognised; one-data-byte messages */
  mpcsf_parser_init(&p);
  const uint8_t c[] = {0xA0, 0x24, 0x10, 0xD0, 0x22, 0xC1, 0x05};
  CHECK_EQ(feed(&p, c, sizeof c, m, 8), 3);
  CHECK_EQ(mpcsf_msg_is_pressure(&m[0]), 1);
  CHECK_EQ(mpcsf_msg_is_pressure(&m[1]), 1);
  CHECK_EQ(mpcsf_msg_is_pressure(&m[2]), 0);
  CHECK(!strcmp(mpcsf_msg_describe(&m[2], t, sizeof t), "C1 05  program  ch2 program 5"));

  /* SysEx: short, and longer than what is kept; running status is gone afterwards */
  mpcsf_parser_init(&p);
  const uint8_t d[] = {0x90, 0x01, 0x02, 0xF0, 0x47, 0x7F, 0x40, 0xF7, 0x05, 0x06};
  CHECK_EQ(feed(&p, d, sizeof d, m, 8), 2);
  CHECK(!strcmp(mpcsf_msg_describe(&m[1], t, sizeof t), "F0 47 7F 40 F7  sysex  5 bytes"));
  mpcsf_parser_init(&p);
  uint8_t big[204];
  big[0] = 0xF0; memset(big + 1, 0x11, 202); big[203] = 0xF7;
  CHECK_EQ(feed(&p, big, sizeof big, m, 8), 1);
  CHECK_EQ(m[0].len, 204);
  CHECK(strstr(mpcsf_msg_describe(&m[0], t, sizeof t), "sysex  204 bytes ...") != NULL);

  /* stray data without a status is ignored; pitch bend centre */
  mpcsf_parser_init(&p);
  const uint8_t e[] = {0x12, 0x34, 0xE0, 0x00, 0x40};
  CHECK_EQ(feed(&p, e, sizeof e, m, 8), 1);
  CHECK(!strcmp(mpcsf_msg_describe(&m[0], t, sizeof t), "E0 00 40  pitch-bend  ch1 value 0"));
  T_DONE("midi: parser and text");
}
