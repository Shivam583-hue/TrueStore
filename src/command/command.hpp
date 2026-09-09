#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "store/store.hpp"

struct ClientState {
  bool in_multi = false;
  std::vector<std::vector<std::string>> queued;
  std::vector<std::pair<std::string, std::optional<std::string>>> watched;
};

std::string to_upper(std::string value);
bool is_write_command(const std::string &command);
std::string handle_command(const std::vector<std::string> &args, Store &store,
                           ClientState &client);
