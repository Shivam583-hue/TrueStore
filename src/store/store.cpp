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
  for (int i = 2; i < args.size(); i++)
    DynamicVector[vec_name].push_back(args[i]);
  return RespType::Integer(DynamicVector[vec_name].size()).to_bytes();
}

std::string Store::handle_llen(const std::vector<std::string> &args) {
  const std::string &vec_name = args[1];
  return RespType::Integer(DynamicVector[vec_name].size()).to_bytes();
}

std::string Store::handle_lpush(const std::vector<std::string> &args) {
  const std::string &vec_name = args[1];
  for (int i = 2; i < args.size(); i++)
    DynamicVector[vec_name].insert(DynamicVector[vec_name].begin(), args[i]);
  return RespType::Integer(DynamicVector[vec_name].size()).to_bytes();
}

std::string Store::handle_lpop(const std::vector<std::string> &args) {
  const std::string &vec_name = args[1];
  if (!(DynamicVector.find(vec_name) != DynamicVector.end()))
    return RespType::NullBulkString().to_bytes();
  auto b = DynamicVector[vec_name];
  auto first_element = b[0];
  b.erase(b.begin());
  DynamicVector[vec_name] = b;
  return RespType::BulkString(first_element).to_bytes();
}

std::string Store::handle_lrange(const std::vector<std::string> &args) {
  if (args.size() != 4) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'lrange' command")
        .to_bytes();
  }

  const std::string &key = args[1];

  long long start;
  long long stop;

  try {
    start = std::stoll(args[2]);
    stop = std::stoll(args[3]);
  } catch (...) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  }

  auto it = DynamicVector.find(key);

  if (it == DynamicVector.end()) {
    return RespType::Array({}).to_bytes();
  }

  const std::vector<std::string> &list = it->second;
  long long size = static_cast<long long>(list.size());

  if (start < 0) {
    start += size;
  }
  if (start < 0) {
    start = 0;
  }

  if (stop < 0) {
    stop += size;
  }
  if (stop < 0) {
    stop = 0;
  }

  if (start >= size || start > stop) {
    return RespType::Array({}).to_bytes();
  }

  if (stop >= size) {
    stop = size - 1;
  }

  std::vector<std::string> range(list.begin() + start, list.begin() + stop + 1);

  return RespType::Array(std::move(range)).to_bytes();
}
