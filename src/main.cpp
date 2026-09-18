#include "config/config.hpp"
#include "server/server.hpp"

#include <exception>
#include <iostream>

int main(int argc, char *argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  try {
    Server server(parse_args(argc, argv));
    if (!server.start()) {
      return 1;
    }

    server.run();
  } catch (const std::exception &e) {
    std::cerr << "Server error: " << e.what() << '\n';
    return 1;
  }

  return 0;
}
