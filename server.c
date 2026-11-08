/*
server.c : the remote side of the shell

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

The shell code runs in a child so that it can't touch the server's own
stdout, which is the log. Redirecting stdout in the server and putting it
back after each command would mean a mistake there breaks the log for the
whole session.

Everything one connection needs lives in a session_t, allocated when the
client is accepted and freed when it leaves. Nothing about a client is kept
in a file-scope variable: serve_client() reads and writes only through the
session it is given, so running two of them at once is a matter of starting
them, not of untangling shared state first.
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
another. One was enough while the server served a single client and then
stopped; now that it keeps going, a client arriving while another is being
accepted has to wait somewhere rather than be refused.
*/
#define BACKLOG 16

// exit code the child uses to say the line was an exit command
// execute_pipeline returns nothing and the child always exits 0 otherwise, so
// a command's own exit status can't be confused with this
#define EXIT_REQUESTED 42


/*
Everything one connected client needs.

The two buffers used to be file-scope, which was fine while the server
served one client at a time in one process. Serving clients concurrently
makes that a bug rather than a shortcut: two sessions would be reading
commands into the same msg and the same command's output into the same out,
and each would see the other's bytes. They are here instead, so every
session has its own and no session can reach another's.

They are large enough that one session is some 128 KB, which is why a
session is allocated rather than being a local: a local of that size is a
surprise on any stack, and allocating it is also the shape a thread needs,
since the thread has to be handed something that outlives the call that
started it.
*/
typedef struct {
  client_t client;            // who this is, for the log
  char msg[MAX_MSG + 1];      // the command just received, terminated
  char out[MAX_MSG];          // one read of a command's output
} session_t;


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

  if (listen(fd, BACKLOG) == -1) {
    perror("listen");
    close(fd);
    return -1;
  }

  return fd;
}


// sends an error of the server's own, as a whole response: the text and then
// the empty message, since the client is waiting for that to prompt again
// returns 0, or -1 if the client is gone
static int send_error(session_t *s, const char *text) {
  log_error(&s->client, text, strlen(text));
  log_blank();

  if (send_msg(s->client.fd, text, strlen(text)) != 0)
    return -1;
  return send_msg(s->client.fd, "", 0);
}


/*
Runs in the child: points stdout and stderr at their pipes, then parses and
executes the line the same way myshell does. The shell code stays unchanged
since it only writes to stdout and stderr. Never returns; it ends with
_exit like the children in executor.c.

Note for the threaded server: the descriptors closed below are this
session's. A child forked while other sessions are open also inherits their
sockets, and holds them until the command finishes, which keeps another
client's connection half open after that client has left. Closing those too
means knowing about them, which is a list of live sessions.
*/
static void run_child(session_t *s, int listen_fd, int ofd, int efd) {
  pipeline_t pl;
  int devnull;

  // the server ignores SIGPIPE and exec keeps that, without this yes | head
  // has yes fail with a write error instead of just being stopped
  signal(SIGPIPE, SIG_DFL);

  // fork copied every open descriptor. commands have no use for the sockets,
  // and one left running would keep the port and the connection open
  close(listen_fd);
  close(s->client.fd);

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

  /*
  parse_line keeps its token text in storage of its own that is shared
  across the process, so it is safe here and would not be in a thread: this
  is a child, with a copy of everything, and the only one parsing in it.
  Parsing a command anywhere but in the child would have to be revisited.
  */

  // a parse error has already gone to stderr, which is now the pipe, so the
  // client sees it like any other error
  if (parse_line(s->msg, &pl) != 0 || pl.ncmds == 0)
    _exit(0);

  // same check as myshell.c, so "exit now" or a quoted exit also stops it
  if (pl.ncmds == 1 && strcmp(pl.cmds[0].argv[0], "exit") == 0)
    _exit(EXIT_REQUESTED);

  execute_pipeline(&pl);
  _exit(0);
}


/*
Reads one pipe until every writer has closed it, logging each piece and
passing it to the client. is_err says which of the two it is: output is
collected into one log entry, while each piece of error text is an entry of
its own, which is the shape the spec's figure shows.

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

Output is passed on as it arrives instead of being collected first, so its
size doesn't matter and a slow command shows its output while it runs. The
log is the other way round: the pieces are collected and the whole entry
printed at the end, so that another client's entries cannot land in the
middle of this one.

Returns 1 to keep going, 0 if the line was an exit command, -1 if the
client is gone.
*/
static int run_command(session_t *s, int listen_fd) {
  int ofds[2], efds[2];
  int status = 0;
  int shown_any = 0;
  int lost;
  pid_t pid;

  // on any failure here the client still has to get a response, otherwise it
  // waits forever. the session carries on, the next command may well work
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

  // read while the command runs, a pipe only holds about 64 KB so waiting
  // for the command first would deadlock on large output
  // stdout goes first and then stderr, so errors come after normal output
  // stderr isn't read until stdout closes, so a command that writes more than
  // a pipe holds to stderr while stdout is still open has to wait here
  lost = forward_output(s, ofds[READ_END], 0, &shown_any);

  // the output entry is printed before any error entry, so the two appear
  // in the order the spec's figure shows them
  log_output_end(&s->client);

  if (lost == 0)
    lost = forward_output(s, efds[READ_END], 1, &shown_any);

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
    log_no_output(&s->client);
  log_blank();

  // the empty message ends the response
  return send_msg(s->client.fd, "", 0) == 0 ? 1 : -1;
}


/*
Serves one client until it leaves, then closes its socket and frees its
session. Everything it touches comes from the session it is given, so this
is the whole of one client's work and nothing outside it is involved.

That is deliberate: making the server concurrent is then a matter of
starting this on a thread per client instead of calling it, with no state
to separate first.
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
  moment would take the whole server down and every other client with it.
  send_msg then returns -1 and only that session ends.
  */
  signal(SIGPIPE, SIG_IGN);

  listen_fd = start_listening();
  if (listen_fd == -1)
    return EXIT_FAILURE;

  log_server_started();

  /*
  The server does not stop on its own. A client sending exit or
  disconnecting ends that session only, and this loop goes straight back to
  waiting for the next one, which is what phase 3 asks for.
  */
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

    // one client at a time for now, so it is always thread 1; a thread per
    // client is the next change and this is the number it would report
    log_client_connected(&s->client, 1);

    serve_client(s, listen_fd);
  }

  // not reached: the loop above has no exit. the socket is left to the
  // kernel to reclaim when the server is stopped
}