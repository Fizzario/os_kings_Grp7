/*
tests/testserver.c : a stand in server, used only to test the client

this is NOT the project's server. it answers every command with a canned
reply instead of running anything, which is what makes the client's output
predictable enough for run_client_tests.sh to check automatically. the real
server runs commands, and the client cannot tell the two apart because it
only moves text either way.

send_response() below is how a response has to be sent under the protocol,
and the real server needs the same shape: the output goes out in MAX_MSG
sized pieces and is always followed by one empty message, which is what
tells the client the response has ended.

  make tests/testserver
  ./tests/testserver

commands it understands, each a different shape of response:
  big       - replies with 100 KB, more than one message holds
  huge      - replies with 3 MB
  quiet     - replies with no output at all
  slow      - replies in pieces with pauses between them
  anything else gets a short canned reply

  --one-shot  answers one command then closes, to test a server that dies
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "protocol.h"

static char buf[MAX_MSG + 1];


/*
sends one whole response: the output in as many messages as it takes,
then an empty message to mark the end

this is the part the real server needs to copy. a command's output can be
far larger than one message, so it is cut into MAX_MSG sized pieces, and the
empty message at the end is what stops the client waiting for more

returns 0 on success, -1 if the connection failed
*/
static int send_response(int fd, const char *out, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    size_t piece = len - sent;
    if (piece > MAX_MSG) piece = MAX_MSG;

    if (send_msg(fd, out + sent, piece) != 0) return -1;
    sent += piece;
  }

  // the empty message ends the response; a command that printed nothing
  // sends only this
  return send_msg(fd, "", 0);
}


int main(int argc, char *argv[]) {
  int listen_fd, conn_fd, yes = 1;
  struct sockaddr_in addr;
  size_t len;
  char reply[512];

  // --one-shot answers a single command and then closes without warning,
  // which is how the tests make the client face a server that has gone away
  int one_shot = (argc > 1 && strcmp(argv[1], "--one-shot") == 0);

  listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd == -1) { perror("socket"); return 1; }

  // lets the port be reused straight after a previous run exits
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(listen_fd, (struct sockaddr *) &addr, sizeof addr) == -1) {
    perror("bind"); return 1;
  }
  if (listen(listen_fd, 1) == -1) { perror("listen"); return 1; }

  printf("[INFO] Test server started, waiting for client connections...\n");
  fflush(stdout);

  conn_fd = accept(listen_fd, NULL, NULL);
  if (conn_fd == -1) { perror("accept"); return 1; }

  printf("[INFO] Client connected.\n");
  fflush(stdout);

  while (1) {
    int r = recv_msg(conn_fd, buf, sizeof buf, &len);

    if (r == 0) {
      printf("[INFO] Client disconnected, shutting down.\n");
      break;
    }
    if (r < 0) {
      printf("[ERROR] Receive failed, shutting down.\n");
      break;
    }

    printf("[RECEIVED] Received command: \"%s\" from client.\n", buf);
    fflush(stdout);

    if (strcmp(buf, "exit") == 0) {
      printf("[INFO] Client sent exit, shutting down.\n");
      break;
    }

    // the test commands, each exercising a different shape of response
    if (strcmp(buf, "big") == 0 || strcmp(buf, "huge") == 0) {
      size_t n = (strcmp(buf, "huge") == 0) ? 3000000 : 100000;
      char *out = malloc(n);
      memset(out, 'x', n);
      out[0] = 'S';
      out[n - 2] = 'E';
      out[n - 1] = '\n';
      printf("[OUTPUT] Sending %zu bytes to client.\n", n);
      fflush(stdout);
      if (send_response(conn_fd, out, n) != 0) { free(out); break; }
      free(out);
      continue;
    }

    if (strcmp(buf, "quiet") == 0) {
      printf("[OUTPUT] Sending no output to client.\n");
      fflush(stdout);
      if (send_response(conn_fd, "", 0) != 0) break;
      continue;
    }

    if (strcmp(buf, "slow") == 0) {
      printf("[OUTPUT] Sending output to client in pieces.\n");
      fflush(stdout);
      if (send_msg(conn_fd, "piece one\n", 10) != 0) break;
      sleep(1);
      if (send_msg(conn_fd, "piece two\n", 10) != 0) break;
      sleep(1);
      if (send_msg(conn_fd, "", 0) != 0) break;   // terminator
      continue;
    }

    snprintf(reply, sizeof reply,
             "stub reply line 1 for: %.200s\nstub reply line 2\n", buf);

    printf("[OUTPUT] Sending output to client:\n%s", reply);
    fflush(stdout);

    if (send_response(conn_fd, reply, strlen(reply)) != 0) {
      printf("[ERROR] Send failed, shutting down.\n");
      break;
    }

    if (one_shot) {
      printf("[INFO] One shot, closing without warning.\n");
      break;
    }
  }

  close(conn_fd);
  close(listen_fd);
  return 0;
}