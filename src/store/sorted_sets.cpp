#include "store/store.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace {
std::string arity_error(const std::string &command) {
  return RespType::SimpleError("ERR wrong number of arguments for '" + command +
                               "' command").to_bytes();
}

std::string wrong_type() {
  return RespType::SimpleError(
             "WRONGTYPE Operation against a key holding the wrong kind of value")
      .to_bytes();
}

bool parse_score(const std::string &text, double &score) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front()))) {
    return false;
  }
  try {
    std::size_t consumed;
    score = std::stod(text, &consumed);
    return consumed == text.size() && !std::isnan(score);
  } catch (const std::exception &) {
    return false;
  }
}

bool parse_index(const std::string &text, long long &index) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), index);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::string format_score(double score) {
  if (score == 0) {
    return "0";
  }
  if (std::isinf(score)) {
    return score < 0 ? "-inf" : "inf";
  }
  char buffer[128];
  const auto result = std::to_chars(std::begin(buffer), std::end(buffer), score);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("Failed to format sorted set score");
  }
  return std::string(buffer, result.ptr);
}
}

bool Store::SortedSet::add(double score, const std::string &member) {
  auto [it, inserted] = scores.try_emplace(member, score);
  if (!inserted) {
    ordered.erase({it->second, member});
    it->second = score;
  }
  ordered.emplace(score, member);
  return inserted;
}

bool Store::wrong_sorted_set_type(const std::string &key) {
  const auto type = key_type(key);
  return type != "none" && type != "zset";
}

std::string Store::handle_zadd(const std::vector<std::string> &args) {
  if (args.size() < 4 || args.size() % 2 != 0) {
    return arity_error("zadd");
  }

  std::vector<std::pair<double, std::string>> entries;
  for (std::size_t i = 2; i < args.size(); i += 2) {
    double score;
    if (!parse_score(args[i], score)) {
      return RespType::SimpleError("ERR value is not a valid float").to_bytes();
    }
    entries.emplace_back(score, args[i + 1]);
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }

  auto &set = sorted_sets_[args[1]];
  long long added = 0;
  for (const auto &[score, member] : entries) {
    added += set.add(score, member);
  }
  return RespType::Integer(added).to_bytes();
}

std::string Store::handle_zrank(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    return arity_error("zrank");
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto it = sorted_sets_.find(args[1]);
  if (it == sorted_sets_.end()) {
    return RespType::NullBulkString().to_bytes();
  }
  const auto member = it->second.scores.find(args[2]);
  if (member == it->second.scores.end()) {
    return RespType::NullBulkString().to_bytes();
  }
  const auto position = it->second.ordered.find({member->second, member->first});
  return RespType::Integer(std::distance(it->second.ordered.begin(), position))
      .to_bytes();
}

std::string Store::handle_zrange(const std::vector<std::string> &args) {
  if (args.size() != 4 && args.size() != 5) {
    return arity_error("zrange");
  }
  const bool with_scores = args.size() == 5;
  if (with_scores && to_upper(args[4]) != "WITHSCORES") {
    return RespType::SimpleError("ERR syntax error").to_bytes();
  }
  long long start, stop;
  if (!parse_index(args[2], start) || !parse_index(args[3], stop)) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto it = sorted_sets_.find(args[1]);
  if (it == sorted_sets_.end()) {
    return RespType::Array({}).to_bytes();
  }

  const auto &ordered = it->second.ordered;
  const auto size = static_cast<long long>(ordered.size());
  if (start < 0) {
    start += size;
  }
  if (stop < 0) {
    stop += size;
  }
  start = std::max(start, 0LL);
  stop = std::min(stop, size - 1);
  if (start > stop || start >= size) {
    return RespType::Array({}).to_bytes();
  }

  std::vector<std::string> members;
  auto position = std::next(ordered.begin(), start);
  for (long long index = start; index <= stop; ++index, ++position) {
    members.push_back(position->second);
    if (with_scores) {
      members.push_back(format_score(position->first));
    }
  }
  return RespType::Array(std::move(members)).to_bytes();
}

std::string Store::handle_zcard(const std::vector<std::string> &args) {
  if (args.size() != 2) {
    return arity_error("zcard");
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto it = sorted_sets_.find(args[1]);
  return RespType::Integer(it == sorted_sets_.end()
                               ? 0 : static_cast<long long>(it->second.scores.size()))
      .to_bytes();
}

std::string Store::handle_zscore(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    return arity_error("zscore");
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto it = sorted_sets_.find(args[1]);
  if (it == sorted_sets_.end()) {
    return RespType::NullBulkString().to_bytes();
  }
  const auto member = it->second.scores.find(args[2]);
  if (member == it->second.scores.end()) {
    return RespType::NullBulkString().to_bytes();
  }
  return RespType::BulkString(format_score(member->second)).to_bytes();
}

std::string Store::handle_zrem(const std::vector<std::string> &args) {
  if (args.size() < 3) {
    return arity_error("zrem");
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto it = sorted_sets_.find(args[1]);
  if (it == sorted_sets_.end()) {
    return RespType::Integer(0).to_bytes();
  }

  long long removed = 0;
  auto &set = it->second;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto member = set.scores.find(args[i]);
    if (member != set.scores.end()) {
      set.ordered.erase({member->second, member->first});
      set.scores.erase(member);
      ++removed;
    }
  }
  if (set.scores.empty()) {
    sorted_sets_.erase(it);
  }
  return RespType::Integer(removed).to_bytes();
}
