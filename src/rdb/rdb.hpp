#pragma once

#include <string>
#include <vector>

struct RdbEntry {
  std::string key;
  std::string value;
};

std::vector<RdbEntry> load_rdb(const std::string &path);
