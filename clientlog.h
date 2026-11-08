/*
clientlog.h : who a client is, and the server's log of what it does
*/

#ifndef CLIENTLOG_H
#define CLIENTLOG_H

#include <stddef.h>      // size_t
#include <netinet/in.h>  // struct sockaddr_in, INET_ADDRSTRLEN

/*
How much of a command's output is kept to put in the log.
*/
#define LOG_OUTPUT_MAX 8192


// one connected client: what the log calls it, and where it came from
typedef struct {
  int  id;                      // the N in "Client #N", 1 for the first
  int  fd;                      // the socket, owned by the thread serving it
  char ip[INET_ADDRSTRLEN];     // the address it connected from, as text
  int  port;                    // the port it connected from

  /*
  Scratch for building one [OUTPUT] entry.
  */
  char   out[LOG_OUTPUT_MAX];
  size_t out_len;
  size_t out_dropped;           // bytes past LOG_OUTPUT_MAX, 0 if none
  int    out_open;              // 1 between log_output_begin and _end
} client_t;


/*
Fills c in for a client that has just been accepted: takes the next free
client number, and converts the address accept() gave into text
*/
int client_init(client_t *c, int fd, const struct sockaddr_in *addr);


/*
The lines the server prints, one function per line in the spec's figure.
Each one writes its whole entry while holding the log's mutex.
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
*/
void log_error(const client_t *c, const char *text, size_t len);

// [OUTPUT] [Client #1 - ...] Sending no output to client.
void log_no_output(const client_t *c);

// [INFO] [Client #3 - ...] Client requested disconnect. Closing connection.
void log_disconnect_requested(const client_t *c);

// [INFO] Client #3 disconnected.
void log_client_disconnected(const client_t *c);

/*
A blank line, which the figure uses to separate one command's entries from
the next.
*/
void log_blank(void);


/*
Any other [INFO] line, for cases the figure does not show: a thread that
could not be started, a receive that failed. Printed with the client's name
in front when c is given, without it when c is NULL.
*/
void log_info(const client_t *c, const char *fmt, ...);


/*
The [OUTPUT] entry for a command that produced output.
*/
void log_output_begin(client_t *c);
void log_output_chunk(client_t *c, const char *text, size_t len);
void log_output_end(client_t *c);

#endif