#include <string>
#include <vector>

#include "store/store.hpp"

std::string to_upper(std::string value);
std::string handle_command(const std::vector<std::string> &args, Store &store);
