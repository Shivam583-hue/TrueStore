#pragma once

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

class Aof {
public:
  Aof() = default;
  ~Aof();
  Aof(const Aof &) = delete;
  Aof &operator=(const Aof &) = delete;

  void open(const Config &config);
  void append(const std::vector<std::string> &args);
  void append_transaction(const std::vector<std::vector<std::string>> &commands);
  void sync_if_due();
  int sync_timeout_ms() const;

private:
  void write(const std::string &bytes);

  int fd_ = -1;
  std::vector<std::filesystem::path> files_;
  std::string fsync_;
  bool dirty_ = false;
  std::chrono::steady_clock::time_point last_sync_;
};
