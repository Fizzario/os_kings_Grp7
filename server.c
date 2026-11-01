/*
server.c : the remote side of the shell

listens on a TCP port, accepts one client and runs its commands with the
parser and executor from phase 1, sending the output back over the socket
stops when the client sends exit or disconnects

for each command the flow is:
  recv_msg gets the line from the client
  a child process is forked with its stdout and stderr pointing at two pipes
  the child parses and executes the line, exactly as myshell would
  the server reads stdout then stderr, logs them and passes them to the client
  an empty message tells the client the response is finished

the shell code runs in a child so that it can't touch the server's own stdout,
which is the log. redirecting stdout in the server and putting it back after
each command would mean a mistake there breaks the log for the whole session
*/

#include <stdio.h>      // printf(), perror(), fflush()
#include <stdlib.h>     // EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>     // memset(), strcmp(), strlen(), strncmp()
#include <fcntl.h>      // open(), O_RDONLY
#include <signal.h>     // signal(), SIGPIPE, SIG_IGN, SIG_DFL
#include <unistd.h>     // close(), fork(), pipe(), dup2(), read(), _exit()
#include <sys/wait.h>   // waitpid(), WIFEXITED, WEXITSTATUS
#include <arpa/inet.h>  // socket(), bind(), listen(), accept(), htons(), htonl()
#include "myshell.h"    // pipeline_t, parse_line(), execute_pipeline(), MAX_LINE
#include "protocol.h"   // send_msg(), recv_msg(), SERVER_PORT, MAX_MSG

#define READ_END 0
#define WRITE_END 1

#define RUN_FAILED "Server could not run the command.\n"

// exit code the child uses to say the line was an exit command
// execute_pipeline returns nothing and the child always exits 0 otherwise, so
// a command's own exit status can't be confused with this
#define EXIT_REQUESTED 42

// one byte larger than the biggest message so recv_msg can terminate it
static char msg[MAX_MSG + 1];

// one read of command output, at most one message
// static because 64 KB is a lot to keep on the stack for every command
static char out[MAX_MSG];


// creates the socket, binds it to SERVER_PORT and starts listening
// returns the listening socket, or -1 after printing what failed
static int start_listening(void) {
  int fd, yes = 1;
  struct sockaddr_in addr;

  // AF_INET is IPv4, SOCK_STREAM is TCP
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return -1;
  }

  // without this bind fails for a while after the server is restarted
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  // the port and address have to be in network byte order, INADDR_ANY means
  // accept connections on any address this machine has
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr *) &addr, sizeof addr) == -1) {
    perror("bind");
    close(fd);
    return -1;
  }

  // single client model, so the backlog can stay at 1
  if (listen(fd, 1) == -1) {
    perror("listen");
    close(fd);
    return -1;
  }

  return fd;
}


// prints an error the way the spec's server output shows it, two lines:
//   [ERROR] Command not found: "unknowncmd"
//   [OUTPUT] Sending error message to client: "Command not found: unknowncmd"
// the first line quotes the name of a missing command, the second one doesn't
// any other error text, from the parser or a program, is printed as it came
static void log_error(const char *text, size_t len) {
  static const char prefix[] = "Command not found: ";
  size_t plen = sizeof prefix - 1;

  // the newline stays in what goes to the client but not inside the quotes
  while (len > 0 && text[len - 1] == '\n')
    len--;

  if (len > plen && strncmp(text, prefix, plen) == 0)
    printf("[ERROR] Command not found: \"%.*s\"\n", (int) (len - plen), text + plen);
  else
    printf("[ERROR] %.*s\n", (int) len, text);

  printf("[OUTPUT] Sending error message to client: \"%.*s\"\n", (int) len, text);
}


// sends an error of the server's own, as a whole response: the text and then
// the empty message, since the client is waiting for that to prompt again
// returns 0, or -1 if the client is gone
static int send_error(int conn_fd, const char *text) {
  log_error(text, strlen(text));
  printf("\n");

  if (send_msg(conn_fd, text, strlen(text)) != 0)
    return -1;
  return send_msg(conn_fd, "", 0);
}


