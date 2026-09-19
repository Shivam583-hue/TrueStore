#pragma once

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "store/store.hpp"

struct ClientState {
  int fd = -1;
  bool authenticated = false;
  std::set<std::string> subscriptions;
  bool close_after_reply = false;
  bool in_multi = false;
  std::vector<std::vector<std::string>> queued;
  std::vector<std::pair<std::string, std::optional<std::string>>> watched;
  long long repl_offset = 0;
};

std::string to_upper(std::string value);
bool is_write_command(const std::string &command);
std::string handle_command(const std::vector<std::string> &args, Store &store,
                           ClientState &client);
