/*

Listens on a TCP port and serves clients, running their commands with the
parser and executor from phase 1 and sending the output back over the
socket. A client's session ends when it sends exit or disconnects; the
server closes that connection, logs it, and goes back to waiting for the
next client. It does not stop on its own.

For each command the flow is:
  recv_msg gets the line from the client
  a child process is forked with its stdout and stderr pointing at two pipes
  the child parses and executes the line, exactly as myshell would
  the server reads stdout then stderr, logs them and passes them to the client
  an empty message tells the client the response is finished
*/

#include <stdio.h>      // perror()
#include <stdlib.h>     // EXIT_SUCCESS, EXIT_FAILURE, malloc(), free()
#include <string.h>     // memset(), strcmp(), strlen(), strncmp()
#include <fcntl.h>      // open(), O_RDONLY
#include <signal.h>     // signal(), SIGPIPE, SIG_IGN, SIG_DFL
#include <unistd.h>     // close(), fork(), pipe(), dup2(), read(), _exit()
#include <sys/wait.h>   // waitpid(), WIFEXITED, WEXITSTATUS
#include <arpa/inet.h>  // socket(), bind(), listen(), accept(), htons(), htonl()
#include "myshell.h"    // pipeline_t, parse_line(), execute_pipeline(), MAX_LINE
#include "protocol.h"   // send_msg(), recv_msg(), SERVER_PORT, MAX_MSG
#include "clientlog.h"  // client_t, client_init(), the log_* entries

#define READ_END 0
#define WRITE_END 1

#define RUN_FAILED "Server could not run the command.\n"

/*
How many connections the kernel may hold while the server is busy with
another. One was enough when the server stopped after a single client; now
that it keeps going, a client arriving while another is being accepted has
to wait somewhere rather than be refused.
*/
#define BACKLOG 16

// exit code the child uses to say the line was an exit command
#define EXIT_REQUESTED 42


/*
Everything one connected client needs. The two buffers used to be
file-scope, which was fine while one client was served at a time. Serving
clients concurrently would have two sessions reading commands into the same
msg and output into the same out, so each session carries its own.
*/
typedef struct {
  client_t client;            // who this is, for the log
  char msg[MAX_MSG + 1];      // the command just received, terminated
  char out[MAX_MSG];          // one read of a command's output
} session_t;


// creates the socket, binds it to SERVER_PORT and starts listening
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

  if (listen(fd, BACKLOG) == -1) {
    perror("listen");
    close(fd);
    return -1;
  }

  return fd;
}


// sends an error of the server's own, as a whole response
static int send_error(session_t *s, const char *text) {
  log_error(&s->client, text, strlen(text));
  log_blank();

  if (send_msg(s->client.fd, text, strlen(text)) != 0)
    return -1;
  return send_msg(s->client.fd, "", 0);
}


/*
Runs in the child: points stdout and stderr at their pipes, then parses and
executes the line the same way myshell does.
*/
static void run_child(session_t *s, int listen_fd, int ofd, int efd) {
  pipeline_t pl;
  int devnull;

  signal(SIGPIPE, SIG_DFL);

  close(listen_fd);
  close(s->client.fd);

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

  report_command_name = 1;

  /*
  parse_line keeps its token text in storage of its own that is shared
  across the process, so it is safe here only because this is the forked
  child, with a copy of everything and the only one parsing in it. Parsing
  a command anywhere but in the child would have to be revisited.
  */

  // a parse error has already gone to stderr, which is now the pipe
  if (parse_line(s->msg, &pl) != 0 || pl.ncmds == 0)
    _exit(0);

  if (pl.ncmds == 1 && strcmp(pl.cmds[0].argv[0], "exit") == 0)
    _exit(EXIT_REQUESTED);

  execute_pipeline(&pl);
  _exit(0);
}


/*
Returns 0, or -1 if the client is gone.
*/
static int forward_output(session_t *s, int fd, int is_err, int *shown_any) {
  ssize_t n;

  while ((n = read(fd, s->out, sizeof s->out)) > 0) {
    if (is_err)
      log_error(&s->client, s->out, (size_t) n);
    else
      log_output_chunk(&s->client, s->out, (size_t) n);

    *shown_any = 1;

    if (send_msg(s->client.fd, s->out, (size_t) n) != 0)
      return -1;
  }

  return 0;
}


