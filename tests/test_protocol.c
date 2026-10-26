/* throwaway harness: exercises protocol.c over a real socket pair */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "protocol.h"

static int fails = 0;
static void ok(const char *what, int cond) {
  printf("  %-52s %s\n", what, cond ? "PASS" : "FAIL");
  if (!cond) fails++;
}

int main(void) {
  int sv[2];
  char buf[MAX_MSG + 1];
  size_t len;
  int r;

  /* --- round trip of several messages, including an empty one --- */
  printf("round trip\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  ok("send short", send_msg(sv[0], "ls -l", 5) == 0);
  ok("send empty", send_msg(sv[0], "", 0) == 0);
  ok("send with newlines+NUL-ish bytes", send_msg(sv[0], "a\nb\tc\x01", 6) == 0);
  r = recv_msg(sv[1], buf, sizeof buf, &len);
  ok("recv 1 returns 1", r == 1);
  ok("recv 1 length", len == 5);
  ok("recv 1 content", strcmp(buf, "ls -l") == 0);
  r = recv_msg(sv[1], buf, sizeof buf, &len);
  ok("recv 2 empty len 0", r == 1 && len == 0);
  ok("recv 2 terminated", buf[0] == '\0');
  r = recv_msg(sv[1], buf, sizeof buf, &len);
  ok("recv 3 binary-ish preserved", r == 1 && len == 6 && memcmp(buf, "a\nb\tc\x01", 6) == 0);
  close(sv[0]); close(sv[1]);

  /* --- a payload far bigger than one TCP segment --- */
  printf("large payload (fragmented by the kernel)\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  {
    char *big = malloc(60000);
    memset(big, 'x', 60000);
    big[0] = 'S'; big[59999] = 'E';
    if (fork() == 0) { send_msg(sv[0], big, 60000); close(sv[0]); _exit(0); }
    r = recv_msg(sv[1], buf, sizeof buf, &len);
    ok("recv large returns 1", r == 1);
    ok("recv large length 60000", len == 60000);
    ok("recv large first/last byte", buf[0] == 'S' && buf[59999] == 'E');
    wait(NULL); free(big);
  }
  close(sv[0]); close(sv[1]);

  /* --- header and payload arriving in separate, delayed writes --- */
  printf("split writes (header, then payload, separately)\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  {
    uint32_t h = htonl(11);
    if (fork() == 0) {
      write(sv[0], &h, 2);            /* half the header */
      usleep(50000);
      write(sv[0], ((char *)&h) + 2, 2); /* rest of header */
      usleep(50000);
      write(sv[0], "hello", 5);       /* part of payload */
      usleep(50000);
      write(sv[0], " world", 6);      /* rest of payload */
      close(sv[0]); _exit(0);
    }
    r = recv_msg(sv[1], buf, sizeof buf, &len);
    ok("reassembles across 4 writes", r == 1 && len == 11 && strcmp(buf, "hello world") == 0);
    wait(NULL);
  }
  close(sv[0]); close(sv[1]);

  /* --- peer closes between messages: clean end of session --- */
  printf("peer closes cleanly between messages\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  send_msg(sv[0], "one", 3);
  close(sv[0]);
  r = recv_msg(sv[1], buf, sizeof buf, &len);
  ok("pending message still delivered", r == 1 && strcmp(buf, "one") == 0);
  r = recv_msg(sv[1], buf, sizeof buf, &len);
  ok("then returns 0, not -1", r == 0);
  close(sv[1]);

  /* --- peer closes mid-message: truncated, must be an error --- */
  printf("peer closes midway through a message\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  {
    uint32_t h = htonl(100);
    write(sv[0], &h, sizeof h);
    write(sv[0], "only ten..", 10);
    close(sv[0]);
    r = recv_msg(sv[1], buf, sizeof buf, &len);
    ok("truncated message returns -1", r == -1);
  }
  close(sv[1]);

  /* --- a length that cannot fit is refused rather than trusted --- */
  printf("bogus length header\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  {
    uint32_t h = htonl(0xFFFFFFF0u);
    write(sv[0], &h, sizeof h);
    r = recv_msg(sv[1], buf, sizeof buf, &len);
    ok("absurd length returns -1", r == -1);
  }
  close(sv[0]); close(sv[1]);

  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  {
    char small[16];
    send_msg(sv[0], "this is longer than the buffer", 30);
    r = recv_msg(sv[1], small, sizeof small, &len);
    ok("payload too big for caller buffer returns -1", r == -1);
  }
  close(sv[0]); close(sv[1]);

  /* --- oversized send is refused up front --- */
  printf("oversized send\n");
  socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  ok("send over MAX_MSG returns -1", send_msg(sv[0], "x", MAX_MSG + 1) == -1);
  close(sv[0]); close(sv[1]);

  printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "ALL PASS", fails, fails == 1 ? "" : "s");
  return fails != 0;
}