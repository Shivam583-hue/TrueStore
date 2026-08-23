#include "server.hpp"

#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

Server::Server(int port) : port_(port), server_fd_(-1) {}

Server::~Server() {
  if (server_fd_ >= 0) {
    close(server_fd_);
  }
}

bool Server::start() {
  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    std::cerr << "Failed to create server socket\n";
    return false;
  }

  int reuse = 1;
  if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    std::cerr << "setsockopt failed\n";
    return false;
  }

  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port_);

  if (bind(server_fd_, reinterpret_cast<struct sockaddr *>(&server_addr),
           sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port " << port_ << "\n";
    return false;
  }

  int connection_backlog = 5;
  if (listen(server_fd_, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return false;
  }

  return true;
}

void Server::run() {
  struct sockaddr_in client_addr{};
  socklen_t client_addr_len = sizeof(client_addr);

  int client_fd =
      accept(server_fd_, reinterpret_cast<struct sockaddr *>(&client_addr),
             &client_addr_len);
  if (client_fd < 0) {
    std::cerr << "Error: Accept failed\n";
    return;
  }
  // std::cout << "Client connected\n";

  char buffer[1024] = {0};
  while (1) {
    ssize_t bytes_received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received > 0) {
      // std::cout << "Client says: " << buffer << "\n";
      std::cout << buffer << "\n";
    } else
      break;

    const char *response = "+PONG\r\n";
    send(client_fd, response, std::strlen(response), 0);
  }

  close(client_fd);
}