/*
Runs one command and sends its output to the client.
*/
static int run_command(session_t *s, int listen_fd) {
  int ofds[2], efds[2];
  int status = 0;
  int shown_any = 0;
  int lost;
  pid_t pid;

  // on any failure here the client still has to get a response, otherwise it
  // waits forever.
  if (pipe(ofds) == -1) {
    perror("pipe");
    return send_error(s, RUN_FAILED) == 0 ? 1 : -1;
  }
  if (pipe(efds) == -1) {
    perror("pipe");
    close(ofds[READ_END]);
    close(ofds[WRITE_END]);
    return send_error(s, RUN_FAILED) == 0 ? 1 : -1;
  }

  pid = fork();

  if (pid == -1) {
    perror("fork");
    close(ofds[READ_END]);
    close(ofds[WRITE_END]);
    close(efds[READ_END]);
    close(efds[WRITE_END]);
    return send_error(s, RUN_FAILED) == 0 ? 1 : -1;
  }
  else if (pid == 0) { // the child process
    close(ofds[READ_END]);
    close(efds[READ_END]);
    run_child(s, listen_fd, ofds[WRITE_END], efds[WRITE_END]);
  }

  // our copies have to go or read below never sees the end of the output
  close(ofds[WRITE_END]);
  close(efds[WRITE_END]);

  log_output_begin(&s->client);

  lost = forward_output(s, ofds[READ_END], 0, &shown_any);

  // the output entry is printed before any error entry, so the two appear
  // in the order the spec's figure shows them
  log_output_end(&s->client);

  if (lost == 0)
    lost = forward_output(s, efds[READ_END], 1, &shown_any);

  close(ofds[READ_END]);
  close(efds[READ_END]);

  waitpid(pid, &status, 0);

  if (lost)
    return -1;

  // no response for exit, the client is told by the connection closing
  if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_REQUESTED)
    return 0;

  if (!shown_any)
    log_no_output(&s->client);
  log_blank();

  // the empty message ends the response
  return send_msg(s->client.fd, "", 0) == 0 ? 1 : -1;
}


/*
Serves one client until it leaves, then closes its socket and frees its
session. Everything it touches comes from the session it is given, so this
is the whole of one client's work and nothing outside it is involved.
*/
static void serve_client(session_t *s, int listen_fd) {
  size_t len;

  while (1) {
    // blocks until the next command, framing is handled in protocol.c
    int r = recv_msg(s->client.fd, s->msg, sizeof s->msg, &len);

    // 0 is the client closing between messages, the normal way to disconnect
    if (r == 0)
      break;

    // a bad length or a connection cut midway through a message
    if (r < 0) {
      log_info(&s->client, "Receive failed. Closing connection.");
      break;
    }

    // myclient already refuses these, but the parser's buffers are MAX_LINE
    // so the server can't count on the other end being myclient
    if (len >= MAX_LINE) {
      if (send_error(s, "Command line too long.\n") != 0)
        break;
      continue;
    }

    log_received(&s->client, s->msg);

    // myclient trims exit before sending, so a plain compare catches the usual
    // case without forking. "exit now" or "exit" in quotes still reach the
    // child, which recognises them after parsing
    if (strcmp(s->msg, "exit") == 0) {
      log_disconnect_requested(&s->client);
      break;
    }

    log_executing(&s->client, s->msg);

    r = run_command(s, listen_fd);

    // the child recognised an exit the plain compare above did not
    if (r == 0) {
      log_disconnect_requested(&s->client);
      break;
    }
    if (r < 0)
      break;
  }

  log_client_disconnected(&s->client);

  close(s->client.fd);
  free(s);
}


int main(void) {
  int listen_fd;

  /*
  A client that disappears should make a send fail rather than kill the
  server. Without this, one client closing its connection at the wrong
  moment would take the server down and every other client with it.
  */
  signal(SIGPIPE, SIG_IGN);

  listen_fd = start_listening();
  if (listen_fd == -1)
    return EXIT_FAILURE;

  log_server_started();

  while (1) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof addr;
    session_t *s;
    int fd;

    // the address is kept this time: the log names every client by where it
    // connected from, so accept has to be asked for it
    fd = accept(listen_fd, (struct sockaddr *) &addr, &addrlen);
    if (fd == -1) {
      perror("accept");
      continue;   // one failed accept is not a reason to stop serving
    }

    s = malloc(sizeof *s);
    if (s == NULL) {
      // nothing can be done for this client, but the ones already connected
      // are unaffected and the next one may well fit
      perror("malloc");
      close(fd);
      continue;
    }

    client_init(&s->client, fd, &addr);

    log_client_connected(&s->client, 1);

    serve_client(s, listen_fd);
  }

}