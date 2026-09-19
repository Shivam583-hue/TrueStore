#include "store/store.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>

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

bool parse_offset(const std::string &text, std::uint64_t &offset) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), offset);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         text == std::to_string(offset) && offset < (std::uint64_t{1} << 32);
}

std::string offset_error() {
  return RespType::SimpleError("ERR bit offset is not an integer or out of range")
      .to_bytes();
}

bool parse_index(const std::string &text, long long &index) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), index);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
         text == std::to_string(index);
}
}

bool Store::wrong_string_type(const std::string &key) {
  const auto type = key_type(key);
  return type != "none" && type != "string";
}

std::string Store::handle_setbit(const std::vector<std::string> &args) {
  if (args.size() != 4) {
    return arity_error("setbit");
  }
  std::uint64_t offset;
  if (!parse_offset(args[2], offset)) {
    return offset_error();
  }
  if (args[3] != "0" && args[3] != "1") {
    return RespType::SimpleError("ERR bit is not an integer or out of range")
        .to_bytes();
  }
  if (wrong_string_type(args[1])) {
    return wrong_type();
  }
  auto &value = Storage[args[1]];
  const auto index = static_cast<std::size_t>(offset / 8);
  if (index >= value.size()) {
    value.resize(index + 1, '\0');
  }
  const auto mask = static_cast<unsigned char>(0x80 >> (offset % 8));
  const auto byte = static_cast<unsigned char>(value[index]);
  const bool previous = (byte & mask) != 0;
  value[index] = static_cast<char>(args[3] == "1" ? byte | mask : byte & ~mask);
  return RespType::Integer(previous).to_bytes();
}

std::string Store::handle_getbit(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    return arity_error("getbit");
  }
  std::uint64_t offset;
  if (!parse_offset(args[2], offset)) {
    return offset_error();
  }
  if (wrong_string_type(args[1])) {
    return wrong_type();
  }
  const auto value = Storage.find(args[1]);
  const auto index = static_cast<std::size_t>(offset / 8);
  if (value == Storage.end() || index >= value->second.size()) {
    return RespType::Integer(0).to_bytes();
  }
  const auto byte = static_cast<unsigned char>(value->second[index]);
  return RespType::Integer((byte >> (7 - offset % 8)) & 1).to_bytes();
}

std::string Store::handle_bitcount(const std::vector<std::string> &args) {
  if (args.size() != 2 && args.size() != 4 && args.size() != 5) {
    return arity_error("bitcount");
  }
  long long start = 0, end = std::numeric_limits<long long>::max();
  if (args.size() >= 4 &&
      (!parse_index(args[2], start) || !parse_index(args[3], end))) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  }
  bool bit_range = false;
  if (args.size() == 5) {
    const auto unit = to_upper(args[4]);
    if (unit != "BYTE" && unit != "BIT") {
      return RespType::SimpleError("ERR syntax error").to_bytes();
    }
    bit_range = unit == "BIT";
  }
  if (wrong_string_type(args[1])) {
    return wrong_type();
  }
  const auto value = Storage.find(args[1]);
  if (value == Storage.end() || (start < 0 && end < 0 && start > end)) {
    return RespType::Integer(0).to_bytes();
  }
  const auto length = static_cast<long long>(value->second.size()) *
                      (bit_range ? 8 : 1);
  if (start < 0) {
    start += length;
  }
  if (end < 0) {
    end += length;
  }
  start = std::max(start, 0LL);
  end = std::max(end, 0LL);
  end = std::min(end, length - 1);
  if (start > end) {
    return RespType::Integer(0).to_bytes();
  }
  const auto first_bit = static_cast<std::size_t>(bit_range ? start : start * 8);
  const auto last_bit = static_cast<std::size_t>(bit_range ? end : end * 8 + 7);
  long long count = 0;
  for (std::size_t i = first_bit / 8; i <= last_bit / 8; ++i) {
    unsigned int byte = static_cast<unsigned char>(value->second[i]);
    if (i == first_bit / 8) {
      byte &= 0xffu >> (first_bit % 8);
    }
    if (i == last_bit / 8) {
      byte &= 0xffu << (7 - last_bit % 8);
    }
    count += std::popcount(byte);
  }
  return RespType::Integer(count).to_bytes();
}

std::string Store::handle_bitop(const std::vector<std::string> &args) {
  if (args.size() < 4) {
    return arity_error("bitop");
  }
  const auto operation = to_upper(args[1]);
  if (operation != "AND" && operation != "OR" && operation != "XOR" &&
      operation != "NOT") {
    return RespType::SimpleError("ERR syntax error").to_bytes();
  }
  if (operation == "NOT" && args.size() != 4) {
    return RespType::SimpleError("ERR BITOP NOT must be called with a single source key.")
        .to_bytes();
  }
  std::vector<std::string> sources;
  std::size_t length = 0;
  for (std::size_t i = 3; i < args.size(); ++i) {
    if (wrong_string_type(args[i])) {
      return wrong_type();
    }
    const auto value = Storage.find(args[i]);
    sources.push_back(value == Storage.end() ? std::string{} : value->second);
    length = std::max(length, sources.back().size());
  }
  std::string result(length, '\0');
  for (std::size_t i = 0; i < length; ++i) {
    unsigned int byte = operation == "AND" ? 0xff : 0;
    for (const auto &source : sources) {
      const unsigned int input = i < source.size()
          ? static_cast<unsigned char>(source[i]) : 0;
      if (operation == "AND") {
        byte &= input;
      } else if (operation == "OR") {
        byte |= input;
      } else if (operation == "XOR") {
        byte ^= input;
      } else {
        byte = input ^ 0xff;
      }
    }
    result[i] = static_cast<char>(byte);
  }
  if (result.empty()) {
    Storage.erase(args[2]);
  } else {
    Storage[args[2]] = std::move(result);
  }
  Expirations.erase(args[2]);
  DynamicVector.erase(args[2]);
  Streams.erase(args[2]);
  sorted_sets_.erase(args[2]);
  return RespType::Integer(static_cast<long long>(length)).to_bytes();
}
