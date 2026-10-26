/*
protocol.c : the length prefixed framing described in protocol.h
*/

#include <errno.h>      // errno, EINTR
#include <stdint.h>     // uint32_t
#include <unistd.h>     // read(), write()
#include <arpa/inet.h>  // htonl(), ntohl()
#include "protocol.h"


/*
writes exactly len bytes, however many calls that takes
a signal can interrupt the call before anything moves, which is not a
failure, so EINTR just retries
returns 0 when all of it went out, -1 on failure
*/
static int send_all(int fd, const char *buf, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    ssize_t n = write(fd, buf + sent, len - sent);

    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }

    if (n == 0) return -1;   // wrote nothing and did not fail, treat as dead

    sent += (size_t) n;
  }

  return 0;
}


/*
reads exactly len bytes, however many calls that takes
returns 1 when all of it arrived, 0 when the peer closed the connection
before sending it all, -1 on failure
*/
static int recv_all(int fd, char *buf, size_t len) {
  size_t got = 0;

  while (got < len) {
    ssize_t n = read(fd, buf + got, len - got);

    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }

    if (n == 0) return 0;    // peer closed

    got += (size_t) n;
  }

  return 1;
}


// sends the 4 byte length then the payload, see protocol.h
int send_msg(int fd, const char *data, size_t len) {
  uint32_t header;

  if (len > MAX_MSG) return -1;

  // htonl puts the length in network byte order so both ends read it same
  header = htonl((uint32_t) len);

  if (send_all(fd, (const char *) &header, sizeof header) != 0) return -1;

  // an empty message is just the header
  if (len > 0 && send_all(fd, data, len) != 0) return -1;

  return 0;
}


// reads the 4 byte length then exactly that many bytes, see protocol.h
int recv_msg(int fd, char *buf, size_t bufsize, size_t *len) {
  uint32_t header;
  size_t payload;
  int r;

  r = recv_all(fd, (char *) &header, sizeof header);

  // nothing arrived at all: which is how a session normally ends
  if (r <= 0) return r;

  payload = (size_t) ntohl(header);

  // refuse a length that will not fit, leaving room for the terminator
  if (payload > MAX_MSG || payload >= bufsize) return -1;

  if (payload > 0) {
    r = recv_all(fd, buf, payload);

    if (r <= 0) return -1;
  }

  // terminate so the caller can treat the payload as a string
  buf[payload] = '\0';
  *len = payload;

  return 1;
}