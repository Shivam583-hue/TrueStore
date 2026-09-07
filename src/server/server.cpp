#include "server.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"
#include "store/store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
Store store;

struct Waiter {
  int fd;
  BlockKind kind;
  std::vector<std::string> keys;
  std::vector<StreamID> ids;
  std::size_t count;
  bool has_deadline;
  std::chrono::steady_clock::time_point deadline;
};

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
} // namespace

Server::Server(int port, bool is_replica, std::string master_host,
               int master_port)
    : port_(port), server_fd_(-1), is_replica_(is_replica),
      master_host_(std::move(master_host)), master_port_(master_port),
      master_fd_(-1) {}

Server::~Server() {
  if (server_fd_ >= 0) {
    close(server_fd_);
  }

  if (master_fd_ >= 0) {
    close(master_fd_);
  }
}

bool Server::start() {
  server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    std::cerr << "socket() failed\n";
    return false;
  }

  int reuse = 1;
  if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    std::cerr << "setsockopt() failed\n";
    return false;
  }

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port_);

  int on = 1;
  if (ioctl(server_fd_, FIONBIO, (char *)&on) < 0) {
    std::cerr << "ioctl() failed\n";
    return false;
  }

  if (bind(server_fd_, reinterpret_cast<sockaddr *>(&server_addr),
           sizeof(server_addr)) < 0) {
    std::cerr << "bind() failed\n";
    return false;
  }

  if (listen(server_fd_, 5) < 0) {
    std::cerr << "listen() failed\n";
    return false;
  }

  return true;
}

void Server::connect_to_master() {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *resolved = nullptr;
  std::string port_str = std::to_string(master_port_);

  if (getaddrinfo(master_host_.c_str(), port_str.c_str(), &hints, &resolved) !=
      0) {
    std::cerr << "Failed to resolve master host\n";
    return;
  }

  int fd = socket(resolved->ai_family, resolved->ai_socktype,
                   resolved->ai_protocol);

  if (fd < 0) {
    std::cerr << "Failed to create socket to master\n";
    freeaddrinfo(resolved);
    return;
  }

  if (connect(fd, resolved->ai_addr, resolved->ai_addrlen) < 0) {
    std::cerr << "Failed to connect to master\n";
    close(fd);
    freeaddrinfo(resolved);
    return;
  }

  freeaddrinfo(resolved);

  std::string ping = RespType::Array({"PING"}).to_bytes();

  if (!send_all(fd, ping)) {
    std::cerr << "Failed to send PING to master\n";
    close(fd);
    return;
  }

  if (read_line(fd).empty()) {
    std::cerr << "No reply to PING from master\n";
    close(fd);
    return;
  }

  std::string replconf_port =
      RespType::Array({"REPLCONF", "listening-port", std::to_string(port_)})
          .to_bytes();

  if (!send_all(fd, replconf_port)) {
    std::cerr << "Failed to send REPLCONF listening-port to master\n";
    close(fd);
    return;
  }

  if (read_line(fd).empty()) {
    std::cerr << "No reply to REPLCONF listening-port from master\n";
    close(fd);
    return;
  }

  std::string replconf_capa =
      RespType::Array({"REPLCONF", "capa", "psync2"}).to_bytes();

  if (!send_all(fd, replconf_capa)) {
    std::cerr << "Failed to send REPLCONF capa to master\n";
    close(fd);
    return;
  }

  if (read_line(fd).empty()) {
    std::cerr << "No reply to REPLCONF capa from master\n";
    close(fd);
    return;
  }

  std::string psync = RespType::Array({"PSYNC", "?", "-1"}).to_bytes();

  if (!send_all(fd, psync)) {
    std::cerr << "Failed to send PSYNC to master\n";
    close(fd);
    return;
  }

  if (read_line(fd).empty()) {
    std::cerr << "No reply to PSYNC from master\n";
    close(fd);
    return;
  }

  master_fd_ = fd;
}

