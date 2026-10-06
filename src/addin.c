/* addin.c: LD_PRELOAD entry points of the surface addin (monitor + remote).
 *
 * Remote (remote=1): a second thread listens to external MIDI controllers on an own ALSA sequencer
 * client. A trigger queues button messages; the hooks below hand them to MPC as if the control
 * surface had sent them: snd_rawmidi_read returns them, and so that MPC wakes up for them the
 * surface's poll descriptors get one more fd (a pipe). Real surface data is never changed or
 * dropped, and nothing is inserted in the middle of a message.
 *
 * Monitor:
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
#include <fcntl.h>
#include <poll.h>
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
#include "seq_abi.h"

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
  pthread_once_t once, once_remote;

  /* remote */
  _Atomic(snd_rawmidi_t *) surf;   /* the control surface's input handle */
  _Atomic int surf_nonblock;
  mpcsf_parser surf_parser;        /* MPC's reading thread only: where the real stream stands */
  int pipe_r, pipe_w;              /* wakes MPC's poll (or our own, in a blocking read) */
  pthread_mutex_t ilock;
  uint8_t inj[64];                 /* queued button messages, 3 bytes each */
  uint32_t inj_len;
  _Atomic unsigned long delivered;
} g = {.qlock = PTHREAD_MUTEX_INITIALIZER, .once = PTHREAD_ONCE_INIT, .once_remote = PTHREAD_ONCE_INIT,
       .ilock = PTHREAD_MUTEX_INITIALIZER, .pipe_r = -1, .pipe_w = -1};

/* ---- real functions ------------------------------------------------------------------------- */

static int (*real_open)(snd_rawmidi_t **, snd_rawmidi_t **, const char *, int);
static int (*real_close)(snd_rawmidi_t *);
static ssize_t (*real_read)(snd_rawmidi_t *, void *, size_t);
static int (*real_nonblock)(snd_rawmidi_t *, int);
static int (*real_pd_count)(snd_rawmidi_t *);
static int (*real_pd)(snd_rawmidi_t *, struct pollfd *, unsigned int);
static int (*real_pd_revents)(snd_rawmidi_t *, struct pollfd *, unsigned int, unsigned short *);

static void resolve_reals(void) {
  *(void **)&real_open = dlsym(RTLD_NEXT, "snd_rawmidi_open");
  *(void **)&real_close = dlsym(RTLD_NEXT, "snd_rawmidi_close");
  *(void **)&real_read = dlsym(RTLD_NEXT, "snd_rawmidi_read");
  *(void **)&real_nonblock = dlsym(RTLD_NEXT, "snd_rawmidi_nonblock");
  *(void **)&real_pd_count = dlsym(RTLD_NEXT, "snd_rawmidi_poll_descriptors_count");
  *(void **)&real_pd = dlsym(RTLD_NEXT, "snd_rawmidi_poll_descriptors");
  *(void **)&real_pd_revents = dlsym(RTLD_NEXT, "snd_rawmidi_poll_descriptors_revents");
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
  if (g.cfg.remote && (!real_pd_count || !real_pd || !real_pd_revents || !real_nonblock)) {
    mpcsf_log("libasound poll functions not found; remote off, monitoring only");
    g.cfg.remote = 0;
  }
  if (g.cfg.remote) {
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) != 0) { mpcsf_log("pipe: %s; remote off", strerror(errno)); g.cfg.remote = 0; }
    else { g.pipe_r = p[0]; g.pipe_w = p[1]; }
  }
  mpcsf_parser_init(&g.surf_parser);
  g.active = 1;
  if (g.cfg.remote) {
    mpcsf_log("active: monitor%s + remote, %d trigger(s):", g.cfg.log_surface ? "" : " (off)", g.cfg.n_maps);
    for (int i = 0; i < g.cfg.n_maps; i++)
      mpcsf_log("  %s ch%d %d = %s", g.cfg.maps[i].trig == MPCSF_TRIG_CC ? "cc" : "note", g.cfg.maps[i].channel,
                g.cfg.maps[i].number, g.cfg.maps[i].text);
  } else {
    mpcsf_log("active: monitoring only, nothing is changed");
  }
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

/* ---- remote: injection queue ------------------------------------------------------------------ */

