/*
protocol.h : how the client and the server frame messages on the socket

TCP is a byte stream with no message boundaries: one read can return half a
command, or two of them stuck together, and the output of a command can
arrive in several pieces. Neither side could tell where a message ends.

So every message is sent as a 4 byte length followed by that many bytes of
payload. The length is in network byte order, so both ends agree on it
whatever the machine. The receiver reads the 4 bytes, learns the size, and
then reads exactly that many.

Both the client and the server link this, and neither should read or write
the socket directly.


A command is one message. A response is a sequence: zero or more messages
carrying output, then one empty message to mark the end.

The terminator is what lets a response be any size. The output of ls -l
/usr/bin is around 100 KB and ls -lR /usr/share is several megabytes, so a
response does not fit in one message and the server has to send it in
pieces. Without an end marker the client could not tell a pause between
pieces from the end of the output, and would block forever waiting for more.

A command that printed nothing sends only the terminator, which is also how
the client knows the command finished rather than produced no reply.
*/

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>   // size_t

#define SERVER_IP   "127.0.0.1" // the server runs on the same machine
#define SERVER_PORT 8080        // port the server listens on

// largest payload in one message, so a bad length cannot ask for any amount
// of memory; command lines are short, command output is what needs the room
#define MAX_MSG 65536


/*
sends one message: the 4 byte length, then len bytes of data
len may be 0, which sends an empty message
returns 0 on success, -1 on failure
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