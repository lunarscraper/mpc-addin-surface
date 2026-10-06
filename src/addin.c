/* addin.c: LD_PRELOAD entry points of the surface monitor.
 *
 * Inside MPC's process only (checked via /proc/self/exe), this hooks libasound's raw MIDI calls:
 *   snd_rawmidi_open   notes every input MPC opens (the built-in control surface is one of them);
 *   snd_rawmidi_read   after the real read, copies the bytes MPC just received into a small queue;
 *   snd_rawmidi_close  forgets the handle.
 * A logger thread (SCHED_OTHER, all signals blocked) takes the queue, splits it into MIDI messages
 * and writes one log line per message. Nothing is changed, added or held back: MPC sees exactly
 * the bytes the device sent.
 *
 * Read-hook rules: no allocation, no file I/O, no waiting. It takes a mutex with trylock only; when
 * the logger holds it at that instant the chunk is counted as dropped instead of waiting for it.
 * In any other process every hook is a plain pass-through.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "midi.h"

/* Only the hooks (and test helpers) are exported; everything else is built hidden. */
#define EXPORT __attribute__((visibility("default")))

typedef struct snd_rawmidi snd_rawmidi_t;   /* opaque, as in libasound */

#define MAX_IN 8
#define QUEUE_BYTES 8192u
#define CHUNK_MAX 255u

typedef struct {
  _Atomic(snd_rawmidi_t *) h;
  char name[32];
  mpcsf_parser parser;        /* logger thread only */
  _Atomic unsigned gen;       /* bumped when the slot gets a new handle: the logger resets the parser */
  unsigned seen_gen;          /* logger thread only */
} input_slot;

static struct {
  int active;                 /* process is MPC and the addin is enabled */
  mpcsf_cfg cfg;
  input_slot in[MAX_IN];
  pthread_mutex_t qlock;
  uint8_t queue[QUEUE_BYTES]; /* records: slot, length, bytes */
  uint32_t qlen;
  _Atomic unsigned long dropped, chunks;
  pthread_once_t once;
} g = {.qlock = PTHREAD_MUTEX_INITIALIZER, .once = PTHREAD_ONCE_INIT};

/* ---- real functions ------------------------------------------------------------------------- */

static int (*real_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static int (*real_close)(snd_rawmidi_t *);
static ssize_t (*real_read)(snd_rawmidi_t *, void *, size_t);

static void resolve_reals(void) {
  *(void **)&real_open = dlsym(RTLD_NEXT, "snd_rawmidi_open");
  *(void **)&real_close = dlsym(RTLD_NEXT, "snd_rawmidi_close");
  *(void **)&real_read = dlsym(RTLD_NEXT, "snd_rawmidi_read");
}

/* ---- activation ------------------------------------------------------------------------------ */

static int exe_is_mpc(void) {
  char p[256];
  ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
  if (n <= 0) return 0;
  p[n] = 0;
  const char *b = strrchr(p, '/');
  return strcmp(b ? b + 1 : p, "MPC") == 0;
}

/* The folder this .so was loaded from (the installer puts its settings and log there), "" if unknown. */
static void addin_dir(char *out, size_t n) {
  out[0] = 0;
  FILE *f = fopen("/proc/self/maps", "re");
  if (!f) return;
  unsigned long me = (unsigned long)(uintptr_t)&addin_dir;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    unsigned long a, b;
    char *path = strchr(line, '/');
    if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b) continue;
    path[strcspn(path, "\n")] = 0;
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    if (strlen(path) < n) memcpy(out, path, strlen(path) + 1);   /* too long: unknown, never a truncated path */
    break;
  }
  fclose(f);
}

