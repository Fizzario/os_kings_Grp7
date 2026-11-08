/*
clientlog.c : the per-client log described in clientlog.h

Two locks, each guarding one thing:

  log_lock    the terminal. Held for exactly as long as it takes to write
              one whole entry, so an entry is never cut in half by another
              thread's entry.

  count_lock  the counter that hands out client numbers, so two clients
              connecting at the same moment cannot be given the same one.

They are separate because they protect unrelated things and are never held
at the same time, which also means they cannot deadlock against each other.

Every entry is built in a local buffer first and then written with one
fwrite while the lock is held. Building the text outside the lock keeps the
lock held for as little as possible, and one fwrite rather than several
printf calls means there is only one place the write can be interleaved,
which the lock then rules out.
*/

#include <stdarg.h>      // va_list, va_start, va_end
#include <stdio.h>       // snprintf(), vsnprintf(), fwrite(), fflush()
#include <string.h>      // strncmp(), memcpy(), strlen()
#include <pthread.h>     // pthread_mutex_t and friends
#include <arpa/inet.h>   // inet_ntop(), ntohs()
#include "clientlog.h"

// guards the terminal, so one entry is written whole
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

// guards next_id below
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

// the number the next client to connect is given
static int next_id = 1;

/*
The text in front of an error that means the command did not exist. The
figure quotes the command's name on the [ERROR] line for this one error and
prints every other error as it came, so it has to be recognised by its text.
The executor produces it, with the name appended, when the server asks for
the name to be reported.
*/
static const char NOT_FOUND[] = "Command not found: ";

/*
The longest one entry can be. The output entry is the big one: its header,
up to LOG_OUTPUT_MAX of the command's output, and a note if any was left
out. The rest is room for the header and the blank line after it.
*/
#define ENTRY_MAX (LOG_OUTPUT_MAX + 512)


/*
Writes one finished entry to the terminal and flushes it.

The flush matters for two reasons. The log is read as the server runs, so an
entry sitting in the buffer is an entry nobody can see yet; and when the log
is redirected to a file stdout is fully buffered, so without it the entries
of several clients would reach the file in whatever order their buffers
happened to fill, not the order they happened.
*/
static void emit(const char *text, size_t len) {
  pthread_mutex_lock(&log_lock);
  fwrite(text, 1, len, stdout);
  fflush(stdout);
  pthread_mutex_unlock(&log_lock);
}


/*
Writes "[Client #2 - 192.168.1.101:56790] " into buf, or nothing at all when
c is NULL, which is how the two lines that carry no client are printed.
Returns how many characters it wrote.
*/
static int prefix(char *buf, size_t size, const client_t *c) {
  if (c == NULL) return 0;
  return snprintf(buf, size, "[Client #%d - %s:%d] ", c->id, c->ip, c->port);
}


/*
Builds a one line entry, "[TAG] [Client #N - ip:port] message", and writes
it. Used by every single line entry below, so the shape of a line is
defined once.
*/
static void line(const client_t *c, const char *tag, const char *fmt, ...) {
  char entry[ENTRY_MAX];
  va_list ap;
  int n;

  n = snprintf(entry, sizeof entry, "[%s] ", tag);
  n += prefix(entry + n, sizeof entry - (size_t) n, c);

  va_start(ap, fmt);
  n += vsnprintf(entry + n, sizeof entry - (size_t) n, fmt, ap);
  va_end(ap);

  // snprintf reports what it would have written, so a long command could
  // push n past the buffer; clamp before adding the newline
  if (n > (int) sizeof entry - 2) n = (int) sizeof entry - 2;

  entry[n++] = '\n';
  emit(entry, (size_t) n);
}


int client_init(client_t *c, int fd, const struct sockaddr_in *addr) {
  c->fd = fd;
  c->out_len = 0;
  c->out_dropped = 0;
  c->out_open = 0;

  // two clients can be accepted at the same moment by different threads, so
  // taking a number has to be one indivisible step
  pthread_mutex_lock(&count_lock);
  c->id = next_id++;
  pthread_mutex_unlock(&count_lock);

  c->port = (int) ntohs(addr->sin_port);

  // the address arrives as a number and the log shows it as text
  if (inet_ntop(AF_INET, &addr->sin_addr, c->ip, sizeof c->ip) == NULL) {
    // the client is still perfectly usable, only its address is unknown
    snprintf(c->ip, sizeof c->ip, "unknown");
    c->port = 0;
    return -1;
  }

  return 0;
}


void log_server_started(void) {
  line(NULL, "INFO", "Server started, waiting for client connections...");
}


void log_client_connected(const client_t *c, int thread_no) {
  // this line names the client rather than being prefixed with it, which is
  // why it is built from c's fields instead of going through prefix()
  line(NULL, "INFO", "Client #%d connected from %s:%d. Assigned to Thread-%d.",
       c->id, c->ip, c->port, thread_no);
}


