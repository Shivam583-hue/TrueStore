#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct RdbEntry {
  std::string key;
  std::string value;
  std::optional<std::uint64_t> expire_at_ms;
};

std::vector<RdbEntry> load_rdb(const std::string &path);
