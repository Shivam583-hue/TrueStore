#include "rdb/rdb.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {

class Reader {
public:
  explicit Reader(std::string data) : data_(std::move(data)) {}

  bool done() const { return pos_ >= data_.size(); }

  std::uint8_t byte() {
    if (pos_ >= data_.size()) {
      throw std::runtime_error("unexpected end of RDB file");
    }

    return static_cast<std::uint8_t>(data_[pos_++]);
  }

  std::uint64_t little_endian(std::size_t width) {
    std::uint64_t value = 0;

    for (std::size_t i = 0; i < width; ++i) {
      value |= static_cast<std::uint64_t>(byte()) << (8 * i);
    }

    return value;
  }

  std::uint64_t big_endian(std::size_t width) {
    std::uint64_t value = 0;

    for (std::size_t i = 0; i < width; ++i) {
      value = (value << 8) | byte();
    }

    return value;
  }

  std::string bytes(std::size_t count) {
    if (data_.size() - pos_ < count) {
      throw std::runtime_error("unexpected end of RDB file");
    }

    std::string out = data_.substr(pos_, count);
    pos_ += count;
    return out;
  }

  std::uint64_t length() {
    std::uint8_t first = byte();

    switch (first >> 6) {
    case 0:
      return first & 0x3F;
    case 1:
      return (static_cast<std::uint64_t>(first & 0x3F) << 8) | byte();
    case 2:
      if (first == 0x80) {
        return big_endian(4);
      }
      if (first == 0x81) {
        return big_endian(8);
      }
      break;
    }

    throw std::runtime_error("invalid RDB length encoding");
  }

  std::string string() {
    if (done()) {
      throw std::runtime_error("unexpected end of RDB file");
    }

    std::uint8_t first = static_cast<std::uint8_t>(data_[pos_]);

    if ((first >> 6) != 3) {
      return bytes(static_cast<std::size_t>(length()));
    }

    ++pos_;

    switch (first) {
    case 0xC0:
      return std::to_string(static_cast<std::int8_t>(byte()));
    case 0xC1:
      return std::to_string(static_cast<std::int16_t>(little_endian(2)));
    case 0xC2:
      return std::to_string(static_cast<std::int32_t>(little_endian(4)));
    }

    throw std::runtime_error("unsupported RDB string encoding");
  }

private:
  std::string data_;
  std::size_t pos_ = 0;
};

}

std::vector<RdbEntry> load_rdb(const std::string &path) {
  std::ifstream file(path, std::ios::binary);

  if (!file) {
    return {};
  }

  Reader reader(std::string(std::istreambuf_iterator<char>(file), {}));

  if (reader.bytes(9).substr(0, 5) != "REDIS") {
    throw std::runtime_error("missing REDIS header");
  }

  std::vector<RdbEntry> entries;
  std::optional<std::uint64_t> expire_at_ms;

  while (!reader.done()) {
    std::uint8_t opcode = reader.byte();

    switch (opcode) {
    case 0xFA:
      reader.string();
      reader.string();
      break;

    case 0xFE:
      reader.length();
      break;

    case 0xFB:
      reader.length();
      reader.length();
      break;

    case 0xFC:
      expire_at_ms = reader.little_endian(8);
      break;

    case 0xFD:
      expire_at_ms = reader.little_endian(4) * 1000;
      break;

    case 0x00: {
      RdbEntry entry;
      entry.key = reader.string();
      entry.value = reader.string();
      entry.expire_at_ms = std::exchange(expire_at_ms, std::nullopt);
      entries.push_back(std::move(entry));
      break;
    }

    case 0xFF:
      return entries;

    default:
      throw std::runtime_error("unsupported RDB opcode");
    }
  }

  return entries;
}
