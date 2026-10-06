/* midi.h: a byte-stream MIDI parser (running status, realtime bytes inside messages, SysEx) and a
 * one-line description of a message. No allocation, no I/O. */
#ifndef MPCSF_MIDI_H
#define MPCSF_MIDI_H

#include <stddef.h>
#include <stdint.h>

#define MPCSF_SYSEX_KEEP 48   /* bytes of a SysEx kept for the log; the true length is counted */

typedef struct {
  uint8_t status;             /* running status, 0 = none */
  uint8_t data[2];
  uint8_t have;
  int in_sysex;
  uint8_t sysex[MPCSF_SYSEX_KEEP];
  uint32_t sysex_len;
} mpcsf_parser;

typedef struct {
  uint8_t bytes[MPCSF_SYSEX_KEEP];
  uint32_t len;               /* true length; for a SysEx it can exceed what `bytes` holds */
} mpcsf_msg;

void mpcsf_parser_init(mpcsf_parser *p);
/* Feed one byte. Returns 1 and fills *m when a message is complete. */
int mpcsf_parser_feed(mpcsf_parser *p, uint8_t b, mpcsf_msg *m);
/* 1 for poly/channel aftertouch (pad pressure). */
int mpcsf_msg_is_pressure(const mpcsf_msg *m);
/* e.g. "90 03 7F  note-on  ch1 note 3 vel 127". Returns out. */
char *mpcsf_msg_describe(const mpcsf_msg *m, char *out, size_t n);

#endif
