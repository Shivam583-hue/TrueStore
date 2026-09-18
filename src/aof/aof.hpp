#pragma once

#include "config/config.hpp"

#include <filesystem>
#include <vector>

class Aof {
public:
  Aof() = default;
  ~Aof();
  Aof(const Aof &) = delete;
  Aof &operator=(const Aof &) = delete;

  void open(const Config &config);

private:
  int fd_ = -1;
  std::vector<std::filesystem::path> files_;
};
