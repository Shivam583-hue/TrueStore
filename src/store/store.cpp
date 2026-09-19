#include "store/store.hpp"
#include "resp/resp.hpp"

#include <fnmatch.h>

void Store::init(const Config &config) {
  dir_ = config.dir;
  dbfilename_ = config.dbfilename;
  appendonly_ = config.appendonly;
  appenddirname_ = config.appenddirname;
  appendfilename_ = config.appendfilename;
  appendfsync_ = config.appendfsync;
  init_replication(config);
}

void Store::load_entries(const std::vector<RdbEntry> &entries) {
  const auto system_now = std::chrono::system_clock::now();
  const auto steady_now = std::chrono::steady_clock::now();

  for (const RdbEntry &entry : entries) {
    if (entry.expire_at_ms) {
      const std::chrono::system_clock::time_point expire_at{
          std::chrono::milliseconds(*entry.expire_at_ms)};

      if (expire_at <= system_now) {
        continue;
      }

      Expirations[entry.key] =
          steady_now +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              expire_at - system_now);
    }

    Storage[entry.key] = entry.value;
  }
}

std::string Store::handle_keys(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'keys' command")
        .to_bytes();
  }

  std::vector<std::string> candidates;

  for (const auto &[key, value] : Storage) {
    candidates.push_back(key);
  }

  for (const auto &[key, list] : DynamicVector) {
    candidates.push_back(key);
  }

  for (const auto &[key, stream] : Streams) {
    candidates.push_back(key);
  }

  for (const auto &[key, set] : sorted_sets_) {
    candidates.push_back(key);
  }

  std::vector<std::string> matched;

  for (const std::string &key : candidates) {
    if (is_expired(key)) {
      continue;
    }

    if (fnmatch(args[1].c_str(), key.c_str(), 0) == 0) {
      matched.push_back(key);
    }
  }

  return RespType::Array(std::move(matched)).to_bytes();
}

bool Store::is_expired(const std::string &key) {
  auto it = Expirations.find(key);

  if (it == Expirations.end()) {
    return false;
  }

  if (std::chrono::steady_clock::now() >= it->second) {
    Expirations.erase(it);
    Storage.erase(key);
    return true;
  }

  return false;
}

std::optional<std::string> Store::peek(const std::string &key) {
  if (is_expired(key)) {
    return std::nullopt;
  }

  auto it = Storage.find(key);

  if (it == Storage.end()) {
    return std::nullopt;
  }

  return it->second;
}

std::optional<BlockRequest> Store::take_pending_block() {
  std::optional<BlockRequest> block = std::move(pending_block_);
  pending_block_.reset();
  return block;
}

std::string Store::handle_type(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'type' command")
        .to_bytes();
  }

  return RespType::SimpleString(key_type(args[1])).to_bytes();
}

std::string Store::key_type(const std::string &key) {
  if (is_expired(key)) {
    return "none";
  }
  if (Storage.contains(key)) {
    return "string";
  }
  if (DynamicVector.contains(key)) {
    return "list";
  }
  if (Streams.contains(key)) {
    return "stream";
  }
  return sorted_sets_.contains(key) ? "zset" : "none";
}
