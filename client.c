/*
client.c : the user facing half of the remote shell

connects to the server, then repeats: show a prompt, read a command line,
send it over the socket, wait for the output, print it.

the client never parses or runs anything itself. it only moves text, so the
command behaves exactly as it does in ./myshell because it is the same shell
code running it, just on the other end of the socket.

the session ends when the user types exit, presses ctrl+D, or the server
goes away.
*/

#include <stdio.h>      // printf(), fgets(), fflush(), fwrite(), getchar()
#include <stdlib.h>     // EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>     // strcspn(), strchr(), strlen(), strncmp()
#include <signal.h>     // signal(), SIGPIPE, SIG_IGN
#include <unistd.h>     // close(), isatty(), STDIN_FILENO
#include <arpa/inet.h>  // socket(), connect(), inet_pton(), htons()
#include "myshell.h"    // MAX_LINE, shared with the local shell
#include "protocol.h"   // send_msg(), recv_msg(), SERVER_IP, SERVER_PORT

// one byte larger than the biggest payload so recv_msg can terminate it
static char response[MAX_MSG + 1];


// 1 when the line holds nothing but spaces and tabs
static int is_blank(const char *line) {
  while (*line == ' ' || *line == '\t') line++;
  return *line == '\0';
}


/*
1 when the line is the exit command, ignoring spaces and tabs around it
the local shell accepts "  exit  " because it parses the line first, and the
client has no parser, so the same tolerance is spelled out here
*/
static int is_exit(const char *line) {
  const char *p = line;
  const char *end;

  while (*p == ' ' || *p == '\t') p++;

  if (strncmp(p, "exit", 4) != 0) return 0;

  end = p + 4;
  while (*end == ' ' || *end == '\t') end++;

  return *end == '\0';
}


/*
reads one whole response and prints it

a response is zero or more messages carrying output followed by an empty
message, so output of any size arrives in pieces and the empty one says
where it ends. each piece is printed as it arrives rather than collected
first, so the output of a long command appears while it is still coming

returns  1 when the whole response was printed
         0 when the server closed the connection
        -1 on a failure
*/
static int read_response(int sock) {
  size_t len;

  while (1) {
    switch (recv_msg(sock, response, sizeof response, &len)) {

      case 1:
        // the empty message ends the response
        if (len == 0) return 1;

        // the output already carries its own newlines, so it is written
        // exactly as it came rather than printed as a line
        fwrite(response, 1, len, stdout);
        fflush(stdout);
        break;

      case 0:
        return 0;

      default:
        return -1;
    }
  }
}


/*
opens a TCP connection to the server
returns the socket, or -1 after printing why it failed
*/
static int connect_to_server(void) {
  int fd;
  struct sockaddr_in addr;

  // AF_INET is IPv4, SOCK_STREAM is TCP, which is what the server listens on
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return -1;
  }

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;

  // the port has to be in network byte order, the same as the length prefix
  addr.sin_port = htons(SERVER_PORT);

  // turns the dotted address text into the number the kernel wants
  if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) != 1) {
    fprintf(stderr, "Invalid server address: %s\n", SERVER_IP);
    close(fd);
    return -1;
  }

  if (connect(fd, (struct sockaddr *) &addr, sizeof addr) == -1) {
    // the usual cause is that the server is not running yet, so say so
    // rather than leaving the user with a bare errno string
    fprintf(stderr, "Could not connect to the server at %s:%d.\n",
            SERVER_IP, SERVER_PORT);
    perror("connect");
    close(fd);
    return -1;
  }

  return fd;
}


int main(void) {
  char line[MAX_LINE];
  int sock;
  int status = EXIT_SUCCESS;

  // only prompt when a person is typing, so a piped session reads cleanly
  int interactive = isatty(STDIN_FILENO);

  /*
  writing to a socket whose other end has closed raises SIGPIPE, and the
  default action for that signal is to kill the process on the spot. the
  client would be killed mid session with no message whenever the server
  went away. ignoring it makes the write return an error instead, which the
  code below already handles and reports
  */
  signal(SIGPIPE, SIG_IGN);

  sock = connect_to_server();
  if (sock == -1) return EXIT_FAILURE;

  while (1) {

    if (interactive) {
      printf("$ ");

      // the prompt has no newline, so it has to be pushed out by hand or it
      // sits in the buffer while the client blocks on input
      fflush(stdout);
    }

    // ctrl+D ends the session; closing the socket is what tells the server
    if (fgets(line, sizeof(line), stdin) == NULL) {
      if (interactive) printf("\n");
      break;
    }

    // a full buffer with no newline means the line was longer than the
    // buffer and the rest is still waiting, so drop it rather than letting
    // it arrive as a second command
    if (strchr(line, '\n') == NULL && strlen(line) == MAX_LINE - 1) {
      int ch;
      while ((ch = getchar()) != '\n' && ch != EOF) {
        ;
      }
      fprintf(stderr, "Command line too long.\n");
      continue;
    }

    line[strcspn(line, "\n")] = '\0';

    // nothing to run, and no reason to make the server log a blank command
    // a line of only spaces counts, the same as it does in the local shell
    if (is_blank(line)) {
      continue;
    }

    /*
    exit is forwarded rather than handled here, because the server is
    required to shut down when it receives it

    it is sent trimmed, as exactly "exit", so the server can recognise it
    with a plain comparison. if the raw line went instead, a user typing
    "  exit  " would stop the client while the server kept running, which
    is the requirement failing quietly
    */
    if (is_exit(line)) {
      send_msg(sock, "exit", 4);
      break;
    }

    if (send_msg(sock, line, strlen(line)) != 0) {
      fprintf(stderr, "Connection to the server was lost.\n");
      status = EXIT_FAILURE;
      break;
    }

    switch (read_response(sock)) {

      case 1:
        break;   // whole response printed, prompt again

      case 0:
        // the server closed between messages, which is an orderly shutdown
        fprintf(stderr, "Server closed the connection.\n");
        close(sock);
        return status;

      default:
        fprintf(stderr, "Lost contact with the server.\n");
        close(sock);
        return EXIT_FAILURE;
    }
  }

  close(sock);
  return status;
}