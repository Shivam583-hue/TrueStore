#include "stream/stream.hpp"

#include <cerrno>
#include <charconv>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

bool StreamID::parse(const std::string &text, StreamID &out) {
  const std::size_t dash = text.find('-');

  if (dash == std::string::npos || dash == 0 || dash + 1 >= text.size() ||
      text.find('-', dash + 1) != std::string::npos) {
    return false;
  }

  std::uint64_t ms = 0;
  std::uint64_t seq = 0;

  const char *begin = text.data();
  const char *end = text.data() + text.size();

  const auto ms_result = std::from_chars(begin, begin + dash, ms);
  const auto seq_result = std::from_chars(begin + dash + 1, end, seq);

  if (ms_result.ec != std::errc{} || ms_result.ptr != begin + dash ||
      seq_result.ec != std::errc{} || seq_result.ptr != end) {
    return false;
  }

  out.milliseconds = ms;
  out.sequence = seq;
  return true;
}

std::string StreamID::to_string() const {
  return std::to_string(milliseconds) + "-" + std::to_string(sequence);
}

bool operator<(const StreamID &a, const StreamID &b) {
  if (a.milliseconds != b.milliseconds) {
    return a.milliseconds < b.milliseconds;
  }
  return a.sequence < b.sequence;
}

namespace {

void write_u64_be(unsigned char *destination, std::uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    destination[i] = static_cast<unsigned char>(value & 0xFFu);
    value >>= 8;
  }
}

std::uint64_t read_u64_be(const unsigned char *source) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | source[i];
  }
  return value;
}

} // namespace

void Stream::free_payload(void *payload) {
  delete static_cast<StreamEntryData *>(payload);
}

Stream::EncodedID Stream::encode_id(const StreamID &id) {
  EncodedID encoded{};
  write_u64_be(encoded.data(), id.milliseconds);
  write_u64_be(encoded.data() + 8, id.sequence);
  return encoded;
}

StreamID Stream::decode_id(const unsigned char *key, std::size_t len) {
  if (len != kEncodedIDSize) {
    throw std::runtime_error("invalid encoded stream ID length in rax");
  }

  StreamID id;
  id.milliseconds = read_u64_be(key);
  id.sequence = read_u64_be(key + 8);
  return id;
}

Stream::Stream() : tree_(raxNew()) {
  if (tree_ == nullptr) {
    throw std::bad_alloc();
  }
}

Stream::~Stream() {
  if (tree_ != nullptr) {
    raxFreeWithCallback(tree_, free_payload);
  }
}

StreamAddResult Stream::resolve_id(const std::string &id_text,
                                   StreamID &out) const {
  const std::size_t dash = id_text.find('-');

  // Partially auto-generated: <milliseconds>-*
  if (dash != std::string::npos &&
      id_text.compare(dash + 1, std::string::npos, "*") == 0) {
    if (dash == 0) {
      return StreamAddResult::InvalidID;
    }

    std::uint64_t ms = 0;
    const char *begin = id_text.data();
    const auto result = std::from_chars(begin, begin + dash, ms);

    if (result.ec != std::errc{} || result.ptr != begin + dash) {
      return StreamAddResult::InvalidID;
    }

    if (!has_max_id_) {
      // 0-0 is reserved, so an empty stream starts at 0-1 for ms == 0.
      out = StreamID{ms, ms == 0 ? 1u : 0u};
      return StreamAddResult::Ok;
    }

    if (ms < max_id_.milliseconds) {
      return StreamAddResult::NotGreater;
    }

    if (ms > max_id_.milliseconds) {
      out = StreamID{ms, 0};
      return StreamAddResult::Ok;
    }

    if (max_id_.sequence == std::numeric_limits<std::uint64_t>::max()) {
      return StreamAddResult::NotGreater;
    }

    out = StreamID{ms, max_id_.sequence + 1};
    return StreamAddResult::Ok;
  }

  if (!StreamID::parse(id_text, out)) {
    return StreamAddResult::InvalidID;
  }

  if (out.milliseconds == 0 && out.sequence == 0) {
    return StreamAddResult::ZeroID;
  }

  // Explicit IDs must be strictly greater than the current top item.
  if (has_max_id_ && !(max_id_ < out)) {
    return StreamAddResult::NotGreater;
  }

  return StreamAddResult::Ok;
}

StreamAddResult Stream::insert(const std::string &id_text, StreamEntryData data,
                               StreamID &assigned) {
  StreamID id;
  const StreamAddResult resolved = resolve_id(id_text, id);

  if (resolved != StreamAddResult::Ok) {
    return resolved;
  }

  EncodedID key = encode_id(id);
  auto payload = std::make_unique<StreamEntryData>(std::move(data));

  void *existing = nullptr;
  errno = 0;

  if (raxTryInsert(tree_, key.data(), key.size(), payload.get(), &existing) ==
      0) {
    if (errno == ENOMEM) {
      throw std::bad_alloc();
    }
    return StreamAddResult::NotGreater;
  }

  payload.release();
  max_id_ = id;
  has_max_id_ = true;
  assigned = id;
  return StreamAddResult::Ok;
}

const StreamEntryData *Stream::find(const std::string &id_text) const {
  StreamID id;

  if (!StreamID::parse(id_text, id)) {
    return nullptr;
  }

  EncodedID key = encode_id(id);
  void *result = raxFind(tree_, key.data(), key.size());

  if (result == raxNotFound) {
    return nullptr;
  }

  return static_cast<const StreamEntryData *>(result);
}

std::vector<std::pair<std::string, StreamEntryData>>
Stream::get_range(const std::string &start_id_text) const {
  StreamID start_id;

  if (!StreamID::parse(start_id_text, start_id)) {
    throw std::invalid_argument(
        "invalid stream ID, expected <milliseconds>-<sequence>");
  }

  EncodedID start_key = encode_id(start_id);
  std::vector<std::pair<std::string, StreamEntryData>> results;

  raxIterator iterator;
  raxStart(&iterator, tree_);

  errno = 0;

  if (!raxSeek(&iterator, ">=", start_key.data(), start_key.size())) {
    const int seek_errno = errno;
    raxStop(&iterator);

    if (seek_errno == ENOMEM) {
      throw std::bad_alloc();
    }
    throw std::runtime_error("raxSeek failed");
  }

  errno = 0;

  while (raxNext(&iterator)) {
    const StreamID id = decode_id(iterator.key, iterator.key_len);
    const auto *payload = static_cast<const StreamEntryData *>(iterator.data);

    if (payload != nullptr) {
      results.emplace_back(id.to_string(), *payload);
    }

    errno = 0;
  }

  const int next_errno = errno;
  raxStop(&iterator);

  if (next_errno == ENOMEM) {
    throw std::bad_alloc();
  }

  return results;
}
