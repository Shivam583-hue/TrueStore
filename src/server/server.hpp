#pragma once

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
  int server_fd_;
  int master_fd_;
  long long master_initial_offset_;
};
