#include "foundation/progress.h"

#include "foundation/format.h"

#include "hostmem/duration.h"
#include "hostmem/mono_timer.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
  /** How wide the bar is drawn, in cells. */
  PROGRESS_BAR_CELLS = 22,
  /** How wide the moving block of an open walk is. */
  PROGRESS_PULSE_CELLS = 4,
  /** Work that settles faster than this never draws a bar at all. */
  PROGRESS_QUIET_MILLIS = 900,
  /** How often the line is redrawn while the work runs. */
  PROGRESS_DRAW_MILLIS = 200,
  /** How often a line is written when no terminal is watching. */
  PROGRESS_LOG_SECONDS = 10,
  /** Longest step name kept; anything longer is cut off. */
  PROGRESS_NAME_SIZE = 200
};

/** One cell of a bar that has been walked through. */
#define PROGRESS_FULL "█"
/** One cell still ahead. */
#define PROGRESS_EMPTY "░"

/**
 * The one walk that is open.  A build is a sequence of steps, never two at
 * once, so the state is the module's rather than the caller's — and the caller
 * says what happens next instead of carrying a handle from step to step.
 */
static struct {
  char what[PROGRESS_NAME_SIZE]; /**< The step, as it was announced. */
  uint64_t total;                /**< What the whole is, or 0 when unknown. */
  _Atomic uint64_t current;      /**< What has been done, as last reported. */
  ProgressPoll poll;             /**< Fetches the count when nobody reports it. */
  void *poll_user;
  hostmem_mono_timer start;
  pthread_t ticker;
  pthread_mutex_t mutex;
  pthread_cond_t wake;
  bool running;   /**< A step is open. */
  bool ticking;   /**< …and a thread of its own is drawing it. */
  bool stop;      /**< Asked to end; guarded by the mutex. */
  bool tty;       /**< Someone is watching a terminal, so the line may move. */
  bool line_open; /**< The announcement still waits for its end. */
  size_t drawn;   /**< Cells standing on the line, so they can be wiped. */
} g = {.mutex = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};

/* =========================================================================
 *  Putting a line down and taking it back
 * ========================================================================= */

/** Cells a UTF-8 line occupies — every byte but a continuation starts one. */
static size_t cells_of(const char *text) {
  size_t cells = 0;
  for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
    if ((*p & 0xC0) != 0x80) ++cells;
  }
  return cells;
}

/**
 * @brief Write one line over the one before it.
 *
 *  Whatever the last line left behind is covered with spaces rather than cut
 *  away with an escape sequence — a terminal that speaks no ANSI still ends up
 *  with a clean line, and there is nothing to get wrong on Windows.
 */
static void put_line(const char *line) {
  size_t cells = cells_of(line);
  fputc('\r', stdout);
  fputs(line, stdout);
  for (size_t i = cells; i < g.drawn; ++i) { fputc(' ', stdout); }
  if (cells < g.drawn) { fputc('\r', stdout); }
  g.drawn = cells;
  fflush(stdout);
}

/**
 * @brief Give the bar a line of its own, below the announcement.
 *
 *  The announcement is left unfinished on purpose: a step that settles before
 *  the first bar would appear simply writes its duration behind its own name,
 *  and two lines are spent only where there was something to watch.
 */
static void open_the_line(void) {
  if (!g.line_open) return;
  fputc('\n', stdout);
  g.line_open = false;
}

/** Take the bar off the screen; the line below it is the caller's again. */
static void wipe_line(void) {
  if (!g.drawn) return;
  put_line("");
  fputc('\r', stdout);
  g.drawn = 0;
}

/* =========================================================================
 *  Composing what stands on the line
 * ========================================================================= */

/** Append text, never past the end, and report where the writing now stands. */
static size_t put(char *out, size_t size, size_t at, const char *text) {
  size_t length = strlen(text);
  if (at + length + 1 > size) length = at + 1 < size ? size - at - 1 : 0;
  memcpy(out + at, text, length);
  out[at + length] = '\0';
  return at + length;
}

