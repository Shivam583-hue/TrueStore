#include "store/store.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>

template <typename T> const char *get_type() { return "unknown"; }
template <> const char *get_type<int>() { return "int"; }
template <> const char *get_type<double>() { return "double"; }
template <> const char *get_type<float>() { return "float"; }
template <> const char *get_type<char>() { return "char"; }
template <> const char *get_type<bool>() { return "bool"; }
template <> const char *get_type<std::string>() { return "string"; }

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

    return std::make_pair(key, std::move(value));
  }

  return std::nullopt;
}

std::optional<BlockRequest> Store::take_pending_block() {
  std::optional<BlockRequest> block = std::move(pending_block_);
  pending_block_.reset();
  return block;
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

  auto popped = try_blpop(keys);

  if (popped) {
    return RespType::Array({popped->first, popped->second}).to_bytes();
  }

  pending_block_ = BlockRequest{BlockKind::List, keys, {}, 0, timeout};
  return {};
}

std::string Store::handle_type(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'lrange' command")
        .to_bytes();
  }

  auto key = args[1];
  std::string n = "none";

  if (Streams.find(key) != Streams.end()) {
    return RespType::SimpleString("stream").to_bytes();
  }

  auto it = Storage.find(key);
  if (it == Storage.end()) {
    return RespType::SimpleString(n).to_bytes();
  }

  auto val = it->second;
  return RespType::SimpleString(get_type<decltype(val)>()).to_bytes();
}

std::string Store::handle_xadd(const std::vector<std::string> &args) {
  if (args.size() < 5 || (args.size() - 3) % 2 != 0) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'xadd' command")
        .to_bytes();
  }

  const std::string &key = args[1];
  const std::string &id = args[2];

  StreamEntryData data;
  data.reserve((args.size() - 3) / 2);

  for (std::size_t i = 3; i < args.size(); i += 2) {
    data.emplace_back(args[i], args[i + 1]);
  }

  auto it = Streams.find(key);
  const bool created = it == Streams.end();

  if (created) {
    it = Streams.try_emplace(key).first;
  }

  StreamID assigned;
  const StreamAddResult result =
      it->second.insert(id, std::move(data), assigned);

  if (created && result != StreamAddResult::Ok) {
    Streams.erase(it);
  }

  switch (result) {
  case StreamAddResult::Ok:
    return RespType::BulkString(assigned.to_string()).to_bytes();

  case StreamAddResult::ZeroID:
    return RespType::SimpleError(
               "ERR The ID specified in XADD must be greater than 0-0")
        .to_bytes();

  case StreamAddResult::NotGreater:
    return RespType::SimpleError("ERR The ID specified in XADD is equal or "
                                 "smaller than the target stream top item")
        .to_bytes();

  case StreamAddResult::InvalidID:
    break;
  }

  return RespType::SimpleError(
             "ERR Invalid stream ID specified as stream command argument")
      .to_bytes();
}

namespace {

RespType entries_to_resp(
    const std::vector<std::pair<std::string, StreamEntryData>> &entries) {
  std::vector<RespType> encoded;
  encoded.reserve(entries.size());

  for (const auto &[id, data] : entries) {
    std::vector<std::string> fields;
    fields.reserve(data.size() * 2);

    for (const auto &[field, value] : data) {
      fields.push_back(field);
      fields.push_back(value);
    }

    encoded.push_back(RespType::NestedArray(
        {RespType::BulkString(id), RespType::Array(std::move(fields))}));
  }

  return RespType::NestedArray(std::move(encoded));
}

} // namespace

std::string Store::handle_xrange(const std::vector<std::string> &args) {
  if (args.size() != 4 && args.size() != 6) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'xrange' command")
        .to_bytes();
  }

  StreamID start;
  StreamID end;

  if (!parse_range_start(args[2], start) || !parse_range_end(args[3], end)) {
    return RespType::SimpleError(
               "ERR Invalid stream ID specified as stream command argument")
        .to_bytes();
  }

  std::size_t count = 0;

  if (args.size() == 6) {
    if (to_upper(args[4]) != "COUNT") {
      return RespType::SimpleError("ERR syntax error").to_bytes();
    }

    long long parsed;

    try {
      parsed = std::stoll(args[5]);
    } catch (...) {
      return RespType::SimpleError(
                 "ERR value is not an integer or out of range")
          .to_bytes();
    }

    if (parsed <= 0) {
      return RespType::Array({}).to_bytes();
    }

    count = static_cast<std::size_t>(parsed);
  }

  auto it = Streams.find(args[1]);

  if (it == Streams.end()) {
    return RespType::Array({}).to_bytes();
  }

  return entries_to_resp(it->second.get_range(start, end, count)).to_bytes();
}

