#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "config/config.hpp"
#include "rdb/rdb.hpp"
#include "stream/stream.hpp"

struct ClientState;

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
};

class Store {
  struct SortedSet {
    std::unordered_map<std::string, double> scores;
    std::set<std::pair<double, std::string>> ordered;

    bool add(double score, const std::string &member);
  };

  std::map<std::string, std::string> Storage;
  std::map<std::string, std::chrono::steady_clock::time_point> Expirations;
  std::unordered_map<std::string, std::vector<std::string>> DynamicVector;
  std::optional<BlockRequest> pending_block_;
  std::vector<std::vector<std::string>> pending_propagations_;
  std::unordered_map<std::string, Stream> Streams;
  std::unordered_map<std::string, SortedSet> sorted_sets_;
  std::unordered_map<std::string, std::set<int>> subscribers_;
  std::vector<std::pair<int, std::string>> pending_messages_;

  bool default_user_enabled_ = true;
  bool default_user_nopass_ = true;
  std::vector<std::string> default_user_passwords_;

  bool is_replica_ = false;
  std::string master_host_;
  int master_port_ = 0;
  std::string master_replid_;
  long long master_repl_offset_ = 0;

  std::string dir_;
  std::string dbfilename_;

  std::string appendonly_;
  std::string appenddirname_;
  std::string appendfilename_;
  std::string appendfsync_;

  bool wrong_sorted_set_type(const std::string &key);
  bool wrong_string_type(const std::string &key);

public:
  void init(const Config &config);
  void load_entries(const std::vector<RdbEntry> &entries);

  bool default_user_auto_auth() const {
    return default_user_enabled_ && default_user_nopass_;
  }
  std::string handle_acl(const std::vector<std::string> &args);
  std::string handle_auth(const std::vector<std::string> &args,
                          ClientState &client);

  std::string handle_replconf(const std::vector<std::string> &args);
  std::string handle_psync(const std::vector<std::string> &args);
  std::string handle_wait(const std::vector<std::string> &args);

  long long repl_offset() const { return master_repl_offset_; }
  void bump_repl_offset(long long bytes) { master_repl_offset_ += bytes; }
  void set_repl_offset(long long value) { master_repl_offset_ = value; }

  void queue_propagation(std::vector<std::string> args) {
    pending_propagations_.push_back(std::move(args));
  }

  std::vector<std::vector<std::string>> take_pending_propagations() {
    return std::exchange(pending_propagations_, {});
  }

  std::string handle_set(const std::vector<std::string> &args);
  std::string handle_get(const std::vector<std::string> &args);
  std::string handle_setbit(const std::vector<std::string> &args);
  std::string handle_getbit(const std::vector<std::string> &args);
  bool is_expired(const std::string &key);
  std::string handle_rpush(const std::vector<std::string> &args);
  std::string handle_lrange(const std::vector<std::string> &args);
  std::string handle_lpush(const std::vector<std::string> &args);
  std::string handle_llen(const std::vector<std::string> &args);
  std::string handle_lpop(const std::vector<std::string> &args);
  std::string handle_blpop(const std::vector<std::string> &args);
  std::string handle_type(const std::vector<std::string> &args);
  std::string key_type(const std::string &key);
  std::string handle_keys(const std::vector<std::string> &args);
  std::string handle_xadd(const std::vector<std::string> &args);
  std::string handle_xrange(const std::vector<std::string> &args);
  std::string handle_xread(const std::vector<std::string> &args);
  std::string handle_incr(const std::vector<std::string> &args);
  std::string handle_config_get(const std::vector<std::string> &args);
  std::string handle_info(const std::vector<std::string> &args);
  std::string handle_zadd(const std::vector<std::string> &args);
  std::string handle_zrank(const std::vector<std::string> &args);
  std::string handle_zrange(const std::vector<std::string> &args);
  std::string handle_zcard(const std::vector<std::string> &args);
  std::string handle_zscore(const std::vector<std::string> &args);
  std::string handle_zrem(const std::vector<std::string> &args);
  std::string handle_geoadd(const std::vector<std::string> &args);
  std::string handle_geopos(const std::vector<std::string> &args);
  std::string handle_geodist(const std::vector<std::string> &args);
  std::string handle_geosearch(const std::vector<std::string> &args);
  std::string handle_subscribe(const std::vector<std::string> &args,
                               ClientState &client);
  std::string handle_unsubscribe(const std::vector<std::string> &args,
                                 ClientState &client);
  std::string handle_publish(const std::vector<std::string> &args);
  void remove_subscriber(int fd);
  std::vector<std::pair<int, std::string>> take_pending_messages() {
    return std::exchange(pending_messages_, {});
  }

  std::optional<std::string> peek(const std::string &key);

  std::optional<std::pair<std::string, std::string>>
  try_blpop(const std::vector<std::string> &keys);

  std::optional<std::string> try_xread(const std::vector<std::string> &keys,
                                       const std::vector<StreamID> &ids,
                                       std::size_t count);

  std::optional<BlockRequest> take_pending_block();
};
