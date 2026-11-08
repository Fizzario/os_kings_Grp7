/*
tests/test_clientlog.c : checks the log holds up with many threads at once

The thing being tested is not that the lines say the right words, which is
easy to read, but that an entry written by one thread is never cut in half
by another. That cannot be seen by running one thread: it needs several
threads logging as fast as they can, and then a check that every line of
the result is a whole line of one entry and nothing else.

The test writes the log to a file, reads it back, and checks every line.
Any interleaving shows up as a line that starts in one client's entry and
finishes in another's.

  gcc -Wall -Wextra -g -I. -pthread -o tests/test_clientlog \
      tests/test_clientlog.c clientlog.c
  ./tests/test_clientlog
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "clientlog.h"

#define THREADS 12      // clients logging at once
#define ROUNDS  60      // entries each one writes

static int failures = 0;

static void ok(const char *what, int cond) {
  printf("  %-54s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) failures++;
}


/*
The log writes to stdout, so to read it back the tests point stdout at a
file and then put it back. freopen cannot put it back, because the terminal
it came from may not be openable by name, so the original is kept as a
spare descriptor with dup and restored with dup2.
*/
static int saved_stdout = -1;

static void capture(const char *path) {
  int fd;

  fflush(stdout);
  if (saved_stdout == -1)
    saved_stdout = dup(STDOUT_FILENO);

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  dup2(fd, STDOUT_FILENO);
  close(fd);
}

static void uncapture(void) {
  fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
}


// reads a captured log back into buf, terminated
static size_t slurp(const char *path, char *buf, size_t size) {
  FILE *f = fopen(path, "r");
  size_t n;

  if (f == NULL) { buf[0] = 0; return 0; }
  n = fread(buf, 1, size - 1, f);
  buf[n] = 0;
  fclose(f);
  return n;
}


// builds a client_t as the server would, from a made up address
static void make_client(client_t *c, int fd, const char *ip, int port) {
  struct sockaddr_in a;

  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short) port);
  inet_pton(AF_INET, ip, &a.sin_addr);

  client_init(c, fd, &a);
}


/*
One thread's work: a client that connects, runs a few commands with output
of varying size, hits an error, and leaves. The output is deliberately
multi-line, since a multi-line entry is what would interleave if the lock
were missing.
*/
static void *client_thread(void *arg) {
  int n = *(int *) arg;
  client_t c;
  char ip[32], body[600], cmd[64];
  int i, j;

  snprintf(ip, sizeof ip, "10.0.0.%d", n + 1);
  make_client(&c, 100 + n, ip, 40000 + n);

  log_client_connected(&c, n + 1);

  for (i = 0; i < ROUNDS; i++) {
    snprintf(cmd, sizeof cmd, "cmd-%d-%d", c.id, i);

    log_received(&c, cmd);
    log_executing(&c, cmd);

    if (i % 3 == 2) {
      // an error, which is the two line entry
      char err[128];
      snprintf(err, sizeof err, "Command not found: %s\n", cmd);
      log_error(&c, err, strlen(err));
    }
    else {
      // a multi-line output entry, sent in pieces as a command's would be
      log_output_begin(&c);
      for (j = 0; j < 8; j++) {
        snprintf(body, sizeof body,
                 "client%d-line%d-of-%d aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",
                 c.id, j, i);
        log_output_chunk(&c, body, strlen(body));
      }
      log_output_end(&c);
    }
  }

  log_disconnect_requested(&c);
  log_client_disconnected(&c);
  return NULL;
}


