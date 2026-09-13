#include "store/store.hpp"
#include "resp/resp.hpp"

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
               "ERR wrong number of arguments for 'lrange' command")
        .to_bytes();
  }

  const std::string &key = args[1];

  if (Streams.find(key) != Streams.end()) {
    return RespType::SimpleString("stream").to_bytes();
  }

  if (Storage.find(key) == Storage.end()) {
    return RespType::SimpleString("none").to_bytes();
  }

  return RespType::SimpleString("string").to_bytes();
}
