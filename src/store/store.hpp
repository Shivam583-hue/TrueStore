#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "stream/stream.hpp"

enum class BlockKind {
  List,
  Stream,
  Wait,
};

struct BlockRequest {
  BlockKind kind;
  std::vector<std::string> keys;
  std::vector<StreamID> ids;
  std::size_t count;
  double timeout;
  long long target_offset = 0;
};

class Store {
  std::map<std::string, std::string> Storage;
  std::map<std::string, std::chrono::steady_clock::time_point> Expirations;
  std::unordered_map<std::string, std::vector<std::string>> DynamicVector;
  std::optional<BlockRequest> pending_block_;
  std::vector<std::vector<std::string>> pending_propagations_;
  std::unordered_map<std::string, Stream> Streams;

  bool is_replica_ = false;
  std::string master_host_;
  int master_port_ = 0;
  std::string master_replid_;
  long long master_repl_offset_ = 0;

public:
  void init(bool is_replica, std::string master_host, int master_port);

  std::string handle_replconf(const std::vector<std::string> &args);
  std::string handle_psync(const std::vector<std::string> &args);
  std::string handle_wait(const std::vector<std::string> &args);

  long long repl_offset() const { return master_repl_offset_; }
  void bump_repl_offset(long long bytes) { master_repl_offset_ += bytes; }

  void queue_propagation(std::vector<std::string> args) {
    pending_propagations_.push_back(std::move(args));
  }

  std::vector<std::vector<std::string>> take_pending_propagations() {
    return std::exchange(pending_propagations_, {});
  }

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
  std::string handle_xread(const std::vector<std::string> &args);
  std::string handle_incr(const std::vector<std::string> &args);
  std::string handle_info(const std::vector<std::string> &args);

  std::optional<std::string> peek(const std::string &key);

  std::optional<std::pair<std::string, std::string>>
  try_blpop(const std::vector<std::string> &keys);

  std::optional<std::string> try_xread(const std::vector<std::string> &keys,
                                       const std::vector<StreamID> &ids,
                                       std::size_t count);

  std::optional<BlockRequest> take_pending_block();
};
