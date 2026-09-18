#pragma once

#include "aof/aof.hpp"
#include "config/config.hpp"

class Server {
public:
  explicit Server(Config config);
  ~Server();

  bool start();
  void run();

private:
  void connect_to_master();

  Config config_;
  Aof aof_;
  int server_fd_;
  int master_fd_;
  long long master_initial_offset_;
};
