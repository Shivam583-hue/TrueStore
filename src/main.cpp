#include "server/server.hpp"

#include <iostream>

int main(int argc, char *argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  int port = 6379;
  if (argc > 2) {
    port = std::atoi(argv[2]);
  }

  Server server(port);
  if (!server.start()) {
    return 1;
  }

  server.run();

  return 0;
}
