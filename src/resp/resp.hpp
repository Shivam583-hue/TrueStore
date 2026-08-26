#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class RespError : public std::runtime_error {
public:
  explicit RespError(const std::string &message);
};

struct RespType {
  enum class Type {
    SimpleString,
    BulkString,
    SimpleError,
    NullBulkString,
    Integer,
    Array
  };

  Type type;
  std::string value;
  std::vector<std::string> elements;

  static RespType SimpleString(std::string value);
  static RespType BulkString(std::string value);
  static RespType SimpleError(std::string value);
  static RespType NullBulkString();
  static RespType Integer(long long value);
  static RespType Array(std::vector<std::string> elements);

  std::string to_bytes() const;
};

std::optional<std::pair<std::string, std::size_t>>
read_till_crlf(const std::string &buffer);

std::size_t parse_usize_from_buf(const std::string &buffer);

std::pair<RespType, std::size_t>
parse_simple_string(const std::string &buffer);

std::optional<std::pair<RespType, std::size_t>>
parse_bulk_string(const std::string &buffer);

std::optional<std::pair<std::vector<std::string>, std::size_t>>
parse_command(const std::string &buffer);
