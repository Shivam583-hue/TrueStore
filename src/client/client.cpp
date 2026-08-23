#include "client.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

Client::Client(int port) : port_(port), client_fd_(-1) {}

Client::~Client() {
  if (client_fd_ >= 0) {
    close(client_fd_);
  }
}

bool Client::start() {
  client_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (client_fd_ < 0) {
    std::cerr << "Failed to create client socket\n";
    return false;
  }

  struct sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_);
  if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
    std::cerr << "Invalid server address\n";
    return false;
  }

  if (connect(client_fd_, reinterpret_cast<struct sockaddr *>(&server_addr),
              sizeof(server_addr)) < 0) {
    std::cerr << "Error: connection failed\n";
    return false;
  }
  // std::cout << "Connected to the server successfully!\n";

  return true;
}

void Client::run() {
  // const char *message = "Hello Server, this is the Client!";
  const char *message = "PING";
  send(client_fd_, message, std::strlen(message), 0);

  char buffer[1024] = {0};
  ssize_t bytes_received = recv(client_fd_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received > 0) {
    std::cout << buffer << "\n";
    // std::cout << "Server response: " << buffer << "\n";
  }
}