/*
Reads the log back and checks every line is whole.

Every line the log can produce starts with one of the known tags or is a
line of a command's output, and the output lines all have a known shape.
A line that is neither is a line two entries were spliced into.
*/
static int check_lines(const char *path, int *lines_seen) {
  char line[4096];
  FILE *f = fopen(path, "r");
  int bad = 0;

  *lines_seen = 0;
  if (f == NULL) return -1;

  while (fgets(line, sizeof line, f) != NULL) {
    size_t len = strlen(line);
    (*lines_seen)++;

    // every line must end in a newline; one that does not means the write
    // was cut short
    if (len == 0 || line[len - 1] != '\n') {
      printf("    unterminated line: %.80s\n", line);
      bad++;
      continue;
    }

    if (strncmp(line, "[INFO] ", 7) == 0)      continue;
    if (strncmp(line, "[RECEIVED] ", 11) == 0) continue;
    if (strncmp(line, "[EXECUTING] ", 12) == 0)continue;
    if (strncmp(line, "[ERROR] ", 8) == 0)     continue;
    if (strncmp(line, "[OUTPUT] ", 9) == 0)    continue;

    // an output body line, as written by the thread above
    if (strncmp(line, "client", 6) == 0 && strstr(line, "-line") != NULL)
      continue;

    printf("    spliced line: %.80s\n", line);
    bad++;
  }

  fclose(f);
  return bad;
}


/*
Checks that the body lines of one output entry are all from the same client
and in order, which catches an interleaving that happened to leave every
individual line whole.
*/
static int check_entries(const char *path) {
  char line[4096];
  FILE *f = fopen(path, "r");
  int bad = 0, in_entry = 0, want_client = 0, want_line = 0;

  if (f == NULL) return -1;

  while (fgets(line, sizeof line, f) != NULL) {
    if (strncmp(line, "[OUTPUT] [Client #", 18) == 0 &&
        strstr(line, "Sending output to client:") != NULL) {
      sscanf(line, "[OUTPUT] [Client #%d", &want_client);
      in_entry = 1;
      want_line = 0;
      continue;
    }

    if (!in_entry) continue;

    if (line[0] == '[') {        // the entry ended, a new one starts
      if (want_line != 8) {
        printf("    entry for client %d had %d body lines, wanted 8\n",
               want_client, want_line);
        bad++;
      }
      in_entry = 0;
      // fall through so this line can open a new entry next time round
      if (strncmp(line, "[OUTPUT] [Client #", 18) == 0 &&
          strstr(line, "Sending output to client:") != NULL) {
        sscanf(line, "[OUTPUT] [Client #%d", &want_client);
        in_entry = 1;
        want_line = 0;
      }
      continue;
    }

    {
      int got_client = -1, got_line = -1;
      sscanf(line, "client%d-line%d", &got_client, &got_line);

      if (got_client != want_client) {
        printf("    client %d's entry contains a line from client %d\n",
               want_client, got_client);
        bad++;
      }
      if (got_line != want_line) {
        printf("    client %d: body line out of order (%d, wanted %d)\n",
               want_client, got_line, want_line);
        bad++;
      }
      want_line++;
    }
  }

  fclose(f);
  return bad;
}


