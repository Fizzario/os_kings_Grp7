/*
clientlog.h : who a connected client is

In phase 3 this held the server's log as well, which is where the name
comes from. Phase 4 requires a different format, denser and prefixed with
the client's number rather than its address, and that lives in tasklog.c.
What is left here is the client itself: the number the log calls it by, its
socket, and where it connected from.

The scratch space for building one output entry has gone with the code that
used it. Output is now read by the runner on the scheduler's thread and
passed straight to the client, so nothing collects it per client any more.
*/

#ifndef CLIENTLOG_H
#define CLIENTLOG_H

#include <netinet/in.h>  // struct sockaddr_in, INET_ADDRSTRLEN


// one connected client
typedef struct {
  int  id;                    // the N in "[N]", 1 for the first client
  int  fd;                    // the socket, owned by the thread serving it
  char ip[INET_ADDRSTRLEN];   // the address it connected from, as text
  int  port;                  // the port it connected from
} client_t;


/*
Fills c in for a client that has just been accepted: takes the next free
client number, and converts the address accept() gave into text.

Returns 0, or -1 if the address could not be read, in which case the client
is still perfectly usable and only its address is unknown.
*/
int client_init(client_t *c, int fd, const struct sockaddr_in *addr);

#endif