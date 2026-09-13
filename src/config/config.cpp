#include "config/config.hpp"

#include <cstdlib>
#include <cstring>

Config parse_args(int argc, char *argv[]) {
  Config config;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      config.port = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
      config.dir = argv[i + 1];
      ++i;
    } else if (std::strcmp(argv[i], "--dbfilename") == 0 && i + 1 < argc) {
      config.dbfilename = argv[i + 1];
      ++i;
    } else if (std::strcmp(argv[i], "--replicaof") == 0 && i + 1 < argc) {
      config.is_replica = true;

      std::string arg = argv[i + 1];
      std::size_t space = arg.find(' ');

      if (space != std::string::npos) {
        config.master_host = arg.substr(0, space);
        config.master_port = std::atoi(arg.substr(space + 1).c_str());
        ++i;
      } else if (i + 2 < argc) {
        config.master_host = arg;
        config.master_port = std::atoi(argv[i + 2]);
        i += 2;
      } else {
        config.master_host = arg;
        ++i;
      }
    }
  }

  return config;
}
