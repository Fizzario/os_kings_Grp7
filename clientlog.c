/*
  log_lock    the terminal. Held for exactly as long as it takes to write
              one whole entry, so an entry is never cut in half by another
              thread's entry.

  count_lock  the counter that hands out client numbers, so two clients
              connecting at the same moment cannot be given the same one.
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

static int next_id = 1;


// the text that marks an error as a missing command, recognised so the
// name can be quoted on the [ERROR] line
static const char NOT_FOUND[] = "Command not found: ";

/*
The longest one entry can be.
*/
#define ENTRY_MAX (LOG_OUTPUT_MAX + 512)


/*
Writes one finished entry to the terminal and flushes it.
*/
static void emit(const char *text, size_t len) {
  pthread_mutex_lock(&log_lock);
  fwrite(text, 1, len, stdout);
  fflush(stdout);
  pthread_mutex_unlock(&log_lock);
}


/*
Writes "[Client #2 - 192.168.1.101:56790] " into buf, or nothing at all when
c is NULL
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

  // two clients can be accepted at the same moment by different threads
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
  // this line names the client rather than being prefixed with it.
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


void log_blank(void) {
  emit("\n", 1);
}


void log_info(const client_t *c, const char *fmt, ...) {
  char text[ENTRY_MAX];
  va_list ap;

  // the message is formatted first and then handed to line() as a plain
  // string
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

  if (quoted > ENTRY_MAX / 2) quoted = ENTRY_MAX / 2;

  /*
  The [ERROR] line.
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

  // past the limit only the count is kept.
  memcpy(c->out + c->out_len, text, room);
  c->out_len += room;
  c->out_dropped += len - room;
}


void log_output_end(client_t *c) {
  char entry[ENTRY_MAX];
  int n = 0;

  if (!c->out_open) return;
  c->out_open = 0;

  // a command that printed nothing has no output entry at all
  if (c->out_len == 0 && c->out_dropped == 0) return;

  n += snprintf(entry + n, sizeof entry - (size_t) n, "[OUTPUT] ");
  n += prefix(entry + n, sizeof entry - (size_t) n, c);
  n += snprintf(entry + n, sizeof entry - (size_t) n,
                "Sending output to client:\n");

  memcpy(entry + n, c->out, c->out_len);
  n += (int) c->out_len;

  // commands do not always end their output with a newline
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