static void sleep_ms(int ms) {
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&t, NULL);
}

static void drain_pipe(void) {
  char b[16];
  while (read(g.pipe_r, b, sizeof b) > 0) {}
}

static int surf_idle(void) { return !g.surf_parser.in_sysex && g.surf_parser.have == 0; }

/* MPC's reading thread: queued button messages into MPC's buffer, whole messages only, and only
 * between two messages of the real stream. Returns the byte count, 0 if there is nothing to hand over. */
static size_t take_inject(void *buf, size_t size) {
  if (size < 3 || !surf_idle()) return 0;
  if (pthread_mutex_trylock(&g.ilock) != 0) return 0;
  size_t n = g.inj_len < size ? g.inj_len : size;
  n -= n % 3;
  if (n) {
    memcpy(buf, g.inj, n);
    memmove(g.inj, g.inj + n, g.inj_len - n);
    g.inj_len -= (uint32_t)n;
    atomic_fetch_add(&g.delivered, 1);
  }
  if (!g.inj_len) drain_pipe();
  pthread_mutex_unlock(&g.ilock);
  return n;
}

static int inject_pending(void) {
  pthread_mutex_lock(&g.ilock);
  int p = g.inj_len != 0;
  pthread_mutex_unlock(&g.ilock);
  return p;
}

/* Remote thread: queue one button message and wait until MPC has taken it. 0, or -1 when MPC did
 * not read it in time (the message is then withdrawn, so a button can never arrive late or hang). */
static int inject_msg(uint8_t note, uint8_t vel) {
  if (!atomic_load(&g.surf)) return -1;
  pthread_mutex_lock(&g.ilock);
  if (g.inj_len + 3 > sizeof g.inj) { pthread_mutex_unlock(&g.ilock); return -1; }
  g.inj[g.inj_len] = 0x90; g.inj[g.inj_len + 1] = note; g.inj[g.inj_len + 2] = vel;
  g.inj_len += 3;
  pthread_mutex_unlock(&g.ilock);
  ssize_t w = write(g.pipe_w, "x", 1);
  (void)w;
  for (int i = 0; i < 150; i++) {
    if (!inject_pending()) return 0;
    sleep_ms(2);
  }
  pthread_mutex_lock(&g.ilock);
  int late = g.inj_len != 0;
  g.inj_len = 0;
  drain_pipe();
  pthread_mutex_unlock(&g.ilock);
  return late ? -1 : 0;
}

static void run_action(const mpcsf_map *m) {
  uint8_t held[MPCSF_MAX_STEPS];
  int n_held = 0, ok = 1;
  for (int i = 0; i < m->n_steps && ok; i++) {
    uint8_t note = m->steps[i].note;
    if (i) sleep_ms(g.cfg.gap_ms);
    switch (m->steps[i].kind) {
      case MPCSF_STEP_DOWN:
        ok = inject_msg(note, 0x7F) == 0;
        if (ok) held[n_held++] = note;
        break;
      case MPCSF_STEP_UP:
        ok = inject_msg(note, 0) == 0;
        for (int k = 0; k < n_held; k++) if (held[k] == note) held[k] = held[--n_held];
        break;
      default:
        ok = inject_msg(note, 0x7F) == 0;
        if (ok) { sleep_ms(g.cfg.tap_ms); if (inject_msg(note, 0) != 0) { ok = 0; held[n_held++] = note; } }
    }
  }
  if (ok) return;
  mpcsf_log("  action '%s' not completed: MPC did not read the button message in time", m->text);
  /* never leave a button held down: release what went down, with a few more tries */
  for (int k = 0; k < n_held; k++)
    for (int t = 0; t < 5 && inject_msg(held[k], 0) != 0; t++) sleep_ms(50);
}

/* ---- remote: sequencer listener thread -------------------------------------------------------- */

