#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "stream/stream.hpp"

struct BlockRequest {
  std::vector<std::string> keys;
  double timeout;
};

class Store {
  std::map<std::string, std::string> Storage;
  std::map<std::string, std::chrono::steady_clock::time_point> Expirations;
  std::unordered_map<std::string, std::vector<std::string>> DynamicVector;
  std::optional<BlockRequest> pending_block_;
  std::unordered_map<std::string, Stream> Streams;

public:
  std::string handle_set(const std::vector<std::string> &args);
  std::string handle_get(const std::vector<std::string> &args);
  bool is_expired(const std::string &key);
  std::string handle_rpush(const std::vector<std::string> &args);
  std::string handle_lrange(const std::vector<std::string> &args);
  std::string handle_lpush(const std::vector<std::string> &args);
  std::string handle_llen(const std::vector<std::string> &args);
  std::string handle_lpop(const std::vector<std::string> &args);
  std::string handle_blpop(const std::vector<std::string> &args);
  std::string handle_type(const std::vector<std::string> &args);
  std::string handle_xadd(const std::vector<std::string> &args);
  std::string handle_xrange(const std::vector<std::string> &args);

  std::optional<std::pair<std::string, std::string>>
  try_blpop(const std::vector<std::string> &keys);

  std::optional<BlockRequest> take_pending_block();
};
