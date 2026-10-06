/* seq_abi.h: the few pieces of libasound's sequencer ABI the addin uses, written out by hand (the
 * addin is built without ALSA headers and resolves libasound at run time). These layouts and
 * numbers are part of the kernel/libasound ABI and do not change. */
#ifndef MPCSF_SEQ_ABI_H
#define MPCSF_SEQ_ABI_H

#include <stdint.h>

typedef struct snd_seq snd_seq_t;
typedef struct snd_seq_client_info snd_seq_client_info_t;
typedef struct snd_seq_port_info snd_seq_port_info_t;

typedef struct { unsigned char client, port; } mpcsf_seq_addr;

typedef struct {
  unsigned char type, flags, tag, queue;
  struct { unsigned int a, b; } time;
  mpcsf_seq_addr source, dest;
  union {
    struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } note;
    struct { unsigned char channel, unused[3]; unsigned int param; int value; } control;
    mpcsf_seq_addr addr;
    unsigned char raw8[12];
  } data;
} mpcsf_seq_event;

_Static_assert(sizeof(mpcsf_seq_event) == 28, "snd_seq_event_t is 28 bytes");

enum {
  MPCSF_SEQ_OPEN_INPUT = 2,
  MPCSF_SEQ_EV_NOTEON = 6,
  MPCSF_SEQ_EV_CONTROLLER = 10,
  MPCSF_SEQ_EV_PORT_START = 63,
  MPCSF_SEQ_CAP_READ = 1 << 0,
  MPCSF_SEQ_CAP_WRITE = 1 << 1,
  MPCSF_SEQ_CAP_SUBS_READ = 1 << 5,
  MPCSF_SEQ_CAP_NO_EXPORT = 1 << 7,
  MPCSF_SEQ_TYPE_MIDI_GENERIC = 1 << 1,
  MPCSF_SEQ_TYPE_APPLICATION = 1 << 20,
  MPCSF_SEQ_KERNEL_CLIENT = 2,
  MPCSF_SEQ_CLIENT_SYSTEM = 0,
  MPCSF_SEQ_PORT_ANNOUNCE = 1,
  MPCSF_RAWMIDI_NONBLOCK = 2,
};

#endif
