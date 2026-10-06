/* fake_libs.c: stands in for libasound in the host test.
 * Raw MIDI: every input handle reads from one pipe, the "device"; the test writes device bytes with
 * fake_dev_write(). Sequencer: three clients (the surface, a controller, a program); the test feeds
 * events with fake_seq_push(). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "../src/seq_abi.h"

typedef struct { int is_input, nonblock; } fake_rawmidi;

int fake_open_calls, fake_close_calls;
static int dev[2] = {-1, -1}, evp[2] = {-1, -1};

static void init(void) {
  if (dev[0] >= 0) return;
  if (pipe(dev) != 0 || pipe(evp) != 0) abort();
}

void fake_dev_write(const void *p, size_t n) { init(); if (write(dev[1], p, n) != (ssize_t)n) abort(); }

int snd_rawmidi_open(fake_rawmidi **inp, fake_rawmidi **outp, const char *name, int mode) {
  init();
  fake_open_calls++;
  if (name && !strcmp(name, "hw:9,0,0")) return -ENOENT;
  if (inp) { *inp = calloc(1, sizeof **inp); (*inp)->is_input = 1; (*inp)->nonblock = (mode & 2) != 0; }
  if (outp) *outp = calloc(1, sizeof **outp);
  return 0;
}
int snd_rawmidi_close(fake_rawmidi *h) { fake_close_calls++; free(h); return 0; }
int snd_rawmidi_nonblock(fake_rawmidi *h, int nb) { h->nonblock = nb != 0; return 0; }
int snd_rawmidi_poll_descriptors_count(fake_rawmidi *h) { (void)h; return 1; }
int snd_rawmidi_poll_descriptors(fake_rawmidi *h, struct pollfd *p, unsigned int space) {
  (void)h;
  if (space < 1) return 0;
  p[0].fd = dev[0]; p[0].events = POLLIN; p[0].revents = 0;
  return 1;
}
int snd_rawmidi_poll_descriptors_revents(fake_rawmidi *h, struct pollfd *p, unsigned int n, unsigned short *rev) {
  (void)h;
  if (n != 1) return -EINVAL;
  *rev = (unsigned short)p[0].revents;
  return 0;
}
ssize_t snd_rawmidi_read(fake_rawmidi *h, void *buf, size_t size) {
  if (!h || !h->is_input) return -EAGAIN;
  int fl = fcntl(dev[0], F_GETFL);
  fcntl(dev[0], F_SETFL, h->nonblock ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
  ssize_t r = read(dev[0], buf, size);
  return r < 0 ? -errno : r;
}

/* ---- sequencer ---- */
typedef struct { int client, type; char name[32]; } fake_ci;
typedef struct { int client, port; } fake_pi;
static const fake_ci clients[] = {
    {0, 2, "System"}, {14, 2, "Midi Through"}, {20, 2, "Akai Pro Force"}, {24, 2, "Faderfox EC4"}, {128, 1, "Acid"}};
#define NCLIENTS (int)(sizeof clients / sizeof clients[0])

int fake_connects[8][2], fake_n_connects, fake_port_caps;

void fake_seq_push(int type, int ch, int num, int val, int src) {
  mpcsf_seq_event e;
  memset(&e, 0, sizeof e);
  e.type = (unsigned char)type; e.source.client = (unsigned char)src;
  if (type == MPCSF_SEQ_EV_CONTROLLER) { e.data.control.channel = (unsigned char)ch; e.data.control.param = (unsigned)num; e.data.control.value = val; }
  else { e.data.note.channel = (unsigned char)ch; e.data.note.note = (unsigned char)num; e.data.note.velocity = (unsigned char)val; }
  init();
  if (write(evp[1], &e, sizeof e) != sizeof e) abort();
}

int snd_seq_open(void **seq, const char *name, int streams, int mode) { (void)name; (void)mode; init(); *seq = (void *)clients; return streams == 2 ? 0 : -EINVAL; }
int snd_seq_set_client_name(void *s, const char *n) { (void)s; (void)n; return 0; }
int snd_seq_client_id(void *s) { (void)s; return 129; }
int snd_seq_create_simple_port(void *s, const char *n, unsigned caps, unsigned type) { (void)s; (void)n; (void)type; fake_port_caps = (int)caps; return 0; }
int snd_seq_connect_from(void *s, int my, int c, int p) {
  (void)s; (void)my;
  if (fake_n_connects < 8) { fake_connects[fake_n_connects][0] = c; fake_connects[fake_n_connects][1] = p; fake_n_connects++; }
  return 0;
}
int snd_seq_event_input(void *s, mpcsf_seq_event **ev) {
  static mpcsf_seq_event cur;
  (void)s;
  if (read(evp[0], &cur, sizeof cur) != sizeof cur) return -EIO;
  *ev = &cur;
  return 0;
}
size_t snd_seq_client_info_sizeof(void) { return sizeof(fake_ci); }
void snd_seq_client_info_set_client(fake_ci *i, int c) { i->client = c; }
int snd_seq_client_info_get_client(const fake_ci *i) { return i->client; }
const char *snd_seq_client_info_get_name(fake_ci *i) { return i->name; }
int snd_seq_client_info_get_type(const fake_ci *i) { return i->type; }
int snd_seq_query_next_client(void *s, fake_ci *i) {
  (void)s;
  for (int k = 0; k < NCLIENTS; k++) if (clients[k].client > i->client) { *i = clients[k]; return 0; }
  return -ENOENT;
}
int snd_seq_get_any_client_info(void *s, int c, fake_ci *i) {
  (void)s;
  for (int k = 0; k < NCLIENTS; k++) if (clients[k].client == c) { *i = clients[k]; return 0; }
  return -ENOENT;
}
size_t snd_seq_port_info_sizeof(void) { return sizeof(fake_pi); }
void snd_seq_port_info_set_client(fake_pi *i, int c) { i->client = c; }
void snd_seq_port_info_set_port(fake_pi *i, int p) { i->port = p; }
int snd_seq_port_info_get_port(const fake_pi *i) { return i->port; }
unsigned int snd_seq_port_info_get_capability(const fake_pi *i) { return i->port == 0 ? 0x21u : 0x42u; }   /* port 0 readable, port 1 writable */
int snd_seq_query_next_port(void *s, fake_pi *i) { (void)s; if (i->port >= 1) return -ENOENT; i->port++; return 0; }
