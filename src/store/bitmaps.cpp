#include "store/store.hpp"
#include "resp/resp.hpp"

#include <charconv>
#include <cstdint>

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
         offset < (std::uint64_t{1} << 32);
}

std::string offset_error() {
  return RespType::SimpleError("ERR bit offset is not an integer or out of range")
      .to_bytes();
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
