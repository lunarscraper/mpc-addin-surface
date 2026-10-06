/* test_config.c: surface.conf parsing. */
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

  char path[] = "/tmp/mpcsf_conf_XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  const char *txt = "# comment\n enabled = 0 \nlog=\ndevice=hw:0   # the surface\nlog_pressure=1\nmax_lines=50\n";
  CHECK_EQ(write(fd, txt, strlen(txt)), (long long)strlen(txt));
  close(fd);
  CHECK_EQ(mpcsf_cfg_load(&c, path, err, sizeof err), 0);
  CHECK_EQ(c.enabled, 0); CHECK(!strcmp(c.log_path, "")); CHECK(!strcmp(c.device, "hw:0"));
  CHECK_EQ(c.log_pressure, 1); CHECK_EQ(c.max_lines, 50);

  FILE *f = fopen(path, "w");
  fputs("enabled=2\nnonsense\nmax_lines=10\n", f);
  fclose(f);
  CHECK_EQ(mpcsf_cfg_load(&c, path, err, sizeof err), 2);
  CHECK(strstr(err, "line 1") != NULL);
  CHECK_EQ(c.enabled, 1); CHECK_EQ(c.max_lines, 10);
  unlink(path);
  T_DONE("config");
}
