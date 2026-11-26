/*
tests/test_runner.c : checks a task can be run in turns and resumed

The thing being tested is the one the whole phase rests on: that a program
given three seconds stops after three seconds with the rest of its work
intact, and that its next turn carries on from there instead of starting
again. If that is wrong, no scheduling policy on top of it can be right.

There is no server here. A socketpair stands in for the client, so the
output the runner forwards can be read back and counted.

  gcc -Wall -Wextra -g -I. -pthread -o tests/test_runner \
      tests/test_runner.c runner.c tasklog.c task.c clientlog.c protocol.c \
      parser.c executor.c
  ./tests/test_runner
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include "runner.h"
#include "protocol.h"

static int failures = 0;

static void ok(const char *what, int cond) {
  printf("  %-54s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}


// the log would bury the results, so it goes to /dev/null for the run
static void quiet(void) {
  int fd = open("/dev/null", O_WRONLY);

  fflush(stdout);
  if (fd != -1) { dup2(fd, STDOUT_FILENO); close(fd); }
}

static int saved = -1;

static void loud(void) {
  fflush(stdout);
  if (saved != -1) dup2(saved, STDOUT_FILENO);
}


/*
Reads every message waiting on the test's end of the socketpair and joins
them into one buffer, so the output of a task can be looked at as text.
*/
static size_t collect(int fd, char *buf, size_t size) {
  size_t total = 0, len;
  char   msg[4096];
  int    flags = fcntl(fd, F_GETFL, 0);

  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  while (recv_msg(fd, msg, sizeof msg, &len) == 1 && len > 0) {
    if (total + len >= size) break;
    memcpy(buf + total, msg, len);
    total += len;
  }

  fcntl(fd, F_SETFL, flags);
  buf[total] = '\0';
  return total;
}


static int count_lines(const char *s) {
  int n = 0;

  for (; *s; s++)
    if (*s == '\n') n++;
  return n;
}


// true while the process is stopped, as the kernel reports it
static int is_stopped(pid_t pid) {
  char  path[64], buf[512], *p;
  int   fd;
  ssize_t n;

  snprintf(path, sizeof path, "/proc/%d/stat", (int) pid);
  fd = open(path, O_RDONLY);
  if (fd == -1) return 0;

  n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = '\0';

  // the state letter is the field after the command name in brackets
  p = strrchr(buf, ')');
  return p != NULL && p[1] == ' ' && p[2] == 'T';
}


int main(void) {
  client_t c;
  int      sv[2];
  struct sockaddr_in addr;

  saved = dup(STDOUT_FILENO);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  // a socketpair in place of a real client, so output can be read back
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  client_init(&c, sv[0], &addr);
  c.id = 5;

  printf("a shell task runs straight through\n");
  {
    task_t t;
    char   buf[8192];
    run_result r;

    task_init(&t, &c, "echo hello");

    quiet();
    r = runner_turn(&t, -1, 0, NULL);   // 0 seconds means no limit
    loud();

    ok("it finished",                r == RUN_DONE);
    ok("and will not run again",     t.state == TASK_DONE);
    ok("with no child left",         t.pid == -1);

    collect(sv[1], buf, sizeof buf);
    ok("its output reached the client", strcmp(buf, "hello\n") == 0);

    task_destroy(&t);
  }

  printf("\na program is stopped when its time runs out\n");
  {
    task_t t;
    char   buf[8192];
    char   first[8192];
    run_result r;
    pid_t  pid;
    struct timespec a, b;

    task_init(&t, &c, "./demo 6");
    ok("its estimate came from the command", t.remaining == 6);

    clock_gettime(CLOCK_MONOTONIC, &a);
    quiet();
    r = runner_turn(&t, -1, 3, NULL);   // a three second turn
    loud();
    clock_gettime(CLOCK_MONOTONIC, &b);

    pid = t.pid;

    ok("the turn ended because the time ran out", r == RUN_EXPIRED);
    ok("it took about three seconds",
       b.tv_sec - a.tv_sec >= 3 && b.tv_sec - a.tv_sec <= 4);
    ok("three seconds of work are left",  t.remaining == 3);
    ok("it is suspended, not finished",   t.state == TASK_STOPPED);
    ok("its child is still there",        t.pid > 0);
    ok("and the kernel says it is stopped", is_stopped(pid));
    ok("it has had one turn",             t.turns == 1);

    collect(sv[1], first, sizeof first);

    /*
    How many lines land in the first turn depends on whether the task gets
    its third-second line out just before or just after the clock runs out,
    so the count is not fixed. What must be true is that the turn stopped
    somewhere in the middle and began at the beginning.
    */
    ok("it began at the beginning",
       strncmp(first, "Demo 0/6\n", 9) == 0);
    ok("and stopped part way through",
       count_lines(first) >= 3 && count_lines(first) <= 4);

    printf("\nand it carries on from there rather than starting again\n");

    quiet();
    r = runner_turn(&t, -1, 7, NULL);   // the longer, later quantum
    loud();

    ok("the second turn finished it",  r == RUN_DONE);
    ok("nothing is left to do",        t.remaining == 0);
    ok("it has had two turns",         t.turns == 2);
    ok("and its child is gone",        t.pid == -1);

    collect(sv[1], buf, sizeof buf);

    ok("the second turn did not start over",
       strstr(buf, "Demo 0/6") == NULL);

    /*
    The real test of resuming: the two turns put end to end have to be the
    whole run exactly once, with no line repeated and none missing.
    */
    {
      char whole[8192];

      snprintf(whole, sizeof whole, "%s%s", first, buf);
      ok("the two turns together are the whole run, once",
         strcmp(whole,
                "Demo 0/6\nDemo 1/6\nDemo 2/6\nDemo 3/6\n"
                "Demo 4/6\nDemo 5/6\nDemo 6/6\n") == 0);
    }

    task_destroy(&t);
  }

  printf("\na running task can be asked to give way early\n");
  {
    task_t t;
    volatile sig_atomic_t preempt = 1;   // asked for before it even starts
    run_result r;
    char   buf[8192];

    task_init(&t, &c, "./demo 9");

    quiet();
    r = runner_turn(&t, -1, 7, &preempt);
    loud();

    ok("the turn ended as a preemption", r == RUN_PREEMPTED);
    ok("it is suspended",                t.state == TASK_STOPPED);
    ok("too little ran to be charged for", t.remaining == 9);

    collect(sv[1], buf, sizeof buf);

    printf("\nand killing a suspended task leaves nothing behind\n");
    {
      pid_t pid = t.pid;

      quiet();
      runner_kill(&t);
      loud();

      ok("the child is released",  t.pid == -1);
      // a process nobody waited for would still answer here
      ok("and collected, not left as a zombie",
         kill(pid, 0) == -1);
    }

    task_destroy(&t);
  }

  close(sv[0]);
  close(sv[1]);

  printf("\n%s (%d failure%s)\n",
         failures ? "FAILURES" : "ALL PASS",
         failures, failures == 1 ? "" : "s");
  return failures != 0;
}