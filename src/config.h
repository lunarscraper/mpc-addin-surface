/* config.h: surface.conf (key=value lines). */
#ifndef MPCSF_CONFIG_H
#define MPCSF_CONFIG_H

#include <stddef.h>

#define MPCSF_CONF_NAME "surface.conf"

typedef struct {
  int enabled;
  char log_path[192];   /* "auto": surface.log next to the .so; "": none */
  char device[32];      /* substring of the ALSA name; "": every input */
  int log_pressure;
  int max_lines;        /* per second */
} mpcsf_cfg;

void mpcsf_cfg_defaults(mpcsf_cfg *c);
/* Defaults, then the file. Returns -1 if the file can't be read (defaults stay), else the number of
 * bad lines (the first one described in err). */
int mpcsf_cfg_load(mpcsf_cfg *c, const char *path, char *err, size_t errn);

#endif