void log_received(const client_t *c, const char *cmd) {
  line(c, "RECEIVED", "Received command: \"%s\"", cmd);
}


void log_executing(const client_t *c, const char *cmd) {
  line(c, "EXECUTING", "Executing command: \"%s\"", cmd);
}


void log_no_output(const client_t *c) {
  line(c, "OUTPUT", "Sending no output to client.");
}


void log_disconnect_requested(const client_t *c) {
  line(c, "INFO", "Client requested disconnect. Closing connection.");
}


void log_client_disconnected(const client_t *c) {
  // the figure shows this one without the bracketed prefix, like the
  // connection line, so it names the client in the message instead
  line(NULL, "INFO", "Client #%d disconnected.", c->id);
}


void log_info(const client_t *c, const char *fmt, ...) {
  char text[ENTRY_MAX];
  va_list ap;

  // the message is formatted first and then handed to line() as a plain
  // string, because line() takes its own variable arguments and one va_list
  // cannot be passed through to it
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);

  line(c, "INFO", "%s", text);
}


void log_error(const client_t *c, const char *text, size_t len) {
  char entry[ENTRY_MAX];
  size_t quoted = len;
  size_t plen = sizeof NOT_FOUND - 1;
  int n = 0;

  // the newline belongs in what goes to the client but not inside quotes
  while (quoted > 0 && text[quoted - 1] == '\n')
    quoted--;

  // keep the error inside one entry even if a command wrote a great deal to
  // stderr; the client still gets all of it, this only bounds the log
  if (quoted > ENTRY_MAX / 2) quoted = ENTRY_MAX / 2;

  /*
  The [ERROR] line. A missing command is the one error the figure shows
  quoted by name, so its text is recognised and the name pulled out of it;
  everything else is printed exactly as the command wrote it.
  */
  n += snprintf(entry + n, sizeof entry - (size_t) n, "[ERROR] ");
  n += prefix(entry + n, sizeof entry - (size_t) n, c);

  if (quoted > plen && strncmp(text, NOT_FOUND, plen) == 0)
    n += snprintf(entry + n, sizeof entry - (size_t) n,
                  "Command not found: \"%.*s\"\n",
                  (int) (quoted - plen), text + plen);
  else
    n += snprintf(entry + n, sizeof entry - (size_t) n,
                  "%.*s\n", (int) quoted, text);

  // the [OUTPUT] line, which quotes the message as the client will see it
  n += snprintf(entry + n, sizeof entry - (size_t) n, "[OUTPUT] ");
  n += prefix(entry + n, sizeof entry - (size_t) n, c);
  n += snprintf(entry + n, sizeof entry - (size_t) n,
                "Sending error message to client: \"%.*s\"\n",
                (int) quoted, text);

  if (n > (int) sizeof entry) n = (int) sizeof entry;

  // both lines go out together, so nothing can appear between them
  emit(entry, (size_t) n);
}


void log_output_begin(client_t *c) {
  c->out_len = 0;
  c->out_dropped = 0;
  c->out_open = 1;
}


void log_output_chunk(client_t *c, const char *text, size_t len) {
  size_t room;

  if (!c->out_open) return;

  room = sizeof c->out - c->out_len;

  if (len <= room) {
    memcpy(c->out + c->out_len, text, len);
    c->out_len += len;
    return;
  }

  // past the limit only the count is kept, so the entry can say how much of
  // the output it is not showing
  memcpy(c->out + c->out_len, text, room);
  c->out_len += room;
  c->out_dropped += len - room;
}


void log_output_end(client_t *c) {
  char entry[ENTRY_MAX];
  int n = 0;

  if (!c->out_open) return;
  c->out_open = 0;

  // a command that printed nothing has no output entry at all; the caller
  // logs that case with log_no_output instead
  if (c->out_len == 0 && c->out_dropped == 0) return;

  n += snprintf(entry + n, sizeof entry - (size_t) n, "[OUTPUT] ");
  n += prefix(entry + n, sizeof entry - (size_t) n, c);
  n += snprintf(entry + n, sizeof entry - (size_t) n,
                "Sending output to client:\n");

  memcpy(entry + n, c->out, c->out_len);
  n += (int) c->out_len;

  // commands do not always end their output with a newline, and the next
  // entry has to start on its own line
  if (c->out_len > 0 && c->out[c->out_len - 1] != '\n')
    entry[n++] = '\n';

  if (c->out_dropped > 0)
    n += snprintf(entry + n, sizeof entry - (size_t) n,
                  "[... %zu more bytes sent to the client, not shown here]\n",
                  c->out_dropped);

  emit(entry, (size_t) n);

  c->out_len = 0;
  c->out_dropped = 0;
}