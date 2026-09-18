#include "resp/resp.hpp"
#include "store/store.hpp"

#include <stdexcept>

std::string Store::handle_set(const std::vector<std::string> &args) {
  if (args.size() != 3 && args.size() != 5) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'set' command")
        .to_bytes();
  }

  const std::string &key = args[1];
  const std::string &value = args[2];

  auto assign = [&]() {
    Storage[key] = value;
    sorted_sets_.erase(key);
    DynamicVector.erase(key);
    Streams.erase(key);
  };

  if (args.size() == 3) {
    assign();
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

  assign();
  return RespType::SimpleString("OK").to_bytes();
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

std::string Store::handle_incr(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'lrange' command")
        .to_bytes();
  }

  auto key = args[1];
  if (!(Storage.find(key) != Storage.end())) {
    Storage[key] = "1";
    return RespType::Integer(1).to_bytes();
  }
  auto val = Storage[key];
  int n;
  try {
    n = std::stoi(val);
  } catch (const std::invalid_argument &e) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  } catch (const std::out_of_range &e) {

    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  } catch (const std::exception &e) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  }
  n++;
  Storage[key] = std::to_string(n);
  return RespType::Integer(n).to_bytes();
}
