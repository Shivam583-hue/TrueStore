#pragma once

#include <string>
#include <vector>

#include "store/store.hpp"

struct ClientState {
  bool in_multi = false;
  std::vector<std::vector<std::string>> queued;
};

std::string to_upper(std::string value);
std::string handle_command(const std::vector<std::string> &args, Store &store,
                           ClientState &client);