/** The walked-through part of a bar whose end is known. */
static size_t compose_measured_bar(char *out, size_t size, size_t at, double fraction) {
  if (!(fraction > 0.0)) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  size_t filled = (size_t)(fraction * (double)PROGRESS_BAR_CELLS + 0.5);

  at = put(out, size, at, "[");
  for (size_t i = 0; i < PROGRESS_BAR_CELLS; ++i) {
    at = put(out, size, at, i < filled ? PROGRESS_FULL : PROGRESS_EMPTY);
  }
  return put(out, size, at, "]");
}

/**
 * @brief A block travelling back and forth, for work whose end is not known.
 *
 *  It says nothing about how far along the work is — nothing can — but it says
 *  the one thing that matters while a merge holds the program: it is running.
 */
static size_t compose_pulse_bar(char *out, size_t size, size_t at, double elapsed) {
  const size_t span = PROGRESS_BAR_CELLS - PROGRESS_PULSE_CELLS;
  size_t step = (size_t)(elapsed * 8.0) % (2 * span);
  size_t head = step < span ? step : 2 * span - step;

  at = put(out, size, at, "[");
  for (size_t i = 0; i < PROGRESS_BAR_CELLS; ++i) {
    bool lit = i >= head && i < head + PROGRESS_PULSE_CELLS;
    at = put(out, size, at, lit ? PROGRESS_FULL : PROGRESS_EMPTY);
  }
  return put(out, size, at, "]");
}

/** "105.20 MB/s", or nothing at all before there is anything to divide. */
static size_t compose_rate(char *out, size_t size, size_t at, uint64_t bytes, double elapsed) {
  if (!bytes || !(elapsed > 0.0)) return at;
  char rate[32];
  format_byte_units(rate, sizeof(rate), (uint64_t)((double)bytes / elapsed), 2);
  at = put(out, size, at, "   ");
  at = put(out, size, at, rate);
  return put(out, size, at, "/s");
}

/** How long the rest will take at the pace so far, if that can be said yet. */
static size_t compose_eta(
    char *out, size_t size, size_t at, uint64_t current, uint64_t total, double elapsed
) {
  if (!total || current >= total || !current || !(elapsed > 0.0)) return at;
  double remaining = (double)(total - current) / ((double)current / elapsed);
  /* the last second counts itself down faster than it can be read, and
     "ETA 27.5 ms" is a number nobody ever needed */
  if (remaining < 1.0) return at;
  char eta[32];
  hostmem_duration_string(eta, sizeof(eta), (hostmem_duration)(remaining * 1e9), 1);
  at = put(out, size, at, "   ETA ");
  return put(out, size, at, eta);
}

/** The whole running line: bar first, then what it stands for. */
static void compose_running(char *out, size_t size, double elapsed, uint64_t current) {
  char amount[32], whole[32], percent[16];
  size_t at = put(out, size, 0, "  ");

  if (g.total) {
    double fraction = (double)current / (double)g.total;
    at = compose_measured_bar(out, size, at, fraction);
    snprintf(percent, sizeof(percent), "  %5.1f %%", 100.0 * fraction);
    at = put(out, size, at, percent);
    format_byte_units(amount, sizeof(amount), current, 2);
    format_byte_units(whole, sizeof(whole), g.total, 2);
    at = put(out, size, at, "   ");
    at = put(out, size, at, amount);
    at = put(out, size, at, " / ");
    at = put(out, size, at, whole);
  } else {
    at = compose_pulse_bar(out, size, at, elapsed);
    if (current) {
      format_byte_units(amount, sizeof(amount), current, 2);
      at = put(out, size, at, "   ");
      at = put(out, size, at, amount);
    }
  }

  at = compose_rate(out, size, at, current, elapsed);
  at = compose_eta(out, size, at, current, g.total, elapsed);

  char passed[32];
  hostmem_duration_string(passed, sizeof(passed), (hostmem_duration)(elapsed * 1e9), 1);
  at = put(out, size, at, "   ");
  put(out, size, at, passed);
}

/* =========================================================================
 *  The thread that keeps the line alive
 * ========================================================================= */

/** Sleep for a while, or wake early because the step has ended. */
static bool wait_for_end(unsigned millis) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_nsec += (long)millis * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
  }

  pthread_mutex_lock(&g.mutex);
  while (!g.stop) {
    if (pthread_cond_timedwait(&g.wake, &g.mutex, &deadline) == ETIMEDOUT) break;
  }
  bool stop = g.stop;
  pthread_mutex_unlock(&g.mutex);
  return stop;
}

