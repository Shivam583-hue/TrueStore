#include "client/client.hpp"

#include <iostream>

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  Client client(6379);
  if (!client.start()) {
    return 1;
  }

  client.run();

  return 0;
}