void Server::run() {
  store.init(is_replica_, master_host_, master_port_);

  if (is_replica_) {
    connect_to_master();
  }

  constexpr int MAX_FDS = 200;

  pollfd fds[MAX_FDS]{};

  fds[0].fd = server_fd_;
  fds[0].events = POLLIN;

  int nfds = 1;

  std::unordered_map<int, std::string> buffers;
  std::unordered_map<int, ClientState> clients;

  std::vector<Waiter> waiters;

  auto close_client = [&](int index, int fd) {
    close(fd);
    fds[index].fd = -1;
    buffers.erase(fd);
    clients.erase(fd);
    waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
                                 [fd](const Waiter &w) { return w.fd == fd; }),
                  waiters.end());
  };

  auto serve_waiters = [&]() {
    for (std::size_t i = 0; i < waiters.size();) {
      const Waiter &waiter = waiters[i];
      std::string response;

      if (waiter.kind == BlockKind::List) {
        if (auto popped = store.try_blpop(waiter.keys)) {
          response =
              RespType::Array({popped->first, popped->second}).to_bytes();
        }
      } else if (auto reply =
                     store.try_xread(waiter.keys, waiter.ids, waiter.count)) {
        response = std::move(*reply);
      }

      if (response.empty()) {
        ++i;
        continue;
      }

      send_all(waiter.fd, response);
      waiters.erase(waiters.begin() + static_cast<std::ptrdiff_t>(i));
    }
  };

  auto expire_waiters = [&]() {
    const auto now = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < waiters.size();) {
      if (waiters[i].has_deadline && now >= waiters[i].deadline) {
        send_all(waiters[i].fd, RespType::NullArray().to_bytes());
        waiters.erase(waiters.begin() + static_cast<std::ptrdiff_t>(i));
      } else {
        ++i;
      }
    }
  };

  auto next_timeout = [&]() {
    const auto now = std::chrono::steady_clock::now();
    int timeout = -1;

    for (const Waiter &waiter : waiters) {
      if (!waiter.has_deadline) {
        continue;
      }

      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                           waiter.deadline - now)
                           .count();

      if (remaining < 0) {
        remaining = 0;
      }

      if (timeout < 0 || remaining < timeout) {
        timeout = static_cast<int>(remaining);
      }
    }

    return timeout;
  };

  while (true) {
    int ready = poll(fds, nfds, next_timeout());

    if (ready < 0) {
      if (errno == EINTR)
        continue;

      std::cerr << "poll() failed\n";
      break;
    }

    int current_size = nfds;

    for (int i = 0; i < current_size; ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0)
        continue;

      int fd = fds[i].fd;

      if (fds[i].revents & POLLIN) {

        if (fd == server_fd_) {

          while (true) {
            int client_fd = accept(server_fd_, nullptr, nullptr);

            if (client_fd < 0) {
              if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

              if (errno == EINTR)
                continue;

              std::cerr << "accept() failed\n";
              break;
            }

            int on = 1;

            if (ioctl(client_fd, FIONBIO, (char *)&on) < 0) {
              std::cerr << "Failed to make client non-blocking\n";
              close(client_fd);
              continue;
            }

            if (nfds == MAX_FDS) {
              std::cerr << "Too many clients\n";
              close(client_fd);
              continue;
            }

            fds[nfds].fd = client_fd;
            fds[nfds].events = POLLIN;
            ++nfds;
          }
        }

        else {
          bool connection_closed = false;

          while (!connection_closed) {
            char chunk[4096];

            ssize_t bytes = recv(fd, chunk, sizeof(chunk), 0);

            if (bytes > 0) {
              buffers[fd].append(chunk, static_cast<std::size_t>(bytes));

              while (true) {
                std::optional<std::pair<std::vector<std::string>, std::size_t>>
                    command;

                try {
                  command = parse_command(buffers[fd]);
                } catch (const RespError &e) {
                  std::cerr << "Protocol error: " << e.what() << '\n';
                  close_client(i, fd);
                  connection_closed = true;
                  break;
                }

                if (!command) {
                  break;
                }

                auto &[args, consumed] = *command;

                std::string response;

                try {
                  response = handle_command(args, store, clients[fd]);
                } catch (const std::exception &e) {
                  std::cerr << "Command error: " << e.what() << '\n';
                  response =
                      RespType::SimpleError("ERR internal error").to_bytes();
                }

                buffers[fd].erase(0, consumed);

                if (auto block = store.take_pending_block()) {
                  Waiter waiter;
                  waiter.fd = fd;
                  waiter.kind = block->kind;
                  waiter.keys = std::move(block->keys);
                  waiter.ids = std::move(block->ids);
                  waiter.count = block->count;
                  waiter.has_deadline = block->timeout > 0;

                  if (waiter.has_deadline) {
                    waiter.deadline =
                        std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(block->timeout));
                  }

                  waiters.push_back(std::move(waiter));
                } else if (!response.empty()) {
                  send_all(fd, response);
                }
              }
            }

            else if (bytes == 0) {

              close_client(i, fd);
              connection_closed = true;
            }

            else {
              if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
              }

              if (errno == EINTR)
                continue;

              std::cerr << "recv() failed\n";

              close_client(i, fd);
              connection_closed = true;
            }
          }
        }
      }

      if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {

        if (fd != server_fd_ && fds[i].fd >= 0) {
          close_client(i, fd);
        }
      }
    }

    serve_waiters();
    expire_waiters();

    for (int i = 1; i < nfds;) {
      if (fds[i].fd == -1) {
        fds[i] = fds[nfds - 1];
        --nfds;
      } else {
        ++i;
      }
    }
  }
}