static struct {
  int (*open)(snd_seq_t **, const char *, int, int);
  int (*set_client_name)(snd_seq_t *, const char *);
  int (*client_id)(snd_seq_t *);
  int (*create_simple_port)(snd_seq_t *, const char *, unsigned int, unsigned int);
  int (*connect_from)(snd_seq_t *, int, int, int);
  int (*event_input)(snd_seq_t *, mpcsf_seq_event **);
  size_t (*client_info_sizeof)(void);
  void (*client_info_set_client)(snd_seq_client_info_t *, int);
  int (*client_info_get_client)(const snd_seq_client_info_t *);
  const char *(*client_info_get_name)(snd_seq_client_info_t *);
  int (*client_info_get_type)(const snd_seq_client_info_t *);
  int (*query_next_client)(snd_seq_t *, snd_seq_client_info_t *);
  int (*get_any_client_info)(snd_seq_t *, int, snd_seq_client_info_t *);
  size_t (*port_info_sizeof)(void);
  void (*port_info_set_client)(snd_seq_port_info_t *, int);
  void (*port_info_set_port)(snd_seq_port_info_t *, int);
  int (*port_info_get_port)(const snd_seq_port_info_t *);
  unsigned int (*port_info_get_capability)(const snd_seq_port_info_t *);
  int (*query_next_port)(snd_seq_t *, snd_seq_port_info_t *);
} sq;

static int resolve_seq(void) {
  int missing = 0;
#define R(field, name) do { *(void **)&sq.field = dlsym(RTLD_DEFAULT, name); if (!sq.field) missing++; } while (0)
  R(open, "snd_seq_open"); R(set_client_name, "snd_seq_set_client_name"); R(client_id, "snd_seq_client_id");
  R(create_simple_port, "snd_seq_create_simple_port"); R(connect_from, "snd_seq_connect_from");
  R(event_input, "snd_seq_event_input");
  R(client_info_sizeof, "snd_seq_client_info_sizeof"); R(client_info_set_client, "snd_seq_client_info_set_client");
  R(client_info_get_client, "snd_seq_client_info_get_client"); R(client_info_get_name, "snd_seq_client_info_get_name");
  R(client_info_get_type, "snd_seq_client_info_get_type"); R(query_next_client, "snd_seq_query_next_client");
  R(get_any_client_info, "snd_seq_get_any_client_info");
  R(port_info_sizeof, "snd_seq_port_info_sizeof"); R(port_info_set_client, "snd_seq_port_info_set_client");
  R(port_info_set_port, "snd_seq_port_info_set_port"); R(port_info_get_port, "snd_seq_port_info_get_port");
  R(port_info_get_capability, "snd_seq_port_info_get_capability"); R(query_next_port, "snd_seq_query_next_port");
#undef R
  return missing;
}

static char names[256][40];   /* client id -> name, for the log (remote thread only) */

/* Listen to every readable port of one client, if it is a hardware device we may listen to. */
static void consider_client(snd_seq_t *seq, int me, int port, snd_seq_client_info_t *ci, snd_seq_port_info_t *pi) {
  int id = sq.client_info_get_client(ci);
  const char *name = sq.client_info_get_name(ci);
  if (!name) name = "?";
  if (id < 0 || id > 255 || id == me) return;
  snprintf(names[id], sizeof names[id], "%s", name);
  const char *why = NULL;
  if (sq.client_info_get_type(ci) != MPCSF_SEQ_KERNEL_CLIENT) why = "a program, not a device";
  else if (id < 16) why = "system";
  else if (mpcsf_list_match(g.cfg.exclude, name)) why = "in exclude=";
  else if (g.cfg.source[0] && !strstr(name, g.cfg.source)) why = "not source=";
  if (why) { mpcsf_log("  midi device %d '%s': not listening (%s)", id, name, why); return; }
  int n = 0;
  memset(pi, 0, sq.port_info_sizeof());
  sq.port_info_set_client(pi, id);
  sq.port_info_set_port(pi, -1);
  while (sq.query_next_port(seq, pi) >= 0) {
    unsigned cap = sq.port_info_get_capability(pi);
    if ((cap & (MPCSF_SEQ_CAP_READ | MPCSF_SEQ_CAP_SUBS_READ)) != (MPCSF_SEQ_CAP_READ | MPCSF_SEQ_CAP_SUBS_READ)) continue;
    if (sq.connect_from(seq, port, id, sq.port_info_get_port(pi)) >= 0) n++;   /* a second connect just fails */
  }
  mpcsf_log("  midi device %d '%s': listening on %d port(s)", id, name, n);
}

