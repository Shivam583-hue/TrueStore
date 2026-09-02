#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "rax/rax.h"
}

using StreamEntryData = std::vector<std::pair<std::string, std::string>>;

struct StreamID {
  std::uint64_t milliseconds = 0;
  std::uint64_t sequence = 0;

  static bool parse(const std::string &text, StreamID &out);

  std::string to_string() const;
};

bool operator<(const StreamID &a, const StreamID &b);

bool parse_range_start(const std::string &text, StreamID &out);
bool parse_range_end(const std::string &text, StreamID &out);
bool parse_read_id(const std::string &text, StreamID &out);
bool advance_id(StreamID &id);

enum class StreamAddResult {
  Ok,
  InvalidID,
  ZeroID,
  NotGreater,
};

class Stream {
  static constexpr std::size_t kEncodedIDSize = 16;
  using EncodedID = std::array<unsigned char, kEncodedIDSize>;

  rax *tree_ = nullptr;
  bool has_max_id_ = false;
  StreamID max_id_{};

  static void free_payload(void *payload);
  static EncodedID encode_id(const StreamID &id);
  static StreamID decode_id(const unsigned char *key, std::size_t len);

  StreamAddResult resolve_id(const std::string &id_text, StreamID &out) const;
  StreamAddResult next_sequence(std::uint64_t milliseconds,
                                StreamID &out) const;

public:
  Stream();
  ~Stream();

  Stream(const Stream &) = delete;
  Stream &operator=(const Stream &) = delete;
  Stream(Stream &&) = delete;
  Stream &operator=(Stream &&) = delete;

  StreamAddResult insert(const std::string &id_text, StreamEntryData data,
                         StreamID &assigned);

  const StreamEntryData *find(const std::string &id_text) const;

  bool last_id(StreamID &out) const;

  std::vector<std::pair<std::string, StreamEntryData>>
  get_range(const StreamID &start, const StreamID &end,
            std::size_t count) const;
};
