#include "resp.hpp"

RespError::RespError(const std::string &message)
    : std::runtime_error(message) {}

RespType RespType::SimpleString(std::string value) {
  return {Type::SimpleString, std::move(value), {}};
}

RespType RespType::BulkString(std::string value) {
  return {Type::BulkString, std::move(value), {}};
}

RespType RespType::SimpleError(std::string value) {
  return {Type::SimpleError, std::move(value), {}};
}

RespType RespType::NullBulkString() { return {Type::NullBulkString, "", {}}; }

RespType RespType::Integer(long long value) {
  return {Type::Integer, std::to_string(value), {}};
}

RespType RespType::Array(std::vector<std::string> elements) {
  RespType resp;
  resp.type = Type::Array;
  resp.elements = std::move(elements);
  return resp;
}

RespType RespType::NestedArray(std::vector<RespType> nested) {
  RespType resp;
  resp.type = Type::NestedArray;
  resp.nested = std::move(nested);
  return resp;
}

RespType RespType::NullArray() { return {Type::NullArray, "", {}}; }

std::string RespType::to_bytes() const {
  switch (type) {

  case Type::SimpleString:
    return "+" + value + "\r\n";

  case Type::BulkString:
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";

  case Type::SimpleError:
    return "-" + value + "\r\n";

  case Type::NullBulkString:
    return "$-1\r\n";

  case Type::NullArray:
    return "*-1\r\n";

  case Type::Integer:
    return ":" + value + "\r\n";

  case Type::Array: {
    std::string bytes = "*" + std::to_string(elements.size()) + "\r\n";

    for (const auto &element : elements) {
      bytes += BulkString(element).to_bytes();
    }

    return bytes;
  }

  case Type::NestedArray: {
    std::string bytes = "*" + std::to_string(nested.size()) + "\r\n";

    for (const auto &element : nested) {
      bytes += element.to_bytes();
    }

    return bytes;
  }
  }

  throw RespError("Unknown RESP type");
}

std::optional<std::pair<std::string, std::size_t>>
read_till_crlf(const std::string &buffer) {
  std::size_t pos = buffer.find("\r\n");

  if (pos == std::string::npos) {
    return std::nullopt;
  }

  return std::make_pair(buffer.substr(0, pos), pos + 2);
}

std::size_t parse_usize_from_buf(const std::string &buffer) {
  try {
    return std::stoull(buffer);
  } catch (...) {
    throw RespError("Invalid integer value");
  }
}

std::pair<RespType, std::size_t>
parse_simple_string(const std::string &buffer) {
  if (buffer.empty()) {
    throw RespError("Invalid value for simple string");
  }

  auto result = read_till_crlf(buffer.substr(1));

  if (!result) {
    throw RespError("Invalid value for simple string");
  }

  auto [buf_data, len] = *result;

  std::string simple_str = buf_data;

  return {RespType::SimpleString(std::move(simple_str)), len + 1};
}

std::optional<std::pair<RespType, std::size_t>>
parse_bulk_string(const std::string &buffer) {
  if (buffer.empty()) {
    return std::nullopt;
  }

  if (buffer[0] != '$') {
    throw RespError("Expected bulk string");
  }

  auto result = read_till_crlf(buffer.substr(1));

  if (!result) {
    return std::nullopt;
  }

  auto [buf_data, len] = *result;

  std::size_t bulkstr_len = parse_usize_from_buf(buf_data);

  std::size_t bytes_consumed = len + 1;

  std::size_t bulkstr_end_idx = bytes_consumed + bulkstr_len;

  if (bulkstr_end_idx + 2 > buffer.size()) {
    return std::nullopt;
  }

  std::string bulkstr = buffer.substr(bytes_consumed, bulkstr_len);

  return std::make_pair(RespType::BulkString(std::move(bulkstr)),
                        bulkstr_end_idx + 2);
}

std::optional<std::pair<std::vector<std::string>, std::size_t>>
parse_command(const std::string &buffer) {
  if (buffer.empty()) {
    return std::nullopt;
  }

  if (buffer[0] != '*') {
    throw RespError("Expected array for command");
  }

  auto header = read_till_crlf(buffer.substr(1));

  if (!header) {
    return std::nullopt;
  }

  auto [count_str, header_len] = *header;

  std::size_t count = parse_usize_from_buf(count_str);
  std::size_t offset = header_len + 1;

  std::vector<std::string> elements;
  elements.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    auto element = parse_bulk_string(buffer.substr(offset));

    if (!element) {
      return std::nullopt;
    }

    auto &[resp, consumed] = *element;

    elements.push_back(std::move(resp.value));
    offset += consumed;
  }

  return std::make_pair(std::move(elements), offset);
}