static void handle_event(const mpcsf_seq_event *ev, int *lines, time_t *second) {
  int trig, ch, num, val;
  if (ev->type == MPCSF_SEQ_EV_CONTROLLER) {
    trig = MPCSF_TRIG_CC; ch = ev->data.control.channel + 1; num = (int)ev->data.control.param; val = ev->data.control.value;
  } else if (ev->type == MPCSF_SEQ_EV_NOTEON) {
    trig = MPCSF_TRIG_NOTE; ch = ev->data.note.channel + 1; num = ev->data.note.note; val = ev->data.note.velocity;
  } else return;
  const char *from = names[ev->source.client][0] ? names[ev->source.client] : "?";
  time_t now = time(NULL);
  if (now != *second) { *second = now; *lines = 0; }
  if (g.cfg.log_external && (*lines)++ < 20)
    mpcsf_log("ext '%s'  %s ch%d %d value %d", from, trig == MPCSF_TRIG_CC ? "cc" : "note", ch, num, val);
  int fire = trig == MPCSF_TRIG_CC ? val >= 64 : val > 0;
  if (!fire) return;
  for (int i = 0; i < g.cfg.n_maps; i++) {
    const mpcsf_map *m = &g.cfg.maps[i];
    if (m->trig != trig || m->channel != ch || m->number != num) continue;
    mpcsf_log("trigger %s ch%d %d from '%s' -> %s", trig == MPCSF_TRIG_CC ? "cc" : "note", ch, num, from, m->text);
    run_action(m);
  }
}

static void *remote_main(void *arg) {
  (void)arg;
  int missing = resolve_seq();
  if (missing) { mpcsf_log("remote: %d libasound sequencer functions missing; remote off", missing); return NULL; }
  snd_seq_t *seq = NULL;
  int r = sq.open(&seq, "default", MPCSF_SEQ_OPEN_INPUT, 0);
  if (r < 0 || !seq) { mpcsf_log("remote: snd_seq_open failed: %d; remote off", r); return NULL; }
  sq.set_client_name(seq, "surface-addin");
  /* writable for us only: hidden from other programs' port lists (MPC's MIDI settings included) */
  int port = sq.create_simple_port(seq, "in", MPCSF_SEQ_CAP_WRITE | MPCSF_SEQ_CAP_NO_EXPORT,
                                   MPCSF_SEQ_TYPE_MIDI_GENERIC | MPCSF_SEQ_TYPE_APPLICATION);
  if (port < 0) { mpcsf_log("remote: no sequencer port: %d; remote off", port); return NULL; }
  int me = sq.client_id(seq);
  snd_seq_client_info_t *ci = calloc(1, sq.client_info_sizeof());
  snd_seq_port_info_t *pi = calloc(1, sq.port_info_sizeof());
  if (!ci || !pi) return NULL;
  sq.connect_from(seq, port, MPCSF_SEQ_CLIENT_SYSTEM, MPCSF_SEQ_PORT_ANNOUNCE);   /* devices plugged in later */
  mpcsf_log("remote: listening for triggers (sequencer client %d)", me);
  sq.client_info_set_client(ci, -1);
  while (sq.query_next_client(seq, ci) >= 0) consider_client(seq, me, port, ci, pi);

  int errors = 0, lines = 0;
  time_t second = 0;
  for (;;) {
    mpcsf_seq_event *ev = NULL;
    r = sq.event_input(seq, &ev);
    if (r < 0 || !ev) {
      if (++errors > 100) { mpcsf_log("remote: sequencer input keeps failing (%d); remote off", r); return NULL; }
      sleep_ms(50);
      continue;
    }
    errors = 0;
    if (ev->type == MPCSF_SEQ_EV_PORT_START) {
      int id = ev->data.addr.client;
      sleep_ms(100);   /* let the new device finish registering its ports */
      memset(ci, 0, sq.client_info_sizeof());
      if (sq.get_any_client_info(seq, id, ci) >= 0) consider_client(seq, me, port, ci, pi);
      continue;
    }
    if (ev->source.client == MPCSF_SEQ_CLIENT_SYSTEM) continue;
    handle_event(ev, &lines, &second);
  }
  return NULL;
}

