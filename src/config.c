/* config.c: see config.h. */
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Button codes of the Force's control surface (note numbers on channel 1), read from a Force with
 * the stage 1 monitor on 2026-10-06. */
static const struct { const char *name; uint8_t note; } buttons[] = {
    {"menu", 0x02}, {"matrix", 0x03}, {"note", 0x04}, {"mixer", 0x0B},
    {"edit", 0x25}, {"launch", 0x74}, {"stepseq", 0x75}};

static int add_map(mpcsf_cfg *c, int trig, int ch, int num, const char *action) {
  if (c->n_maps >= MPCSF_MAX_MAPS) return -1;
  mpcsf_map *m = &c->maps[c->n_maps];
  memset(m, 0, sizeof *m);
  if (mpcsf_parse_action(action, m) < 0) return -1;
  m->trig = (uint8_t)trig; m->channel = (uint8_t)ch; m->number = (uint8_t)num;
  c->n_maps++;
  return 0;
}

static void default_maps(mpcsf_cfg *c) {
  c->n_maps = 0;
  add_map(c, MPCSF_TRIG_CC, 16, 102, "matrix");
  add_map(c, MPCSF_TRIG_CC, 16, 103, "mixer");
  add_map(c, MPCSF_TRIG_CC, 16, 104, "trackedit:60");
}

void mpcsf_cfg_defaults(mpcsf_cfg *c) {
  memset(c, 0, sizeof *c);
  c->enabled = 1;
  strcpy(c->log_path, "auto");
  c->log_surface = 1;
  c->max_lines = 200;
  c->remote = 1;
  strcpy(c->surface, "hw:0");
  strcpy(c->exclude, "Force,MPC,Through");
  c->log_external = 1;
  c->tap_ms = 40;
  c->gap_ms = 15;
  default_maps(c);
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

static int hex_note(const char *s, uint8_t *out) {
  char *end;
  long x = strtol(s, &end, 16);
  if (end == s || *end || x < 0 || x > 127) return -1;
  *out = (uint8_t)x;
  return 0;
}

static int push(mpcsf_map *m, int kind, uint8_t note) {
  if (m->n_steps >= MPCSF_MAX_STEPS) return -1;
  m->steps[m->n_steps].kind = (uint8_t)kind;
  m->steps[m->n_steps].note = note;
  m->n_steps++;
  return 0;
}

int mpcsf_parse_action(const char *text, mpcsf_map *m) {
  char buf[64];
  if (strlen(text) >= sizeof buf || strlen(text) >= sizeof m->text) return -1;
  strcpy(buf, text);
  m->n_steps = 0;
  char *save = NULL;
  for (char *t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
    uint8_t n;
    int found = 0;
    for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
      if (!strcmp(t, buttons[i].name)) { if (push(m, MPCSF_STEP_TAP, buttons[i].note) < 0) return -1; found = 1; }
    if (found) continue;
    if (!strncmp(t, "tap:", 4) && !hex_note(t + 4, &n)) { if (push(m, MPCSF_STEP_TAP, n) < 0) return -1; }
    else if (!strncmp(t, "down:", 5) && !hex_note(t + 5, &n)) { if (push(m, MPCSF_STEP_DOWN, n) < 0) return -1; }
    else if (!strncmp(t, "up:", 3) && !hex_note(t + 3, &n)) { if (push(m, MPCSF_STEP_UP, n) < 0) return -1; }
    else if (!strncmp(t, "trackedit:", 10) && !hex_note(t + 10, &n)) {   /* hold Edit, tap a Track Select */
      if (push(m, MPCSF_STEP_DOWN, 0x25) < 0 || push(m, MPCSF_STEP_TAP, n) < 0 || push(m, MPCSF_STEP_UP, 0x25) < 0) return -1;
    } else return -1;
  }
  if (!m->n_steps) return -1;
  strcpy(m->text, text);
  return 0;
}

int mpcsf_list_match(const char *list, const char *name) {
  char buf[128];
  if (!list || !name || strlen(list) >= sizeof buf) return 0;
  strcpy(buf, list);
  char *save = NULL;
  for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
    t = trim(t);
    if (*t && strstr(name, t)) return 1;
  }
  return 0;
}

/* "cc 16 102" or "note 16 60" */
static int parse_trigger(char *k, int *trig, int *ch, int *num) {
  char *save = NULL;
  char *w = strtok_r(k, " \t", &save), *a = strtok_r(NULL, " \t", &save), *b = strtok_r(NULL, " \t", &save);
  if (!w || !a || !b || strtok_r(NULL, " \t", &save)) return -1;
  if (!strcmp(w, "cc")) *trig = MPCSF_TRIG_CC;
  else if (!strcmp(w, "note")) *trig = MPCSF_TRIG_NOTE;
  else return -1;
  return to_int(a, 1, 16, ch) || to_int(b, 0, 127, num) ? -1 : 0;
}

int mpcsf_cfg_load(mpcsf_cfg *c, const char *path, char *err, size_t errn) {
  mpcsf_cfg_defaults(c);
  if (err && errn) err[0] = 0;
  FILE *f = path ? fopen(path, "re") : NULL;
  if (!f) return -1;
  char line[512];
  int bad = 0, no = 0, own_maps = 0;
  while (fgets(line, sizeof line, f)) {
    no++;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *k = trim(line);
    if (!*k) continue;
    char shown[41];
    snprintf(shown, sizeof shown, "%s", k);
    char *eq = strchr(k, '=');
    int r = -1;
    if (eq) {
      *eq = 0;
      char *v = trim(eq + 1);
      k = trim(k);
      if (!strcmp(k, "enabled")) r = to_int(v, 0, 1, &c->enabled);
      else if (!strcmp(k, "log")) r = set_str(c->log_path, sizeof c->log_path, v);
      else if (!strcmp(k, "device")) r = 0;   /* stage 1 key, replaced by surface= */
      else if (!strcmp(k, "log_surface")) r = to_int(v, 0, 1, &c->log_surface);
      else if (!strcmp(k, "log_pressure")) r = to_int(v, 0, 1, &c->log_pressure);
      else if (!strcmp(k, "max_lines")) r = to_int(v, 1, 100000, &c->max_lines);
      else if (!strcmp(k, "remote")) r = to_int(v, 0, 1, &c->remote);
      else if (!strcmp(k, "surface")) r = *v ? set_str(c->surface, sizeof c->surface, v) : -1;
      else if (!strcmp(k, "source")) r = set_str(c->source, sizeof c->source, v);
      else if (!strcmp(k, "exclude")) r = set_str(c->exclude, sizeof c->exclude, v);
      else if (!strcmp(k, "log_external")) r = to_int(v, 0, 1, &c->log_external);
      else if (!strcmp(k, "tap_ms")) r = to_int(v, 5, 1000, &c->tap_ms);
      else if (!strcmp(k, "gap_ms")) r = to_int(v, 1, 1000, &c->gap_ms);
      else {
        int trig, ch, num;
        if (!parse_trigger(k, &trig, &ch, &num)) {
          if (!own_maps) { c->n_maps = 0; own_maps = 1; }   /* the file's triggers replace the defaults */
          r = add_map(c, trig, ch, num, v);
        }
      }
    }
    if (r < 0) {
      if (!bad && err && errn) snprintf(err, errn, "line %d: '%s'", no, shown);
      bad++;
    }
  }
  fclose(f);
  return bad;
}