std::optional<std::string>
Store::try_xread(const std::vector<std::string> &keys,
                 const std::vector<StreamID> &ids, std::size_t count) {
  const StreamID end{std::numeric_limits<std::uint64_t>::max(),
                     std::numeric_limits<std::uint64_t>::max()};

  std::vector<RespType> replies;

  for (std::size_t n = 0; n < keys.size(); ++n) {
    auto it = Streams.find(keys[n]);

    if (it == Streams.end()) {
      continue;
    }

    StreamID start = ids[n];

    if (!advance_id(start)) {
      continue;
    }

    auto entries = it->second.get_range(start, end, count);

    if (entries.empty()) {
      continue;
    }

    replies.push_back(RespType::NestedArray(
        {RespType::BulkString(keys[n]), entries_to_resp(entries)}));
  }

  if (replies.empty()) {
    return std::nullopt;
  }

  return RespType::NestedArray(std::move(replies)).to_bytes();
}

std::string Store::handle_xread(const std::vector<std::string> &args) {
  pending_block_.reset();

  std::size_t i = 1;
  std::size_t count = 0;
  bool blocking = false;
  double timeout = 0;

  while (i < args.size() && to_upper(args[i]) != "STREAMS") {
    const std::string option = to_upper(args[i]);

    if (option != "COUNT" && option != "BLOCK") {
      return RespType::SimpleError("ERR syntax error").to_bytes();
    }

    if (i + 1 >= args.size()) {
      return RespType::SimpleError("ERR syntax error").to_bytes();
    }

    long long parsed;

    try {
      parsed = std::stoll(args[i + 1]);
    } catch (...) {
      return RespType::SimpleError(
                 option == "BLOCK"
                     ? "ERR timeout is not an integer or out of range"
                     : "ERR value is not an integer or out of range")
          .to_bytes();
    }

    if (option == "COUNT") {
      count = parsed > 0 ? static_cast<std::size_t>(parsed) : 0;
    } else {
      if (parsed < 0) {
        return RespType::SimpleError("ERR timeout is negative").to_bytes();
      }

      blocking = true;
      timeout = static_cast<double>(parsed) / 1000.0;
    }

    i += 2;
  }

  if (i >= args.size()) {
    return RespType::SimpleError("ERR syntax error").to_bytes();
  }

  ++i;

  const std::size_t remaining = args.size() - i;

  if (remaining == 0 || remaining % 2 != 0) {
    return RespType::SimpleError(
               "ERR Unbalanced XREAD list of streams: for each stream key an "
               "ID or '$' must be specified.")
        .to_bytes();
  }

  const std::size_t total = remaining / 2;

  std::vector<std::string> keys;
  std::vector<StreamID> ids;
  keys.reserve(total);
  ids.reserve(total);

  for (std::size_t n = 0; n < total; ++n) {
    const std::string &key = args[i + n];
    const std::string &id_text = args[i + total + n];

    StreamID id{};

    if (id_text == "$") {
      auto it = Streams.find(key);

      if (it != Streams.end()) {
        it->second.last_id(id);
      }
    } else if (!parse_read_id(id_text, id)) {
      return RespType::SimpleError(
                 "ERR Invalid stream ID specified as stream command argument")
          .to_bytes();
    }

    keys.push_back(key);
    ids.push_back(id);
  }

  if (auto reply = try_xread(keys, ids, count)) {
    return *reply;
  }

  if (blocking) {
    pending_block_ = BlockRequest{BlockKind::Stream, std::move(keys),
                                  std::move(ids), count, timeout};
    return {};
  }

  return RespType::NullArray().to_bytes();
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
