#pragma once

#include <string>

bool send_all(int fd, const std::string &payload);
std::string read_line(int fd);
bool set_nonblocking(int fd);
