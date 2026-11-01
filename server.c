/*
server.c : the remote side of the shell

listens on a TCP port and waits for one client
*/

#include <stdio.h>      // printf(), perror(), fflush()
#include <stdlib.h>     // EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>     // memset()
#include <unistd.h>     // close()
#include <arpa/inet.h>  // socket(), bind(), listen(), accept(), htons(), htonl()
#include "protocol.h"   // SERVER_PORT


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

  close(conn_fd);
  close(listen_fd);
  return EXIT_SUCCESS;
}
