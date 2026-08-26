#include "store/store.hpp"
#include "resp/resp.hpp"

std::string Store::handle_set(const std::vector<std::string> &args) {
  if (args.size() != 3 && args.size() != 5) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'set' command")
        .to_bytes();
  }

  const std::string &key = args[1];
  const std::string &value = args[2];

  Storage[key] = value;

  if (args.size() == 3) {
    Expirations.erase(key);

    return RespType::SimpleString("OK").to_bytes();
  }

  const std::string &option = args[3];

  long long duration;

  try {
    duration = std::stoll(args[4]);
  } catch (...) {
    return RespType::SimpleError("ERR invalid expire time in 'set' command")
        .to_bytes();
  }

  if (duration <= 0) {
    return RespType::SimpleError("ERR invalid expire time in 'set' command")
        .to_bytes();
  }

  const auto now = std::chrono::steady_clock::now();

  if (option == "EX") {
    Expirations[key] = now + std::chrono::seconds(duration);

  } else if (option == "PX") {
    Expirations[key] = now + std::chrono::milliseconds(duration);

  } else {
    return RespType::SimpleError("ERR syntax error").to_bytes();
  }

  return RespType::SimpleString("OK").to_bytes();
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

std::string Store::handle_get(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'get' command")
        .to_bytes();
  }

  const std::string &key = args[1];

  if (is_expired(key)) {
    return RespType::NullBulkString().to_bytes();
  }

  auto it = Storage.find(key);

  if (it == Storage.end()) {
    return RespType::NullBulkString().to_bytes();
  }

  return RespType::BulkString(it->second).to_bytes();
}

std::string Store::handle_rpush(const std::vector<std::string> &args) {
  const std::string &vec_name = args[1];
  DynamicVector[vec_name].push_back(args[2]);
  return RespType::Integer(DynamicVector[vec_name].size()).to_bytes();
}
