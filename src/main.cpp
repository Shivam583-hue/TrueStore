#include "config/config.hpp"
#include "server/server.hpp"

#include <iostream>

int main(int argc, char *argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  Server server(parse_args(argc, argv));
  if (!server.start()) {
    return 1;
  }

  server.run();

  return 0;
}