static uint64_t read_current(void) {
  return g.poll ? g.poll(g.poll_user) : atomic_load(&g.current);
}

/**
 * @brief Draw the open step until it closes.
 *
 *  A thread of its own, because the work it reports on does not come back to
 *  ask: a merge runs for a minute without a single call, and a line that only
 *  moved when the caller remembered to move it would stand still exactly when
 *  the operator most wants to see that it does not.
 *
 *  Where no terminal is watching — a log, a pipe — nothing is overwritten;
 *  a line is simply written now and then, which is what a log wants.
 */
static void *ticker_run(void *unused) {
  (void)unused;
  double logged = 0.0;
  char line[512];

  while (!wait_for_end(PROGRESS_DRAW_MILLIS)) {
    double elapsed = hostmem_mono_timer_seconds(g.start);
    if (elapsed * 1000.0 < PROGRESS_QUIET_MILLIS) continue;

    uint64_t current = read_current();
    if (g.tty) {
      compose_running(line, sizeof(line), elapsed, current);
      open_the_line();
      put_line(line);
    } else if (elapsed - logged >= (double)PROGRESS_LOG_SECONDS) {
      logged = elapsed;
      compose_running(line, sizeof(line), elapsed, current);
      open_the_line();
      printf("%s\n", line);
      fflush(stdout);
    }
  }
  return NULL;
}

/* =========================================================================
 *  What the caller says
 * ========================================================================= */

void progress_begin_polled(
    const char *what, uint64_t total_bytes, ProgressPoll poll, void *user_data
) {
  if (g.running) progress_end(); /* a step left open would take the next one's time */

  snprintf(g.what, sizeof(g.what), "%s", what ? what : "");
  g.total = total_bytes;
  atomic_store(&g.current, 0);
  g.poll = poll;
  g.poll_user = user_data;
  g.drawn = 0;
  g.stop = false;
  g.tty = isatty(fileno(stdout)) != 0;
  g.running = true;
  g.line_open = true;
  hostmem_mono_timer_reset(&g.start);

  /* said before the work starts, not after: the seconds before the first bar
     appears are exactly the ones in which a silent screen looks like a hang */
  printf("→ %s", g.what);
  fflush(stdout);

  g.ticking = pthread_create(&g.ticker, NULL, ticker_run, NULL) == 0;
}

void progress_begin(const char *what, uint64_t total_bytes) {
  progress_begin_polled(what, total_bytes, NULL, NULL);
}

void progress_update(uint64_t currentBytes) {
  atomic_store(&g.current, currentBytes);
}

void progress_end(void) {
  if (!g.running) return;

  if (g.ticking) {
    pthread_mutex_lock(&g.mutex);
    g.stop = true;
    pthread_cond_broadcast(&g.wake);
    pthread_mutex_unlock(&g.mutex);
    pthread_join(g.ticker, NULL);
    g.ticking = false;
  }
  wipe_line();

  double elapsed = hostmem_mono_timer_seconds(g.start);
  uint64_t current = read_current();
  if (g.total && current < g.total) current = g.total; /* the step is through */

  char passed[32], line[256];
  hostmem_duration_string(passed, sizeof(passed), (hostmem_duration)(elapsed * 1e9), 2);
  /* a step nobody had to watch keeps its duration on its own line */
  size_t at = put(line, sizeof(line), 0, g.line_open ? " — done in " : "  done in ");
  at = put(line, sizeof(line), at, passed);
  if (current) {
    char amount[32];
    format_byte_units(amount, sizeof(amount), current, 2);
    at = put(line, sizeof(line), at, ", ");
    at = put(line, sizeof(line), at, amount);
    if (elapsed > 0.0) {
      char rate[32];
      format_byte_units(rate, sizeof(rate), (uint64_t)((double)current / elapsed), 2);
      at = put(line, sizeof(line), at, " at ");
      at = put(line, sizeof(line), at, rate);
      put(line, sizeof(line), at, "/s");
    }
  }
  printf("%s\n", line);
  fflush(stdout);

  g.line_open = false;
  g.running = false;
  g.poll = NULL;
  g.poll_user = NULL;
  g.total = 0;
}
