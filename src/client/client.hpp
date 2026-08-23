#pragma once

class Client {
public:
  explicit Client(int port);
  ~Client();

  bool start();
  void run();

private:
  int port_;
  int client_fd_;
};