static void start_thread(void *(*fn)(void *), const char *name) {
  sigset_t all, old;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, &old);      /* the thread inherits the mask: MPC's signals stay MPC's */
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);   /* never MPC's realtime priority */
  pthread_attr_setschedpolicy(&a, SCHED_OTHER);
  int r = pthread_create(&t, &a, fn, NULL);
  pthread_attr_destroy(&a);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (r) mpcsf_log("thread %s not started: %s", name, strerror(r));
  else pthread_setname_np(t, name);
}

static void start_logger(void) { start_thread(logger_main, "surface-log"); }
static void start_remote(void) { start_thread(remote_main, "surface-remote"); }

/* ---- hooks ------------------------------------------------------------------------------------ */

static inline int is_surf(snd_rawmidi_t *h) {
  return h && g.cfg.remote && atomic_load_explicit(&g.surf, memory_order_relaxed) == h;
}

EXPORT int snd_rawmidi_open(snd_rawmidi_t **inp, snd_rawmidi_t **outp, const char *name, int mode) {
  if (!real_open) resolve_reals();
  if (!real_open) return -ENOSYS;
  int r = real_open(inp, outp, name, mode);
  if (r < 0 || !g.active) return r;
  const char *nm = name ? name : "?";
  if (outp && *outp && !(inp && *inp)) mpcsf_log("output opened: %s", nm);
  if (!(inp && *inp)) return r;
  int logging = 0;
  for (int i = 0; i < MAX_IN && g.cfg.log_surface; i++) {
    snd_rawmidi_t *none = NULL;
    /* the name is written before the handle is published; the read hook only compares handles */
    if (atomic_load(&g.in[i].h)) continue;
    snprintf(g.in[i].name, sizeof g.in[i].name, "%s", nm);
    atomic_fetch_add(&g.in[i].gen, 1);
    if (!atomic_compare_exchange_strong(&g.in[i].h, &none, *inp)) continue;
    logging = 1;
    pthread_once(&g.once, start_logger);
    break;
  }
  int surface = 0;
  if (g.cfg.remote && strstr(nm, g.cfg.surface)) {
    snd_rawmidi_t *none = NULL;
    atomic_store(&g.surf_nonblock, (mode & MPCSF_RAWMIDI_NONBLOCK) != 0);
    if (atomic_compare_exchange_strong(&g.surf, &none, *inp)) {
      surface = 1;
      mpcsf_parser_init(&g.surf_parser);
      pthread_once(&g.once_remote, start_remote);
    }
  }
  mpcsf_log("input opened: %s%s%s%s", nm, outp && *outp ? " (with output)" : "", logging ? " (logging)" : "",
            surface ? (mode & MPCSF_RAWMIDI_NONBLOCK ? " (control surface, non-blocking)" : " (control surface, blocking)") : "");
  return r;
}

EXPORT int snd_rawmidi_close(snd_rawmidi_t *h) {
  if (!real_close) resolve_reals();
  if (!real_close) return -ENOSYS;
  if (g.active && h) {
    snd_rawmidi_t *want = h;
    if (atomic_compare_exchange_strong(&g.surf, &want, NULL)) mpcsf_log("control surface closed");
    for (int i = 0; i < MAX_IN; i++) {
      want = h;
      if (atomic_compare_exchange_strong(&g.in[i].h, &want, NULL)) mpcsf_log("input closed: %s", g.in[i].name);
    }
  }
  return real_close(h);
}

EXPORT int snd_rawmidi_nonblock(snd_rawmidi_t *h, int nonblock) {
  if (!real_nonblock) resolve_reals();
  if (!real_nonblock) return -ENOSYS;
  int r = real_nonblock(h, nonblock);
  if (r >= 0 && is_surf(h)) atomic_store(&g.surf_nonblock, nonblock != 0);
  return r;
}

/* MPC's poll set for the control surface gets one more descriptor: the pipe that announces queued
 * button messages. A caller that does not use the count from _count is left alone. */
EXPORT int snd_rawmidi_poll_descriptors_count(snd_rawmidi_t *h) {
  if (!real_pd_count) resolve_reals();
  if (!real_pd_count) return -ENOSYS;
  int n = real_pd_count(h);
  return n > 0 && is_surf(h) ? n + 1 : n;
}

