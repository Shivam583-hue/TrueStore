#include "resp/resp.hpp"
#include <store/store.hpp>

std::string Store::handleSet(const std::vector<std::string> &args) {
  if (args.size() != 3 && args.size() != 5) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'set' command")
        .to_bytes();
  }

  const std::string &key = args[1];
  const std::string &value = args[2];

  gStorage[key] = value;

  if (args.size() == 3) {
    gExpirations.erase(key);

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
    gExpirations[key] = now + std::chrono::seconds(duration);

  } else if (option == "PX") {
    gExpirations[key] = now + std::chrono::milliseconds(duration);

  } else {
    return RespType::SimpleError("ERR syntax error").to_bytes();
  }

  return RespType::SimpleString("OK").to_bytes();
}

bool Store::isExpired(const std::string &key) {
  auto it = gExpirations.find(key);

  if (it == gExpirations.end()) {
    return false;
  }

  if (std::chrono::steady_clock::now() >= it->second) {
    gExpirations.erase(it);
    gStorage.erase(key);
    return true;
  }

  return false;
}

std::string Store::handleGet(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'get' command")
        .to_bytes();
  }

  const std::string &key = args[1];

  if (isExpired(key)) {
    return RespType::NullBulkString().to_bytes();
  }

  auto it = gStorage.find(key);

  if (it == gStorage.end()) {
    return RespType::NullBulkString().to_bytes();
  }

  return RespType::BulkString(it->second).to_bytes();
}

std::string Store::handleRPUSH(const std::vector<std::string> &args) {
  std::string vec_name = args[1];
  dynamicVector[vec_name].push_back(args[2]);
  return RespType::Integer(dynamicVector[vec_name].size()).to_bytes();
}
