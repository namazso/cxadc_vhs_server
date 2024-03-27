#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "http.h"

int main(int argc, char* argv[]) {
  long lport;
  if (argc < 2 || (lport = atol(argv[1])) <= 0 || lport > 0xffff) {
    errno = EINVAL;
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  const uint16_t port = lport;

  struct sockaddr_in server_addr;

  // create server socket
  const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }

  // config socket
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  // bind socket to port
  if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  // listen for connections
  if (listen(server_fd, 10) < 0) {
    perror("listen failed");
    exit(EXIT_FAILURE);
  }

  printf("server listening on port %u\n", (unsigned)port);
  while (1) {
    // client info
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // accept client connection
    const int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);

    if (client_fd < 0) {
      perror("accept failed");
      continue;
    }

    // create a new thread to handle client request
    pthread_t thread_id;
    int err = 0;
    if ((err = pthread_create(&thread_id, NULL, http_thread, (void*)(intptr_t)client_fd)) != 0) {
      fprintf(stderr, "can't create http thread: %d\n", err);
      continue;
    }
    if ((err = pthread_detach(thread_id)) != 0) {
      fprintf(stderr, "can't detach http thread: %d\n", err);
      continue;
    }
  }

  close(server_fd);
  return 0;
}