/*
tests/test_tasklog.c : checks the phase 4 log lines and the summary

The wording of these lines is marked, and a format that drifts is not
something you notice by reading the log while you work on something else.
So each line is produced and compared against exactly what the spec's
screenshots show.

The summary is checked by faking the turns: the entries have to appear in
the order the turns ended, name the task by its client, and start from 0).

  gcc -Wall -Wextra -g -I. -pthread -o tests/test_tasklog \
      tests/test_tasklog.c tasklog.c task.c clientlog.c
  ./tests/test_tasklog
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include "tasklog.h"

static int failures = 0;

static void ok(const char *what, int cond) {
  printf("  %-54s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}


/*
The log writes to stdout, so the tests point stdout at a file and put it
back afterwards. freopen cannot put it back, because the terminal it came
from may not be openable by name, so the original is kept with dup.
*/
static int saved_stdout = -1;
static const char *PATH = "tests/.tasklog_out";

static void capture(void) {
  int fd;

  fflush(stdout);
  if (saved_stdout == -1)
    saved_stdout = dup(STDOUT_FILENO);

  fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  dup2(fd, STDOUT_FILENO);
  close(fd);
}

static void uncapture(void) {
  fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
}

static char *slurp(char *buf, size_t size) {
  int     fd = open(PATH, O_RDONLY);
  ssize_t n = fd == -1 ? 0 : read(fd, buf, size - 1);

  if (fd != -1) close(fd);
  buf[n > 0 ? n : 0] = '\0';
  return buf;
}


static void make_client(client_t *c, int id) {
  struct sockaddr_in addr;

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(40000);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  client_init(c, id, &addr);
  c->id = id;          // fixed, so the expected text does not move
}


int main(void) {
  client_t c;
  task_t   t;
  char     buf[8192];

  make_client(&c, 5);

  printf("the lines the spec's screenshots show\n");
  {
    task_init(&t, &c, "./demo 12");

    capture();
    tasklog_client_connected(&c);
    tasklog_command(&c, "./demo 12");
    tasklog_created(&t);
    tasklog_started(&t);
    t.remaining = 9;
    tasklog_waiting(&t);
    tasklog_running(&t);
    t.remaining = 0;
    tasklog_ended(&t);
    tasklog_bytes_sent(&t, 134);
    tasklog_client_disconnected(&c);
    uncapture();

    slurp(buf, sizeof buf);

    ok("client connected", strstr(buf, "[5]<<< client connected\n")    != NULL);
    ok("the command as sent", strstr(buf, "[5]>>> ./demo 12\n")        != NULL);
    ok("created with its estimate", strstr(buf, "[5]--- created (12)\n") != NULL);
    ok("started on its first turn", strstr(buf, "[5]--- started (12)\n") != NULL);
    ok("waiting with what is left", strstr(buf, "[5]--- waiting (9)\n")  != NULL);
    ok("running on a later turn",   strstr(buf, "[5]--- running (9)\n")  != NULL);
    ok("ended at zero",             strstr(buf, "[5]--- ended (0)\n")    != NULL);
    ok("bytes sent",            strstr(buf, "[5]<<< 134 bytes sent\n")   != NULL);
    ok("client disconnected",
       strstr(buf, "[5]<<< client disconnected\n") != NULL);

    task_destroy(&t);
  }

  printf("\na shell task carries -1 everywhere a time would go\n");
  {
    task_t s;

    task_init(&s, &c, "pwd");

    capture();
    tasklog_created(&s);
    tasklog_started(&s);
    tasklog_ended(&s);
    uncapture();

    slurp(buf, sizeof buf);
    ok("created (-1)", strstr(buf, "[5]--- created (-1)\n") != NULL);
    ok("started (-1)", strstr(buf, "[5]--- started (-1)\n") != NULL);
    ok("ended (-1)",   strstr(buf, "[5]--- ended (-1)\n")   != NULL);

    task_destroy(&s);
  }

  printf("\nan error names its client and loses the trailing newline\n");
  {
    const char *msg = "Command not found: nosuchcmd\n";

    capture();
    tasklog_error(&c, msg, strlen(msg));
    uncapture();

    slurp(buf, sizeof buf);
    ok("the error line",
       strcmp(buf, "[5]!!! Command not found: nosuchcmd\n") == 0);
  }

  printf("\nthe summary records one entry per turn, in order\n");
  {
    client_t c6, c7;
    task_t   p6, p7;

    make_client(&c6, 6);
    make_client(&c7, 7);
    task_init(&p6, &c6, "./demo 12");
    task_init(&p7, &c7, "./demo 12");

    summary_turn_begin(&p6);
    summary_turn_end(&p6);
    summary_turn_begin(&p7);
    summary_turn_end(&p7);
    summary_turn_begin(&p6);
    summary_turn_end(&p6);

    // asked before the capture starts, so that this line does not end up
    // in the file the summary is about to be written to
    ok("something is waiting to be printed", summary_pending());

    capture();
    summary_print();
    uncapture();

    slurp(buf, sizeof buf);

    // the clock is real, so the numbers are all 0 here; the order and the
    // naming are what this is checking
    ok("it starts at 0)",        strncmp(buf, "0)", 2) == 0);
    ok("three turns recorded",   strstr(buf, "-P6-(0)-P7-(0)-P6-(0)") != NULL);
    ok("and it is cleared after printing", !summary_pending());

    task_destroy(&p6);
    task_destroy(&p7);
  }

  printf("\na shell task is not scheduled in turns, so it is not in it\n");
  {
    task_t s;

    task_init(&s, &c, "pwd");

    capture();
    summary_turn_begin(&s);
    summary_turn_end(&s);
    uncapture();

    ok("nothing was recorded for it", !summary_pending());
    task_destroy(&s);
  }

  remove(PATH);

  printf("\n%s (%d failure%s)\n",
         failures ? "FAILURES" : "ALL PASS",
         failures, failures == 1 ? "" : "s");
  return failures != 0;
}