/* config.h: surface.conf (key=value lines, plus trigger lines "cc 16 102 = matrix"). */
#ifndef MPCSF_CONFIG_H
#define MPCSF_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define MPCSF_CONF_NAME "surface.conf"
#define MPCSF_MAX_MAPS 24
#define MPCSF_MAX_STEPS 8

enum { MPCSF_STEP_DOWN = 1, MPCSF_STEP_UP, MPCSF_STEP_TAP };
enum { MPCSF_TRIG_CC = 1, MPCSF_TRIG_NOTE };

typedef struct { uint8_t kind, note; } mpcsf_step;

typedef struct {
  uint8_t trig, channel, number;       /* channel 1..16 */
  uint8_t n_steps;
  mpcsf_step steps[MPCSF_MAX_STEPS];
  char text[48];                       /* the action as written, for the log */
} mpcsf_map;

typedef struct {
  int enabled;
  char log_path[192];   /* "auto": surface.log next to the .so; "": none */
  int log_surface;      /* log what the control surface sends (stage 1) */
  int log_pressure;
  int max_lines;        /* per second */

  int remote;           /* external MIDI triggers press buttons */
  char surface[32];     /* the control surface: substring of the raw MIDI input's ALSA name */
  char source[48];      /* only listen to sequencer clients whose name contains this; "": all hardware */
  char exclude[96];     /* comma list: never listen to clients whose name contains one of these */
  int log_external;     /* log notes and controllers arriving from the sources */
  int tap_ms, gap_ms;   /* how long a tap holds the button; pause between steps */

  mpcsf_map maps[MPCSF_MAX_MAPS];
  int n_maps;
} mpcsf_cfg;

void mpcsf_cfg_defaults(mpcsf_cfg *c);
/* Defaults, then the file. Returns -1 if the file can't be read (defaults stay), else the number of
 * bad lines (the first one described in err). A file without trigger lines keeps the default triggers. */
int mpcsf_cfg_load(mpcsf_cfg *c, const char *path, char *err, size_t errn);
/* "matrix", "tap:0B", "down:25 tap:60 up:25", "trackedit:60" ... -> steps. 0 or -1. */
int mpcsf_parse_action(const char *text, mpcsf_map *m);
/* 1 if `name` contains one of the comma-separated items of `list`. */
int mpcsf_list_match(const char *list, const char *name);

#endif
