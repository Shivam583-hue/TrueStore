#pragma once

#include <string>

class Server {
public:
  explicit Server(int port, bool is_replica, std::string master_host = "",
                  int master_port = 0);
  ~Server();

  bool start();
  void run();

private:
  void connect_to_master();

  int port_;
  int server_fd_;
  bool is_replica_;
  std::string master_host_;
  int master_port_;
  int master_fd_;
};
