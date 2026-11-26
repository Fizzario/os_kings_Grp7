/*
clientlog.c : giving a newly accepted client its number and its address

One lock, over the counter that hands out client numbers, because two
clients can be accepted at the same moment by different threads and must
not be given the same number.

The log itself used to be here. Phase 4 requires a different format and it
lives in tasklog.c, with the lock that keeps one of its lines from being cut
in half by another thread's.
*/

#include <stdio.h>       // snprintf()
#include <pthread.h>     // pthread_mutex_t and friends
#include <arpa/inet.h>   // inet_ntop(), ntohs()
#include "clientlog.h"

// guards next_id below
static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;

static int next_id = 1;


int client_init(client_t *c, int fd, const struct sockaddr_in *addr) {
  c->fd = fd;

  // two clients can be accepted at the same moment by different threads, so
  // taking a number has to be one indivisible step
  pthread_mutex_lock(&count_lock);
  c->id = next_id++;
  pthread_mutex_unlock(&count_lock);

  c->port = (int) ntohs(addr->sin_port);

  // the address arrives as a number and is kept as text
  if (inet_ntop(AF_INET, &addr->sin_addr, c->ip, sizeof c->ip) == NULL) {
    // the client is still perfectly usable, only its address is unknown
    snprintf(c->ip, sizeof c->ip, "unknown");
    c->port = 0;
    return -1;
  }

  return 0;
}