EXPORT int snd_rawmidi_poll_descriptors(snd_rawmidi_t *h, struct pollfd *pfds, unsigned int space) {
  if (!real_pd) resolve_reals();
  if (!real_pd) return -ENOSYS;
  if (!is_surf(h) || !pfds) return real_pd(h, pfds, space);
  int want = real_pd_count ? real_pd_count(h) : -1;
  if (want <= 0 || space < (unsigned)want + 1) return real_pd(h, pfds, space);
  int n = real_pd(h, pfds, (unsigned)want);
  if (n != want) return n;
  pfds[n].fd = g.pipe_r; pfds[n].events = POLLIN; pfds[n].revents = 0;
  return n + 1;
}

EXPORT int snd_rawmidi_poll_descriptors_revents(snd_rawmidi_t *h, struct pollfd *pfds, unsigned int nfds,
                                                unsigned short *revents) {
  if (!real_pd_revents) resolve_reals();
  if (!real_pd_revents) return -ENOSYS;
  if (!is_surf(h) || !pfds || nfds < 2 || pfds[nfds - 1].fd != g.pipe_r) return real_pd_revents(h, pfds, nfds, revents);
  int r = real_pd_revents(h, pfds, nfds - 1, revents);
  if (r >= 0 && revents && (pfds[nfds - 1].revents & POLLIN)) {
    if (inject_pending()) { if (surf_idle()) *revents |= POLLIN; }
    else drain_pipe();   /* stale wake-up */
  }
  return r;
}

/* Monitor: a copy of what MPC just received, for the logger thread. */
static void tap(snd_rawmidi_t *h, const void *buf, size_t len) {
  int slot = -1;
  for (int i = 0; i < MAX_IN; i++)
    if (atomic_load_explicit(&g.in[i].h, memory_order_relaxed) == h) { slot = i; break; }
  if (slot < 0) return;
  atomic_fetch_add_explicit(&g.chunks, 1, memory_order_relaxed);
  if (pthread_mutex_trylock(&g.qlock) != 0) {
    atomic_fetch_add_explicit(&g.dropped, 1, memory_order_relaxed);
    return;
  }
  const uint8_t *p = buf;
  size_t left = len;
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
}

EXPORT ssize_t snd_rawmidi_read(snd_rawmidi_t *h, void *buf, size_t size) {
  if (!real_read) return -ENOSYS;
  if (!g.active) return real_read(h, buf, size);
  if (is_surf(h) && buf) {
    for (;;) {
      size_t n = take_inject(buf, size);
      if (n) return (ssize_t)n;
      if (size < 3 || atomic_load_explicit(&g.surf_nonblock, memory_order_relaxed)) break;
      /* blocking handle: wait for the device or for a queued button, whichever comes first */
      struct pollfd pfds[5];
      int k = real_pd(h, pfds, 4);
      if (k <= 0 || k > 4) break;
      pfds[k].fd = g.pipe_r; pfds[k].events = POLLIN; pfds[k].revents = 0;
      int pr = poll(pfds, (nfds_t)k + 1, -1);
      if (pr < 0) { if (errno == EINTR) continue; break; }
      if (!(pfds[k].revents & POLLIN)) break;          /* the device has data */
      if (!inject_pending()) { drain_pipe(); continue; }
      if (!surf_idle()) break;                         /* finish the real message first */
    }
  }
  ssize_t r = real_read(h, buf, size);
  if (r <= 0) return r;
  if (is_surf(h)) {
    mpcsf_msg m;
    for (ssize_t i = 0; i < r; i++) mpcsf_parser_feed(&g.surf_parser, ((const uint8_t *)buf)[i], &m);
  }
  tap(h, buf, (size_t)r);
  return r;
}

/* ---- test-only introspection (x86 host test build) ------------------------------------------ */
#ifdef MPCSF_TEST_HOOKS
EXPORT int mpcsf_test_active(void) { return g.active; }
EXPORT unsigned long mpcsf_test_chunks(void) { return atomic_load(&g.chunks); }
EXPORT unsigned long mpcsf_test_delivered(void) { return atomic_load(&g.delivered); }
#endif
