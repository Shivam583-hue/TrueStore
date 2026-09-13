#include "net/socket.hpp"

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>

bool send_all(int fd, const std::string &payload) {
  std::size_t offset = 0;

  while (offset < payload.size()) {
    ssize_t sent =
        send(fd, payload.data() + offset, payload.size() - offset, 0);

    if (sent > 0) {
      offset += static_cast<std::size_t>(sent);
      continue;
    }

    if (sent < 0 && errno == EINTR) {
      continue;
    }

    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd writable{};
      writable.fd = fd;
      writable.events = POLLOUT;

      if (poll(&writable, 1, 1000) <= 0) {
        return false;
      }

      continue;
    }

    std::cerr << "send() failed\n";
    return false;
  }

  return true;
}

std::string read_line(int fd) {
  std::string line;
  char ch;

  while (true) {
    ssize_t bytes = recv(fd, &ch, 1, 0);

    if (bytes <= 0) {
      break;
    }

    line += ch;

    if (line.size() >= 2 && line[line.size() - 2] == '\r' &&
        line[line.size() - 1] == '\n') {
      break;
    }
  }

  return line;
}

bool set_nonblocking(int fd) {
  int on = 1;
  return ioctl(fd, FIONBIO, (char *)&on) >= 0;
}
