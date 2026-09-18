#include "command/command.hpp"
#include "resp/resp.hpp"
#include "store/store.hpp"

#include <limits>

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

    if (sorted_sets_.contains(key)) {
      return RespType::SimpleError(
                 "WRONGTYPE Operation against a key holding the wrong kind of value")
          .to_bytes();
    }

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
