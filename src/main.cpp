#include "server/server.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  int port = 6379;
  bool is_replica = false;
  std::string master_host;
  int master_port = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      port = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--replicaof") == 0 && i + 1 < argc) {
      is_replica = true;

      std::string arg = argv[i + 1];
      std::size_t space = arg.find(' ');

      if (space != std::string::npos) {
        master_host = arg.substr(0, space);
        master_port = std::atoi(arg.substr(space + 1).c_str());
        ++i;
      } else if (i + 2 < argc) {
        master_host = arg;
        master_port = std::atoi(argv[i + 2]);
        i += 2;
      } else {
        master_host = arg;
        ++i;
      }
    }
  }

  Server server(port, is_replica, master_host, master_port);
  if (!server.start()) {
    return 1;
  }

  server.run();

  return 0;
}
