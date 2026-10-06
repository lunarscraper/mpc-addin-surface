/* test_config.c: surface.conf parsing, actions, triggers. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/config.h"
#include "t.h"

int main(void) {
  mpcsf_cfg c;
  char err[160];
  CHECK_EQ(mpcsf_cfg_load(&c, "/nonexistent/surface.conf", err, sizeof err), -1);
  CHECK_EQ(c.enabled, 1); CHECK_EQ(c.max_lines, 200); CHECK(!strcmp(c.log_path, "auto"));
  CHECK_EQ(c.remote, 1); CHECK_EQ(c.n_maps, 3);
  CHECK_EQ(c.maps[0].trig, MPCSF_TRIG_CC); CHECK_EQ(c.maps[0].channel, 16); CHECK_EQ(c.maps[0].number, 102);
  CHECK_EQ(c.maps[0].n_steps, 1); CHECK_EQ(c.maps[0].steps[0].kind, MPCSF_STEP_TAP); CHECK_EQ(c.maps[0].steps[0].note, 0x03);
  CHECK_EQ(c.maps[2].n_steps, 3);
  CHECK_EQ(c.maps[2].steps[0].kind, MPCSF_STEP_DOWN); CHECK_EQ(c.maps[2].steps[0].note, 0x25);
  CHECK_EQ(c.maps[2].steps[1].kind, MPCSF_STEP_TAP); CHECK_EQ(c.maps[2].steps[1].note, 0x60);
  CHECK_EQ(c.maps[2].steps[2].kind, MPCSF_STEP_UP);

  char path[] = "/tmp/mpcsf_conf_XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  /* a stage 1 file (no trigger lines, the old device= key): accepted, default triggers stay */
  const char *old = "# comment\n enabled = 0 \nlog=\ndevice=hw:0   # the surface\nlog_pressure=1\nmax_lines=50\n";
  CHECK_EQ(write(fd, old, strlen(old)), (long long)strlen(old));
  close(fd);
  CHECK_EQ(mpcsf_cfg_load(&c, path, err, sizeof err), 0);
  CHECK_EQ(c.enabled, 0); CHECK(!strcmp(c.log_path, "")); CHECK_EQ(c.log_pressure, 1); CHECK_EQ(c.max_lines, 50);
  CHECK_EQ(c.n_maps, 3);

  /* own triggers replace the defaults */
  FILE *f = fopen(path, "w");
  fputs("source=EC4\nnote 1 60 = mixer\ncc 2 20 = down:25 tap:62 up:25\ncc 3 1 = menu note\n", f);
  fclose(f);
  CHECK_EQ(mpcsf_cfg_load(&c, path, err, sizeof err), 0);
  CHECK(!strcmp(c.source, "EC4")); CHECK_EQ(c.n_maps, 3);
  CHECK_EQ(c.maps[0].trig, MPCSF_TRIG_NOTE); CHECK_EQ(c.maps[0].steps[0].note, 0x0B);
  CHECK_EQ(c.maps[1].n_steps, 3); CHECK_EQ(c.maps[1].steps[1].note, 0x62);
  CHECK_EQ(c.maps[2].n_steps, 2); CHECK_EQ(c.maps[2].steps[1].note, 0x04);

  f = fopen(path, "w");
  fputs("enabled=2\nnonsense\nmax_lines=10\ncc 17 1 = matrix\ncc 1 1 = tap:80\ncc 1 1 = fly\n", f);
  fclose(f);
  CHECK_EQ(mpcsf_cfg_load(&c, path, err, sizeof err), 5);
  CHECK(strstr(err, "line 1") != NULL);
  CHECK_EQ(c.enabled, 1); CHECK_EQ(c.max_lines, 10);
  unlink(path);

  CHECK_EQ(mpcsf_list_match("Force,MPC, Through", "Akai Pro Force"), 1);
  CHECK_EQ(mpcsf_list_match("Force,MPC, Through", "Midi Through"), 1);
  CHECK_EQ(mpcsf_list_match("Force,MPC,Through", "Faderfox EC4"), 0);
  CHECK_EQ(mpcsf_list_match("", "anything"), 0);
  T_DONE("config");
}
