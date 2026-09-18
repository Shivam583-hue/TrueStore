#include "resp/resp.hpp"
#include "store/store.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

std::string Store::handle_rpush(const std::vector<std::string> &args) {
  if (args.size() < 3) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'rpush' command")
        .to_bytes();
  }

  const std::string &vec_name = args[1];
  std::vector<std::string> &list = DynamicVector[vec_name];

  for (std::size_t i = 2; i < args.size(); ++i) {
    list.push_back(args[i]);
  }

  return RespType::Integer(static_cast<long long>(list.size())).to_bytes();
}

std::string Store::handle_llen(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'llen' command")
        .to_bytes();
  }

  auto it = DynamicVector.find(args[1]);

  if (it == DynamicVector.end()) {
    return RespType::Integer(0).to_bytes();
  }

  return RespType::Integer(static_cast<long long>(it->second.size()))
      .to_bytes();
}

std::string Store::handle_lpush(const std::vector<std::string> &args) {
  if (args.size() < 3) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'lpush' command")
        .to_bytes();
  }

  const std::string &vec_name = args[1];
  std::vector<std::string> &list = DynamicVector[vec_name];

  for (std::size_t i = 2; i < args.size(); ++i) {
    list.insert(list.begin(), args[i]);
  }

  return RespType::Integer(static_cast<long long>(list.size())).to_bytes();
}

std::string Store::handle_lpop(const std::vector<std::string> &args) {
  if (args.size() != 2 && args.size() != 3) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'lpop' command")
        .to_bytes();
  }

  const std::string &key = args[1];

  auto it = DynamicVector.find(key);
  const bool empty = it == DynamicVector.end() || it->second.empty();

  if (args.size() == 2) {
    if (empty) {
      return RespType::NullBulkString().to_bytes();
    }

    std::string first = std::move(it->second.front());
    it->second.erase(it->second.begin());

    if (it->second.empty()) {
      DynamicVector.erase(it);
    }

    return RespType::BulkString(std::move(first)).to_bytes();
  }

  long long count;

  try {
    count = std::stoll(args[2]);
  } catch (...) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  }

  if (count < 0) {
    return RespType::SimpleError("ERR value is out of range, must be positive")
        .to_bytes();
  }

  if (empty) {
    return RespType::NullArray().to_bytes();
  }

  if (count == 0) {
    return RespType::Array({}).to_bytes();
  }

  std::vector<std::string> &list = it->second;
  const std::size_t taken =
      std::min(static_cast<std::size_t>(count), list.size());

  std::vector<std::string> removed(
      std::make_move_iterator(list.begin()),
      std::make_move_iterator(list.begin() +
                              static_cast<std::ptrdiff_t>(taken)));

  list.erase(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(taken));

  if (list.empty()) {
    DynamicVector.erase(it);
  }

  return RespType::Array(std::move(removed)).to_bytes();
}

std::optional<std::pair<std::string, std::string>>
Store::try_blpop(const std::vector<std::string> &keys) {
  for (const std::string &key : keys) {
    auto it = DynamicVector.find(key);

    if (it == DynamicVector.end() || it->second.empty()) {
      continue;
    }

    std::string value = std::move(it->second.front());
    it->second.erase(it->second.begin());

    if (it->second.empty()) {
      DynamicVector.erase(it);
    }

    queue_propagation({"LPOP", key});
    return std::make_pair(key, std::move(value));
  }

  return std::nullopt;
}

std::string Store::handle_blpop(const std::vector<std::string> &args) {
  pending_block_.reset();

  if (args.size() < 3) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'blpop' command")
        .to_bytes();
  }

  double timeout;

  try {
    std::size_t consumed;
    timeout = std::stod(args.back(), &consumed);

    if (consumed != args.back().size()) {
      throw std::invalid_argument("trailing characters");
    }
  } catch (...) {
    return RespType::SimpleError("ERR timeout is not a float or out of range")
        .to_bytes();
  }

  if (std::isnan(timeout) || timeout < 0) {
    return RespType::SimpleError("ERR timeout is negative").to_bytes();
  }

  const std::vector<std::string> keys(args.begin() + 1, args.end() - 1);

  for (const auto &key : keys) {
    if (sorted_sets_.contains(key)) {
      return RespType::SimpleError(
                 "WRONGTYPE Operation against a key holding the wrong kind of value")
          .to_bytes();
    }
  }

  auto popped = try_blpop(keys);

  if (popped) {
    return RespType::Array({popped->first, popped->second}).to_bytes();
  }

  pending_block_ = BlockRequest{BlockKind::List, keys, {}, 0, timeout};
  return {};
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
