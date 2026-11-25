/*
task.c : building a task out of a line, and working out how long it will take

The whole of the decision "what is this request, and how long should the
scheduler expect it to run" lives here, so that the queue, the log and the
scheduler never have to look at the text of a command again.
*/

#include <stdlib.h>    // strtol()
#include <string.h>    // strncpy(), strlen()
#include <ctype.h>     // isspace(), isdigit()
#include <pthread.h>   // pthread_mutex_t, pthread_mutex_lock()
#include "task.h"

// the number the next task gets, and the lock that hands it out
static int next_id = 1;
static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER;


/*
Steps over spaces and tabs, returning where the next word starts, or the end
of the string if there is nothing left.
*/
static const char *skip_spaces(const char *p) {
  while (*p != '\0' && isspace((unsigned char) *p))
    p++;
  return p;
}


/*
Returns the length of the word beginning at p: everything up to the next
space, or to the end of the string.
*/
static size_t word_length(const char *p) {
  size_t n = 0;

  while (p[n] != '\0' && !isspace((unsigned char) p[n]))
    n++;
  return n;
}


/*
True if the line contains a pipe that is not inside quotes.

A pipeline is run by the shell as one unit and there is no sensible point
part way through it at which to stop, so anything containing a pipe is a
shell task however it begins. The quotes are respected because echo "a|b"
prints a pipe character rather than joining two commands, exactly as the
parser treats it.
*/
static int has_pipe(const char *p) {
  char quote = '\0';

  for (; *p != '\0'; p++) {
    if (quote != '\0') {
      // inside quotes, the only character with any meaning is the matching
      // quote that closes them
      if (*p == quote)
        quote = '\0';
    }
    else if (*p == '"' || *p == '\'') {
      quote = *p;
    }
    else if (*p == '|') {
      return 1;
    }
  }

  return 0;
}


/*
Reads the argument of ./demo as a number of seconds.

Returns the value, or -1 if the word is not a plain number or is outside what
demo itself will accept. Anything refused here is not an error: the task is
still a program task, it simply has no estimate to take from the command and
falls back on the default.
*/
static int read_demo_seconds(const char *word, size_t len) {
  char   text[16];
  char  *end;
  long   value;

  if (len == 0 || len >= sizeof text)
    return -1;

  memcpy(text, word, len);
  text[len] = '\0';

  value = strtol(text, &end, 10);

  // end is left at the first character that was not a digit, so a word with
  // anything else in it at all is refused here
  if (*end != '\0' || value < 0 || value > 3600)
    return -1;

  return (int) value;
}


task_kind task_classify(const char *line, int *burst) {
  const char *first, *second;
  size_t      first_len;
  int         seconds;

  // a pipeline is never preempted, whatever the commands in it are
  if (has_pipe(line)) {
    *burst = -1;
    return TASK_SHELL;
  }

  first = skip_spaces(line);
  first_len = word_length(first);

  /*
  A program task is one the client runs by naming a path, which in this
  project means a program sitting beside the server. Anything else is a
  command found on the PATH and is treated as an ordinary shell command,
  since there is no way to guess how long one of those will take.
  */
  if (first_len < 3 || first[0] != '.' || first[1] != '/') {
    *burst = -1;
    return TASK_SHELL;
  }

  // ./demo is the one program whose running time the command itself states
  if (first_len == 6 && strncmp(first, "./demo", 6) == 0) {
    second = skip_spaces(first + first_len);
    seconds = read_demo_seconds(second, word_length(second));

    if (seconds >= 0) {
      *burst = seconds;
      return TASK_PROGRAM;
    }
  }

  // a program, but nothing in the command says how long it will run for
  *burst = DEFAULT_BURST;
  return TASK_PROGRAM;
}


int task_init(task_t *t, client_t *c, const char *line) {
  int burst;

  t->kind = task_classify(line, &burst);

  t->client    = c;
  t->burst     = burst;
  t->remaining = burst;   // -1 for a shell task, which never counts down
  t->turns     = 0;
  t->state     = TASK_READY;
  t->pid       = -1;
  t->bytes_sent = 0;

  // the caller has already refused anything at or over MAX_LINE, so this
  // only guards against the line arriving from somewhere that had not
  strncpy(t->line, line, sizeof t->line - 1);
  t->line[sizeof t->line - 1] = '\0';

  // 0 as the second argument shares the semaphore between the threads of
  // this process, which is all that needs to see it
  if (sem_init(&t->done, 0, 0) != 0)
    return -1;

  // two clients can send a command at the same moment on different threads,
  // so taking a number has to be one indivisible step
  pthread_mutex_lock(&id_lock);
  t->id = next_id++;
  pthread_mutex_unlock(&id_lock);

  return 0;
}


void task_destroy(task_t *t) {
  sem_destroy(&t->done);
}


int task_is_first_turn(const task_t *t) {
  return t->turns == 0;
}