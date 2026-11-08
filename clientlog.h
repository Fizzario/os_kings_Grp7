/*
clientlog.h : who a client is, and the server's log of what it does

Every client gets a client_t when it connects. It holds the number the log
calls it by, the address it connected from, and its socket, so any part of
the server that has the client can log about it without passing those around
separately.

The log itself is the reason this is its own unit. With one thread per
client, several threads write to the same terminal at once, and printf gives
no guarantee beyond a single call: a command's output printed line by line
would end up cut in half by another client's log. Every function here writes
one whole entry while holding one mutex, so an entry can never be split by
another thread, and the terminal reads in the order the spec's figure shows.

The exact wording of each line lives here and nowhere else. The format is
part of what phase 3 is marked on, so there is one definition of it rather
than a printf in each place that needs one.
*/

#ifndef CLIENTLOG_H
#define CLIENTLOG_H

#include <stddef.h>      // size_t
#include <netinet/in.h>  // struct sockaddr_in, INET_ADDRSTRLEN

/*
How much of a command's output is kept to put in the log.

The output sent to the client is not limited: it streams straight through as
it arrives. This only bounds the copy the log keeps, so that one ls -lR does
not hold megabytes per client waiting to be printed. Past this the entry
notes how much was left out.
*/
#define LOG_OUTPUT_MAX 8192


// one connected client: what the log calls it, and where it came from
typedef struct {
  int  id;                      // the N in "Client #N", 1 for the first
  int  fd;                      // the socket, owned by the thread serving it
  char ip[INET_ADDRSTRLEN];     // the address it connected from, as text
  int  port;                    // the port it connected from

  /*
  Scratch for building one [OUTPUT] entry. A command's output arrives in
  pieces and the whole entry has to be printed at once, so the pieces are
  collected here and printed when the command finishes. It lives in the
  client rather than in the log so that each thread has its own.
  */
  char   out[LOG_OUTPUT_MAX];
  size_t out_len;
  size_t out_dropped;           // bytes past LOG_OUTPUT_MAX, 0 if none
  int    out_open;              // 1 between log_output_begin and _end
} client_t;


/*
Fills c in for a client that has just been accepted: takes the next free
client number, and converts the address accept() gave into text.

The number is handed out under a lock, so two clients connecting at the same
moment cannot be given the same one. Returns 0, or -1 if the address could
not be converted, in which case the client is still usable and its address
reads as "unknown".
*/
int client_init(client_t *c, int fd, const struct sockaddr_in *addr);


/*
The lines the server prints, one function per line in the spec's figure.
Each one writes its whole entry while holding the log's mutex.

Those that take a client print its name and address in front of the message,
as "[TAG] [Client #2 - 192.168.1.101:56790] ...". The two that do not take
one, log_server_started and log_client_disconnected, are the lines the
figure shows without it.
*/

// [INFO] Server started, waiting for client connections...
void log_server_started(void);

// [INFO] Client #1 connected from 192.168.1.100:56789. Assigned to Thread-1.
void log_client_connected(const client_t *c, int thread_no);

// [RECEIVED] [Client #1 - ...] Received command: "ls -l"
void log_received(const client_t *c, const char *cmd);

// [EXECUTING] [Client #1 - ...] Executing command: "ls -l"
void log_executing(const client_t *c, const char *cmd);

/*
Writes the two lines an error produces: the [ERROR] line naming what went
wrong, then the [OUTPUT] line saying it is being sent on to the client.

text is the error as the command or the shell wrote it, len its length. A
trailing newline is left out of the quoted parts. When the text is a missing
command the name is quoted on the [ERROR] line, matching the figure.
*/
void log_error(const client_t *c, const char *text, size_t len);

// [OUTPUT] [Client #1 - ...] Sending no output to client.
void log_no_output(const client_t *c);

// [INFO] [Client #3 - ...] Client requested disconnect. Closing connection.
void log_disconnect_requested(const client_t *c);

// [INFO] Client #3 disconnected.
void log_client_disconnected(const client_t *c);

/*
Any other [INFO] line, for cases the figure does not show: a thread that
could not be started, a receive that failed. Printed with the client's name
in front when c is given, without it when c is NULL.
*/
void log_info(const client_t *c, const char *fmt, ...);


/*
The [OUTPUT] entry for a command that produced output.

It is built in three calls because the output arrives in pieces and the
whole entry has to be printed at once:

  log_output_begin(c)             before reading the command's output
  log_output_chunk(c, buf, n)     for each piece, as it arrives
  log_output_end(c)               once the command has finished

Only log_output_end takes the mutex, so a slow command does not hold the log
against the other threads while it runs. If no chunk was ever added the
entry is dropped, so a command that printed nothing logs nothing here and
the caller uses log_no_output instead.
*/
void log_output_begin(client_t *c);
void log_output_chunk(client_t *c, const char *text, size_t len);
void log_output_end(client_t *c);

#endif