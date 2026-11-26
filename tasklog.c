/*
tasklog.c : the phase 4 log format, and the execution summary

Two locks, for two unrelated things, and neither is ever held while the
other is taken, so they cannot deadlock against each other:

  log_lock      the terminal. held for exactly as long as one line takes
                to write, so that a line from one client is never split by
                a line from another

  sum_lock      the summary being built up. written at the end of every
                program turn and read once, when it is printed
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "tasklog.h"

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;


/*
Writes one whole line while holding the lock.

The text is built first and the lock taken afterwards, so the lock is held
for the write alone. printf is thread safe for a single call, but a line
here is a prefix and a body, and two calls could be interleaved.
*/
static void emit(const char *fmt, ...) {
  char    line[512];
  va_list ap;
  int     n;

  va_start(ap, fmt);
  n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);

  if (n < 0)
    return;
  if ((size_t) n >= sizeof line)
    n = (int) sizeof line - 1;   // a line longer than the buffer is cut here

  pthread_mutex_lock(&log_lock);
  fwrite(line, 1, (size_t) n, stdout);
  fflush(stdout);                // so a log sent to a file keeps up with events
  pthread_mutex_unlock(&log_lock);
}


void tasklog_server_started(void) {
  emit("------------------------\n| Hello, Server Started |\n"
       "------------------------\n\n");
}


void tasklog_client_connected(const client_t *c) {
  emit("[%d]<<< client connected\n", c->id);
}


void tasklog_client_disconnected(const client_t *c) {
  emit("[%d]<<< client disconnected\n", c->id);
}


void tasklog_command(const client_t *c, const char *line) {
  emit("[%d]>>> %s\n", c->id, line);
}


/*
The four state lines share a shape, so they share a helper. The number is
the task's remaining work, which for a shell task is -1 and never moves.
*/
static void state_line(const task_t *t, const char *state) {
  emit("[%d]--- %s (%d)\n", t->client->id, state, t->remaining);
}

void tasklog_created(const task_t *t) { state_line(t, "created"); }
void tasklog_started(const task_t *t) { state_line(t, "started"); }
void tasklog_running(const task_t *t) { state_line(t, "running"); }
void tasklog_waiting(const task_t *t) { state_line(t, "waiting"); }
void tasklog_ended  (const task_t *t) { state_line(t, "ended");   }


void tasklog_bytes_sent(const task_t *t, size_t bytes) {
  emit("[%d]<<< %zu bytes sent\n", t->client->id, bytes);
}


void tasklog_error(const client_t *c, const char *text, size_t len) {
  // the newline a program puts at the end of its message would break the
  // line in two, so it is left off inside the log
  while (len > 0 && text[len - 1] == '\n')
    len--;

  emit("[%d]!!! %.*s\n", c->id, (int) len, text);
}


void tasklog_blank(void) {
  emit("\n");
}


/*
The execution summary.

Entries are appended as turns end rather than worked out afterwards,
because the only thing that knows a turn ended, and when, is the scheduler
at the moment it ends it.
*/

#define SUMMARY_MAX 4096

static char            summary[SUMMARY_MAX] = "0)";
static size_t          summary_len = 2;
static int             summary_started = 0;
static struct timespec summary_origin;
static pthread_mutex_t sum_lock = PTHREAD_MUTEX_INITIALIZER;


/*
Whole seconds since the clock started, which is what the format shows.

Worked out from the milliseconds and then rounded, rather than by
subtracting the seconds fields. A turn that ran from 2.9 to 5.9 has taken
three seconds, and comparing only the seconds would call it two.
*/
static int elapsed_seconds(void) {
  struct timespec now;
  long ms;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ms = (now.tv_sec - summary_origin.tv_sec) * 1000L
     + (now.tv_nsec - summary_origin.tv_nsec) / 1000000L;

  return (int) ((ms + 500) / 1000);
}


void summary_turn_begin(const task_t *t) {
  if (t->kind != TASK_PROGRAM)
    return;

  pthread_mutex_lock(&sum_lock);

  // the clock starts at the first program turn, so that the first entry is
  // measured from when scheduling began and not from when the server did
  if (!summary_started) {
    clock_gettime(CLOCK_MONOTONIC, &summary_origin);
    summary_started = 1;
  }

  pthread_mutex_unlock(&sum_lock);
}


void summary_turn_end(const task_t *t) {
  char entry[32];
  int  n;

  // a shell task is not scheduled in turns; it runs once, to completion,
  // and does not appear in the summary
  if (t->kind != TASK_PROGRAM)
    return;

  pthread_mutex_lock(&sum_lock);

  if (summary_started) {
    n = snprintf(entry, sizeof entry, "-P%d-(%d)",
                 t->client->id, elapsed_seconds());

    if (n > 0 && summary_len + (size_t) n < sizeof summary) {
      memcpy(summary + summary_len, entry, (size_t) n);
      summary_len += (size_t) n;
      summary[summary_len] = '\0';
    }
  }

  pthread_mutex_unlock(&sum_lock);
}


void summary_print(void) {
  char copy[SUMMARY_MAX];

  pthread_mutex_lock(&sum_lock);

  // nothing ran, so there is nothing worth printing
  if (summary_len <= 2) {
    pthread_mutex_unlock(&sum_lock);
    return;
  }

  memcpy(copy, summary, summary_len + 1);

  // cleared here, so that the next batch of work starts a fresh line with
  // its own clock rather than carrying this one's entries forward
  summary_len = 2;
  summary[2] = '\0';
  summary_started = 0;

  pthread_mutex_unlock(&sum_lock);

  emit("%s\n", copy);
}


int summary_pending(void) {
  int pending;

  pthread_mutex_lock(&sum_lock);
  pending = summary_len > 2;
  pthread_mutex_unlock(&sum_lock);

  return pending;
}