#pragma once

#include <string>

struct Config {
  int port = 6379;
  bool is_replica = false;
  std::string master_host;
  int master_port = 0;
  std::string dir;
  std::string dbfilename;
};

Config parse_args(int argc, char *argv[]);