// runs in the child: points stdout and stderr at their pipes, then parses and
// executes the line the same way myshell does
// the shell code stays unchanged since it only writes to stdout and stderr
// never returns, it ends with _exit like the children in executor.c
static void run_child(char *line, int listen_fd, int conn_fd, int ofd, int efd) {
  pipeline_t pl;
  int devnull;

  // the server ignores SIGPIPE and exec keeps that, without this yes | head
  // has yes fail with a write error instead of just being stopped
  signal(SIGPIPE, SIG_DFL);

  // fork copied every open descriptor. commands have no use for the sockets,
  // and one left running would keep the port and the connection open
  close(listen_fd);
  close(conn_fd);

  // the client can't send input to a command, so anything reading stdin
  // (cat with no file, sort, wc) would wait forever on the server's terminal
  devnull = open("/dev/null", O_RDONLY);
  if (devnull != -1) {
    dup2(devnull, STDIN_FILENO);
    close(devnull);
  }

  // from here anything the shell code or a command prints goes to the server
  dup2(ofd, STDOUT_FILENO);
  dup2(efd, STDERR_FILENO);
  close(ofd);
  close(efd);

  // the spec's client shows the name of the missing command, myshell doesn't
  report_command_name = 1;

  // a parse error has already gone to stderr, which is now the pipe, so the
  // client sees it like any other error
  if (parse_line(line, &pl) != 0 || pl.ncmds == 0)
    _exit(0);

  // same check as myshell.c, so "exit now" or a quoted exit also stops it
  if (pl.ncmds == 1 && strcmp(pl.cmds[0].argv[0], "exit") == 0)
    _exit(EXIT_REQUESTED);

  execute_pipeline(&pl);
  _exit(0);
}


// reads one pipe until every writer has closed it, logging each piece and
// passing it to the client. is_err says which of the two logs it gets
// returns 0, or -1 if the client is gone
static int forward_output(int fd, int conn_fd, int is_err, int *shown_any) {
  int shown_output = 0;
  ssize_t n;

  while ((n = read(fd, out, sizeof out)) > 0) {
    // the two pipes are logged differently but sent the same way
    if (is_err) {
      log_error(out, (size_t) n);
    }
    else {
      if (!shown_output) {
        printf("[OUTPUT] Sending output to client:\n");
        shown_output = 1;
      }
      fwrite(out, 1, (size_t) n, stdout);
      fflush(stdout);
    }
    *shown_any = 1;

    if (send_msg(conn_fd, out, (size_t) n) != 0)
      return -1;
  }

  return 0;
}