int main(void) {
  const char *path = "tests/.clientlog_out";
  pthread_t th[THREADS];
  int arg[THREADS];
  int i, lines = 0, bad;
  client_t c;
  struct sockaddr_in a;

  // --------------------------------------------------------------- format
  // checked with one thread, where the output can simply be read
  printf("the lines the spec's figure shows\n");
  {
    char buf[8192];
    FILE *f;
    size_t n;

    capture(path);

    make_client(&c, 7, "192.168.1.100", 56789);
    log_server_started();
    log_client_connected(&c, 1);
    log_received(&c, "ls -l");
    log_executing(&c, "ls -l");
    log_output_begin(&c);
    log_output_chunk(&c, "total 12\n", 9);
    log_output_end(&c);
    log_error(&c, "Command not found: unknowncmd\n", 30);
    log_no_output(&c);
    log_disconnect_requested(&c);
    log_client_disconnected(&c);

    uncapture();

    n = slurp(path, buf, sizeof buf);
    (void) n; (void) f;

    ok("server started",
       strstr(buf, "[INFO] Server started, waiting for client connections...\n") != NULL);
    ok("client connected names address and thread",
       strstr(buf, "[INFO] Client #1 connected from 192.168.1.100:56789. "
                   "Assigned to Thread-1.\n") != NULL);
    ok("received carries the client prefix",
       strstr(buf, "[RECEIVED] [Client #1 - 192.168.1.100:56789] "
                   "Received command: \"ls -l\"\n") != NULL);
    ok("executing carries the client prefix",
       strstr(buf, "[EXECUTING] [Client #1 - 192.168.1.100:56789] "
                   "Executing command: \"ls -l\"\n") != NULL);
    ok("output header then the body",
       strstr(buf, "[OUTPUT] [Client #1 - 192.168.1.100:56789] "
                   "Sending output to client:\ntotal 12\n") != NULL);
    ok("a missing command is quoted by name",
       strstr(buf, "[ERROR] [Client #1 - 192.168.1.100:56789] "
                   "Command not found: \"unknowncmd\"\n") != NULL);
    ok("and then announced as sent",
       strstr(buf, "[OUTPUT] [Client #1 - 192.168.1.100:56789] Sending error "
                   "message to client: \"Command not found: unknowncmd\"\n") != NULL);
    ok("no output",
       strstr(buf, "[OUTPUT] [Client #1 - 192.168.1.100:56789] "
                   "Sending no output to client.\n") != NULL);
    ok("disconnect requested",
       strstr(buf, "[INFO] [Client #1 - 192.168.1.100:56789] Client requested "
                   "disconnect. Closing connection.\n") != NULL);
    ok("client disconnected",
       strstr(buf, "[INFO] Client #1 disconnected.\n") != NULL);
  }

  // ------------------------------------------------------------ numbering
  printf("\nclient numbers\n");
  {
    client_t a1, a2;
    make_client(&a1, 1, "10.1.1.1", 1111);
    make_client(&a2, 2, "10.1.1.2", 2222);
    ok("each client gets a new number", a2.id == a1.id + 1);
  }

  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(1234);
  a.sin_addr.s_addr = htonl(0x7f000001);
  ok("client_init succeeds on a normal address", client_init(&c, 3, &a) == 0);

  // ------------------------------------------------------- the real point
  printf("\n%d threads logging at once\n", THREADS);

  capture(path);
  for (i = 0; i < THREADS; i++) {
    arg[i] = i;
    pthread_create(&th[i], NULL, client_thread, &arg[i]);
  }
  for (i = 0; i < THREADS; i++)
    pthread_join(th[i], NULL);
  uncapture();

  bad = check_lines(path, &lines);
  ok("every line is whole", bad == 0);

  bad = check_entries(path);
  ok("no entry contains another client's output", bad == 0);

  printf("  (%d lines written by %d threads)\n", lines, THREADS);

  // -------------------------------------------------------------- bounded
  printf("\na command with more output than the log keeps\n");
  {
    char buf[LOG_OUTPUT_MAX * 2];
    char big[4096];
    FILE *f;
    size_t n;

    memset(big, 'x', sizeof big);
    big[sizeof big - 1] = '\n';

    capture(path);
    make_client(&c, 9, "10.9.9.9", 9999);
    log_output_begin(&c);
    for (i = 0; i < 6; i++)                 // 24 KB into an 8 KB buffer
      log_output_chunk(&c, big, sizeof big);
    log_output_end(&c);
    uncapture();

    n = slurp(path, buf, sizeof buf);
    (void) f;

    ok("the entry is bounded", n < LOG_OUTPUT_MAX + 512);
    ok("and says how much it left out",
       strstr(buf, "more bytes sent to the client, not shown here") != NULL);
  }

  remove(path);

  printf("\n%s (%d failure%s)\n",
         failures ? "FAILURES" : "ALL PASS",
         failures, failures == 1 ? "" : "s");
  return failures != 0;
}