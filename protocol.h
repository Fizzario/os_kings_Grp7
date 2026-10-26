/*
protocol.h : how the client and the server frame messages on the socket
*/

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>   // size_t

#define SERVER_IP   "127.0.0.1" // the server runs on the same machine
#define SERVER_PORT 8080        // port the server listens on

// largest payload in one message
#define MAX_MSG 65536


/*
sends one message: the 4 byte length, then len bytes of data
*/
int send_msg(int fd, const char *data, size_t len);


/*
receives one whole message into buf and writes its size to len
buf is always terminated, so the caller can use it as a string, which means
bufsize has to be at least one byte larger than the biggest payload expected

returns  1 when a whole message arrived
         0 when the peer closed the connection before the message started,
           which is the normal way a session ends
        -1 on a failure, including a peer that closed midway through a
           message and a length that does not fit in buf
*/
int recv_msg(int fd, char *buf, size_t bufsize, size_t *len);

#endif