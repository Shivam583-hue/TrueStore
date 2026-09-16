#pragma once

#include <filesystem>
#include <string>

struct Config {
  int port = 6379;
  bool is_replica = false;
  std::string master_host;
  int master_port = 0;
  std::string dir = std::filesystem::current_path().string();
  std::string dbfilename;

  std::string appendonly = "no";
  std::string appenddirname = "appendonlydir";
  std::string appendfilename = "appendonly.aof";
  std::string appendfsync = "everysec";
};

Config parse_args(int argc, char *argv[]);