__attribute__((constructor)) static void mpcsf_ctor(void) {
  resolve_reals();
  if (!exe_is_mpc()) return;

  char dir[128], defpath[160];   /* dir + "/surface.log" fits log_path */
  addin_dir(dir, sizeof dir);
  snprintf(defpath, sizeof defpath, "%s/%s", dir[0] ? dir : ".", MPCSF_CONF_NAME);
  const char *path = getenv("MPC_SURFACE_CONF");
  if (!path || !*path) path = defpath;
  char err[160] = "";
  int bad = mpcsf_cfg_load(&g.cfg, path, err, sizeof err);
  if (!strcmp(g.cfg.log_path, "auto"))
    snprintf(g.cfg.log_path, sizeof g.cfg.log_path, "%s/surface.log", dir[0] ? dir : ".");
  mpcsf_log_open(g.cfg.log_path);
  mpcsf_log("---- MPC started (pid %d) ----", (int)getpid());
  if (bad < 0) mpcsf_log("config %s unreadable, using defaults", path);
  else if (bad) mpcsf_log("config %s: %d problem(s), first: %s", path, bad, err);
  else mpcsf_log("config %s", path);
  if (!g.cfg.enabled) { mpcsf_log("disabled in config"); return; }
  if (!real_open || !real_read || !real_close) {
    mpcsf_log("libasound raw MIDI functions not found (open %d, read %d, close %d); staying idle",
              !!real_open, !!real_read, !!real_close);
    return;
  }
  g.active = 1;
  mpcsf_log("active: monitoring only, nothing is changed. Waiting for MPC to open its raw MIDI inputs "
            "(if no 'input opened' line follows, MPC does not read them through libasound)");
}

/* ---- logger thread ---------------------------------------------------------------------------- */

static void log_chunk(input_slot *s, const uint8_t *p, unsigned n, int *lines, unsigned long *muted) {
  unsigned gen = atomic_load(&s->gen);
  if (gen != s->seen_gen) { mpcsf_parser_init(&s->parser); s->seen_gen = gen; }
  mpcsf_msg m;
  char text[256];
  for (unsigned i = 0; i < n; i++) {
    if (!mpcsf_parser_feed(&s->parser, p[i], &m)) continue;
    if (m.bytes[0] == 0xF8 || m.bytes[0] == 0xFE) continue;        /* clock, active sensing */
    if (!g.cfg.log_pressure && mpcsf_msg_is_pressure(&m)) continue;
    if (*lines >= g.cfg.max_lines) { (*muted)++; continue; }
    (*lines)++;
    mpcsf_log("in %-10s %s", s->name, mpcsf_msg_describe(&m, text, sizeof text));
  }
}

static void *logger_main(void *arg) {
  (void)arg;
  static uint8_t local[QUEUE_BYTES];
  int lines = 0;
  unsigned long muted = 0, last_dropped = 0;
  time_t second = time(NULL);
  for (;;) {
    struct timespec nap = {0, 10 * 1000 * 1000};
    nanosleep(&nap, NULL);
    pthread_mutex_lock(&g.qlock);
    uint32_t n = g.qlen;
    memcpy(local, g.queue, n);
    g.qlen = 0;
    pthread_mutex_unlock(&g.qlock);

    for (uint32_t o = 0; o + 2 <= n;) {
      unsigned slot = local[o], len = local[o + 1];
      o += 2;
      if (o + len > n || slot >= MAX_IN) break;
      log_chunk(&g.in[slot], local + o, len, &lines, &muted);
      o += len;
    }
    time_t now = time(NULL);
    if (now != second) {
      second = now;
      if (muted) mpcsf_log("... %lu more message(s) in that second not logged (max_lines=%d)", muted, g.cfg.max_lines);
      unsigned long d = atomic_load(&g.dropped);
      if (d != last_dropped) { mpcsf_log("... %lu chunk(s) not logged (queue busy or full)", d - last_dropped); last_dropped = d; }
      lines = 0; muted = 0;
    }
  }
  return NULL;
}

