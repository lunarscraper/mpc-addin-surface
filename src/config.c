/* config.c: see config.h. */
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mpcsf_cfg_defaults(mpcsf_cfg *c) {
  memset(c, 0, sizeof *c);
  c->enabled = 1;
  strcpy(c->log_path, "auto");
  c->log_pressure = 0;
  c->max_lines = 200;
}

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

static int to_int(const char *v, int lo, int hi, int *out) {
  char *end;
  long x = strtol(v, &end, 10);
  if (end == v || *end || x < lo || x > hi) return -1;
  *out = (int)x;
  return 0;
}

static int set_str(char *dst, size_t n, const char *v) {
  if (strlen(v) >= n) return -1;
  strcpy(dst, v);
  return 0;
}

int mpcsf_cfg_load(mpcsf_cfg *c, const char *path, char *err, size_t errn) {
  mpcsf_cfg_defaults(c);
  if (err && errn) err[0] = 0;
  FILE *f = path ? fopen(path, "re") : NULL;
  if (!f) return -1;
  char line[512];
  int bad = 0, no = 0;
  while (fgets(line, sizeof line, f)) {
    no++;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *k = trim(line);
    if (!*k) continue;
    char *eq = strchr(k, '=');
    int r = -1;
    if (eq) {
      *eq = 0;
      char *v = trim(eq + 1);
      k = trim(k);
      if (!strcmp(k, "enabled")) r = to_int(v, 0, 1, &c->enabled);
      else if (!strcmp(k, "log")) r = set_str(c->log_path, sizeof c->log_path, v);
      else if (!strcmp(k, "device")) r = set_str(c->device, sizeof c->device, v);
      else if (!strcmp(k, "log_pressure")) r = to_int(v, 0, 1, &c->log_pressure);
      else if (!strcmp(k, "max_lines")) r = to_int(v, 1, 100000, &c->max_lines);
    }
    if (r < 0) {
      if (!bad && err && errn) snprintf(err, errn, "line %d: '%.40s'", no, k);
      bad++;
    }
  }
  fclose(f);
  return bad;
}
