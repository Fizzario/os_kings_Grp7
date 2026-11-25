/*
tests/test_task.c : checks a line is turned into the right kind of task

What matters here is the estimate, because the scheduler picks the task with
the shortest remaining time and an estimate that is wrong by a lot would make
every later decision wrong with it. The cases are the ones a client can
actually type: an ordinary command, a pipeline, ./demo with a number, ./demo
with something that is not a number, and a program the server knows nothing
about.

  gcc -Wall -Wextra -g -I. -pthread -o tests/test_task \
      tests/test_task.c task.c clientlog.c
  ./tests/test_task
*/

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "task.h"

static int failures = 0;

static void ok(const char *what, int cond) {
  printf("  %-54s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}


/*
Classifies a line and checks both answers at once, since a kind without the
estimate that goes with it says very little.
*/
static void expect(const char *line, task_kind kind, int burst) {
  char label[80];
  int  got_burst = 0;
  task_kind got_kind = task_classify(line, &got_burst);

  snprintf(label, sizeof label, "%-28s -> %s (%d)", line,
           kind == TASK_PROGRAM ? "program" : "shell", burst);
  ok(label, got_kind == kind && got_burst == burst);
}


int main(void) {
  printf("ordinary commands are shell tasks with no estimate\n");
  expect("ls -l",               TASK_SHELL, -1);
  expect("pwd",                 TASK_SHELL, -1);
  expect("  echo hello  ",      TASK_SHELL, -1);
  expect("mkdir a",             TASK_SHELL, -1);

  printf("\na pipeline is never preempted, whatever is in it\n");
  expect("ls | wc -l",          TASK_SHELL, -1);
  expect("./demo 5 | cat",      TASK_SHELL, -1);

  printf("\nquoted pipes are text, not pipelines\n");
  expect("echo \"a|b\"",        TASK_SHELL, -1);
  expect("echo 'x|y'",          TASK_SHELL, -1);

  printf("\ndemo states its own running time\n");
  expect("./demo 12",           TASK_PROGRAM, 12);
  expect("./demo 8",            TASK_PROGRAM, 8);
  expect("./demo 0",            TASK_PROGRAM, 0);
  expect("  ./demo   3  ",      TASK_PROGRAM, 3);

  printf("\ndemo without a usable number falls back on the default\n");
  expect("./demo",              TASK_PROGRAM, DEFAULT_BURST);
  expect("./demo abc",          TASK_PROGRAM, DEFAULT_BURST);
  expect("./demo 7x",           TASK_PROGRAM, DEFAULT_BURST);
  expect("./demo -4",           TASK_PROGRAM, DEFAULT_BURST);

  printf("\nany other program is a program task with the default\n");
  expect("./a.out",             TASK_PROGRAM, DEFAULT_BURST);
  expect("./myshell",           TASK_PROGRAM, DEFAULT_BURST);

  printf("\na task carries its own copy of the line\n");
  {
    client_t c;
    task_t   t;
    char     line[MAX_LINE];
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(40000);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    client_init(&c, 7, &addr);

    strcpy(line, "./demo 9");
    ok("task_init succeeds", task_init(&t, &c, line) == 0);

    // the buffer a command arrives in is reused by the next one, so a task
    // that only pointed at it would be holding someone else's command
    strcpy(line, "rm -rf /");
    ok("the line survives its buffer being reused",
       strcmp(t.line, "./demo 9") == 0);

    ok("the estimate is taken from the command", t.burst == 9);
    ok("and nothing has been done yet",          t.remaining == 9);
    ok("it belongs to its client",               t.client == &c);
    ok("it has had no turns",                    task_is_first_turn(&t));
    ok("it is waiting, not running",             t.state == TASK_READY);
    ok("with no child of its own",               t.pid == -1);

    task_destroy(&t);
  }

  printf("\ntask numbers are handed out in order and never repeat\n");
  {
    client_t c;
    task_t   a, b;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    client_init(&c, 8, &addr);

    task_init(&a, &c, "ls");
    task_init(&b, &c, "pwd");

    ok("the second task gets a later number", b.id > a.id);

    task_destroy(&a);
    task_destroy(&b);
  }

  printf("\n%s (%d failure%s)\n",
         failures ? "FAILURES" : "ALL PASS",
         failures, failures == 1 ? "" : "s");
  return failures != 0;
}