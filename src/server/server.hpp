#pragma once

class Server {
public:
  explicit Server(int port);
  ~Server();

  bool start();
  void run();

private:
  int port_;
  int server_fd_;
};
