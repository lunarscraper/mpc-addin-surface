/* midi.c: see midi.h. */
#include "midi.h"

#include <stdio.h>
#include <string.h>

void mpcsf_parser_init(mpcsf_parser *p) { memset(p, 0, sizeof *p); }

/* Data bytes after a status byte; -1 for SysEx start, 0 for no data. */
static int data_len(uint8_t st) {
  if (st < 0xF0) {
    uint8_t hi = st & 0xF0;
    return (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
  }
  switch (st) {
    case 0xF0: return -1;
    case 0xF1: case 0xF3: return 1;
    case 0xF2: return 2;
    default: return 0;
  }
}

static int emit(mpcsf_msg *m, uint8_t st, const uint8_t *d, int n) {
  m->bytes[0] = st;
  for (int i = 0; i < n; i++) m->bytes[1 + i] = d[i];
  m->len = (uint32_t)n + 1;
  return 1;
}

static int end_sysex(mpcsf_parser *p, mpcsf_msg *m, int terminated) {
  uint32_t keep = p->sysex_len < MPCSF_SYSEX_KEEP ? p->sysex_len : MPCSF_SYSEX_KEEP;
  memcpy(m->bytes, p->sysex, keep);
  m->len = p->sysex_len;
  if (terminated) {
    if (m->len < MPCSF_SYSEX_KEEP) m->bytes[m->len] = 0xF7;
    m->len++;
  }
  p->in_sysex = 0;
  p->sysex_len = 0;
  return 1;
}

int mpcsf_parser_feed(mpcsf_parser *p, uint8_t b, mpcsf_msg *m) {
  if (b >= 0xF8) return emit(m, b, NULL, 0);          /* realtime: anywhere, state untouched */
  if (b & 0x80) {
    int done = 0;
    if (p->in_sysex) done = end_sysex(p, m, b == 0xF7);
    if (b == 0xF7) { p->status = 0; p->have = 0; return done; }
    int n = data_len(b);
    p->have = 0;
    if (n < 0) {
      p->in_sysex = 1; p->sysex[0] = 0xF0; p->sysex_len = 1; p->status = 0;
      return done;
    }
    p->status = b < 0xF0 ? b : 0;                      /* system common cancels running status */
    if (n == 0) {
      /* an unterminated SysEx and a data-less status in the same byte: report the SysEx, the
       * status (tune request etc.) is rare enough to lose in that corner */
      return done ? done : emit(m, b, NULL, 0);
    }
    if (b >= 0xF0) { p->status = b; }                  /* keep it until its data arrived */
    return done;
  }
  /* data byte */
  if (p->in_sysex) {
    if (p->sysex_len < MPCSF_SYSEX_KEEP) p->sysex[p->sysex_len] = b;
    p->sysex_len++;
    return 0;
  }
  if (!p->status) return 0;                            /* stray data */
  p->data[p->have++] = b;
  int need = data_len(p->status);
  if (p->have < need) return 0;
  uint8_t st = p->status;
  p->have = 0;
  if (st >= 0xF0) p->status = 0;                       /* no running status for system common */
  return emit(m, st, p->data, need);
}

int mpcsf_msg_is_pressure(const mpcsf_msg *m) {
  if (!m->len || m->bytes[0] >= 0xF0) return 0;
  uint8_t hi = m->bytes[0] & 0xF0;
  return hi == 0xA0 || hi == 0xD0;
}

char *mpcsf_msg_describe(const mpcsf_msg *m, char *out, size_t n) {
  size_t o = 0;
  uint32_t shown = m->len < MPCSF_SYSEX_KEEP ? m->len : MPCSF_SYSEX_KEEP;
  if (n) out[0] = 0;
  for (uint32_t i = 0; i < shown && o + 4 < n; i++)
    o += (size_t)snprintf(out + o, n - o, "%02X ", m->bytes[i]);
  if (o >= n) return out;
  uint8_t st = m->bytes[0];
  int ch = (st & 0x0F) + 1, a = m->bytes[1], b = m->bytes[2];
  const char *more = m->len > shown ? " ..." : "";
  if (st == 0xF0) { snprintf(out + o, n - o, " sysex  %u bytes%s", (unsigned)m->len, more); return out; }
  if (st >= 0xF0) { snprintf(out + o, n - o, " system"); return out; }
  switch (st & 0xF0) {
    case 0x80: snprintf(out + o, n - o, " note-off  ch%d note %d vel %d", ch, a, b); break;
    case 0x90: snprintf(out + o, n - o, " %s  ch%d note %d vel %d", b ? "note-on" : "note-off(on, vel 0)", ch, a, b); break;
    case 0xA0: snprintf(out + o, n - o, " poly-pressure  ch%d note %d value %d", ch, a, b); break;
    case 0xB0: snprintf(out + o, n - o, " cc  ch%d cc %d value %d", ch, a, b); break;
    case 0xC0: snprintf(out + o, n - o, " program  ch%d program %d", ch, a); break;
    case 0xD0: snprintf(out + o, n - o, " channel-pressure  ch%d value %d", ch, a); break;
    default:   snprintf(out + o, n - o, " pitch-bend  ch%d value %d", ch, (b << 7 | a) - 8192); break;
  }
  return out;
}
