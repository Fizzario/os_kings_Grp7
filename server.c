/*
server.c : the remote side of the shell

listens on a TCP port, accepts one client and reads its commands
stops when the client sends exit or disconnects
*/

#include <stdio.h>      // printf(), perror(), fflush()
#include <stdlib.h>     // EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>     // memset(), strcmp()
#include <unistd.h>     // close()
#include <arpa/inet.h>  // socket(), bind(), listen(), accept(), htons(), htonl()
#include "protocol.h"   // send_msg(), recv_msg(), SERVER_PORT, MAX_MSG

// one byte larger than the biggest message so recv_msg can terminate it
static char msg[MAX_MSG + 1];


// creates the socket, binds it to SERVER_PORT and starts listening
// returns the listening socket, or -1 after printing what failed
static int start_listening(void) {
  int fd, yes = 1;
  struct sockaddr_in addr;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return -1;
  }

  // without this bind fails for a while after the server is restarted
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr *) &addr, sizeof addr) == -1) {
    perror("bind");
    close(fd);
    return -1;
  }

  // single client model, so the backlog can stay at 1
  if (listen(fd, 1) == -1) {
    perror("listen");
    close(fd);
    return -1;
  }

  return fd;
}


int main(void) {
  int listen_fd, conn_fd;
  int status = EXIT_SUCCESS;
  size_t len;

  listen_fd = start_listening();
  if (listen_fd == -1)
    return EXIT_FAILURE;

  printf("[INFO] Server started, waiting for client connections...\n");
  fflush(stdout); // a log going to a file wouldn't show this until much later

  conn_fd = accept(listen_fd, NULL, NULL);
  if (conn_fd == -1) {
    perror("accept");
    close(listen_fd);
    return EXIT_FAILURE;
  }

  printf("[INFO] Client connected.\n");

  while (1) {
    int r = recv_msg(conn_fd, msg, sizeof msg, &len);

    if (r == 0) {
      printf("[INFO] Client disconnected, shutting down.\n");
      break;
    }
    if (r < 0) {
      printf("[ERROR] Receive failed, shutting down.\n");
      status = EXIT_FAILURE;
      break;
    }

    printf("[RECEIVED] Received command: \"%s\" from client.\n", msg);

    if (strcmp(msg, "exit") == 0) {
      printf("[INFO] Client sent exit, shutting down.\n");
      break;
    }

    // nothing runs yet, the empty message just lets the client prompt again
    if (send_msg(conn_fd, "", 0) != 0) {
      printf("[INFO] Client disconnected, shutting down.\n");
      break;
    }
  }

  close(conn_fd);
  close(listen_fd);
  return status;
}