static void start_logger(void) {
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, &old);      /* the thread inherits the mask: MPC's signals stay MPC's */
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);   /* never MPC's realtime priority */
  pthread_attr_setschedpolicy(&a, SCHED_OTHER);
  int r = pthread_create(&t, &a, logger_main, NULL);
  pthread_attr_destroy(&a);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (r) mpcsf_log("logger thread not started: %s", strerror(r));
  else pthread_setname_np(t, "surface-log");
}

/* ---- hooks ------------------------------------------------------------------------------------ */

EXPORT int snd_rawmidi_open(snd_rawmidi_t **inp, snd_rawmidi_t **outp, const char *name, int mode) {
  if (!real_open) resolve_reals();
  if (!real_open) return -ENOSYS;
  int r = real_open(inp, outp, name, mode);
  if (r < 0 || !g.active) return r;
  const char *nm = name ? name : "?";
  if (outp && *outp && !(inp && *inp)) mpcsf_log("output opened: %s", nm);
  if (!(inp && *inp)) return r;
  if (g.cfg.device[0] && !strstr(nm, g.cfg.device)) {
    mpcsf_log("input opened: %s (not logged: device=%s)", nm, g.cfg.device);
    return r;
  }
  for (int i = 0; i < MAX_IN; i++) {
    snd_rawmidi_t *none = NULL;
    /* the name is written before the handle is published; the read hook only compares handles */
    if (atomic_load(&g.in[i].h)) continue;
    snprintf(g.in[i].name, sizeof g.in[i].name, "%s", nm);
    atomic_fetch_add(&g.in[i].gen, 1);
    if (!atomic_compare_exchange_strong(&g.in[i].h, &none, *inp)) continue;
    mpcsf_log("input opened: %s%s (logging)", nm, outp && *outp ? " (with output)" : "");
    pthread_once(&g.once, start_logger);
    return r;
  }
  mpcsf_log("input opened: %s (not logged: more than %d inputs)", nm, MAX_IN);
  return r;
}

EXPORT int snd_rawmidi_close(snd_rawmidi_t *h) {
  if (!real_close) resolve_reals();
  if (!real_close) return -ENOSYS;
  if (g.active && h)
    for (int i = 0; i < MAX_IN; i++) {
      snd_rawmidi_t *want = h;
      if (atomic_compare_exchange_strong(&g.in[i].h, &want, NULL)) mpcsf_log("input closed: %s", g.in[i].name);
    }
  return real_close(h);
}

EXPORT ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buf, size_t size) {
  if (!real_read) return -ENOSYS;
  ssize_t r = real_read(h, buf, size);
  if (r <= 0 || !g.active) return r;
  int slot = -1;
  for (int i = 0; i < MAX_IN; i++)
    if (atomic_load_explicit(&g.in[i].h, memory_order_relaxed) == h) { slot = i; break; }
  if (slot < 0) return r;
  atomic_fetch_add_explicit(&g.chunks, 1, memory_order_relaxed);
  if (pthread_mutex_trylock(&g.qlock) != 0) {
    atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
    return r;
  }
  const uint8_t *p = buf;
  size_t left = (size_t)r;
  while (left) {
    unsigned n = left > CHUNK_MAX ? CHUNK_MAX : (unsigned)left;
    if (g.qlen + 2 + n > QUEUE_BYTES) { atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed); break; }
    g.queue[g.qlen] = (uint8_t)slot;
    g.queue[g.qlen + 1] = (uint8_t)n;
    memcpy(g.queue + g.qlen + 2, p, n);
    g.qlen += 2 + n;
    p += n; left -= n;
  }
  pthread_mutex_unlock(&g.qlock);
  return r;
}

/* ---- test-only introspection (x86 host test build) ------------------------------------------ */
#ifdef MPCSF_TEST_HOOKS
EXPORT int mpcsf_test_active(void) { return g.active; }
EXPORT unsigned long mpcsf_test_chunks(void) { return atomic_load(&g.chunks); }
#endif