// runs one command and sends its output to the client
// output is passed on as it arrives instead of being collected first, so its
// size doesn't matter and a slow command shows its output while it runs
// returns 1 to keep going, 0 if the line was an exit command, -1 if the
// client is gone
static int run_command(char *line, int listen_fd, int conn_fd) {
  int ofds[2], efds[2];
  int status = 0;
  int shown_any = 0;
  int lost;
  pid_t pid;

  // on any failure here the client still has to get a response, otherwise it
  // waits forever. the session carries on, the next command may well work
  if (pipe(ofds) == -1) {
    perror("pipe");
    return send_error(conn_fd, RUN_FAILED) == 0 ? 1 : -1;
  }
  if (pipe(efds) == -1) {
    perror("pipe");
    close(ofds[READ_END]);
    close(ofds[WRITE_END]);
    return send_error(conn_fd, RUN_FAILED) == 0 ? 1 : -1;
  }

  // anything still buffered would be copied into the child
  fflush(stdout);

  pid = fork();

  if (pid == -1) {
    perror("fork");
    close(ofds[READ_END]);
    close(ofds[WRITE_END]);
    close(efds[READ_END]);
    close(efds[WRITE_END]);
    return send_error(conn_fd, RUN_FAILED) == 0 ? 1 : -1;
  }
  else if (pid == 0) { // the child process
    close(ofds[READ_END]);
    close(efds[READ_END]);
    run_child(line, listen_fd, conn_fd, ofds[WRITE_END], efds[WRITE_END]);
  }

  // our copies have to go or read below never sees the end of the output
  close(ofds[WRITE_END]);
  close(efds[WRITE_END]);

  // read while the command runs, a pipe only holds about 64 KB so waiting
  // for the command first would deadlock on large output
  // stdout goes first and then stderr, so errors come after normal output
  // stderr isn't read until stdout closes, so a command that writes more than
  // a pipe holds to stderr while stdout is still open has to wait here
  lost = forward_output(ofds[READ_END], conn_fd, 0, &shown_any);
  if (lost == 0)
    lost = forward_output(efds[READ_END], conn_fd, 1, &shown_any);

  // closed before the wait, so a command still writing gets stopped by SIGPIPE
  close(ofds[READ_END]);
  close(efds[READ_END]);

  waitpid(pid, &status, 0);

  if (lost)
    return -1;

  // no response for exit, the client is told by the connection closing
  if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_REQUESTED)
    return 0;

  if (!shown_any)
    printf("[OUTPUT] Sending no output to client.\n");
  printf("\n");

  // the empty message ends the response
  return send_msg(conn_fd, "", 0) == 0 ? 1 : -1;
}


int main(void) {
  int listen_fd, conn_fd;
  int status = EXIT_SUCCESS;
  size_t len;

  // a client that disappears should make send fail, not kill the server
  // send_msg then returns -1 and the server shuts down normally
  signal(SIGPIPE, SIG_IGN);

  listen_fd = start_listening();
  if (listen_fd == -1)
    return EXIT_FAILURE;

  printf("[INFO] Server started, waiting for client connections...\n");
  fflush(stdout); // a log going to a file wouldn't show this until much later

  // blocks until a client connects. this is the only accept, one server and
  // one client is the model the spec asks for
  conn_fd = accept(listen_fd, NULL, NULL);
  if (conn_fd == -1) {
    perror("accept");
    close(listen_fd);
    return EXIT_FAILURE;
  }

  printf("[INFO] Client connected.\n");

  while (1) {
    // blocks until the next command, framing is handled in protocol.c
    int r = recv_msg(conn_fd, msg, sizeof msg, &len);

    // 0 is the client closing between messages, the normal way to disconnect
    if (r == 0) {
      printf("[INFO] Client disconnected, shutting down.\n");
      break;
    }
    // a bad length or a connection cut midway through a message
    if (r < 0) {
      printf("[ERROR] Receive failed, shutting down.\n");
      status = EXIT_FAILURE;
      break;
    }

    // myclient already refuses these, but the parser's buffers are MAX_LINE
    // so the server can't count on the other end being myclient
    if (len >= MAX_LINE) {
      if (send_error(conn_fd, "Command line too long.\n") != 0) {
        printf("[INFO] Client disconnected, shutting down.\n");
        break;
      }
      continue;
    }

    printf("[RECEIVED] Received command: \"%s\" from client.\n", msg);

    // myclient trims exit before sending, so a plain compare catches the usual
    // case without forking. "exit now" or "exit" in quotes still reach the
    // child, which recognises them after parsing
    if (strcmp(msg, "exit") == 0) {
      printf("[INFO] Client sent exit, shutting down.\n");
      break;
    }

    printf("[EXECUTING] Executing command: \"%s\"\n", msg);

    r = run_command(msg, listen_fd, conn_fd);

    if (r == 0) {
      printf("[INFO] Client sent exit, shutting down.\n");
      break;
    }
    if (r < 0) {
      printf("[INFO] Client disconnected, shutting down.\n");
      break;
    }
  }

  close(conn_fd);
  close(listen_fd);
  return status;
}
