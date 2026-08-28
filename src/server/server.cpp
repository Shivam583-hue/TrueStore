#include "server.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"

#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace {
Store store;
} // namespace

Server::Server(int port) : port_(port), server_fd_(-1) {}

Server::~Server() {
  if (server_fd_ >= 0) {
    close(server_fd_);
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

void Server::run() {
  constexpr int MAX_FDS = 200;

  pollfd fds[MAX_FDS]{};

  fds[0].fd = server_fd_;
  fds[0].events = POLLIN;

  int nfds = 1;
  int timeout = 3 * 60 * 1000;

  std::unordered_map<int, std::string> buffers;

  while (true) {
    int ready = poll(fds, nfds, timeout);

    if (ready < 0) {
      if (errno == EINTR)
        continue;

      std::cerr << "poll() failed\n";
      break;
    }

    if (ready == 0) {
      std::cout << "poll() timed out\n";
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
                  close(fd);
                  fds[i].fd = -1;
                  buffers.erase(fd);
                  connection_closed = true;
                  break;
                }

                if (!command) {
                  break;
                }

                auto &[args, consumed] = *command;

                std::string response;

                try {
                  response = handle_command(args, store);
                } catch (const std::exception &e) {
                  std::cerr << "Command error: " << e.what() << '\n';
                  response =
                      RespType::SimpleError("ERR internal error").to_bytes();
                }

                ssize_t sent = send(fd, response.data(), response.size(), 0);

                if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                  std::cerr << "send() failed\n";
                }

                buffers[fd].erase(0, consumed);
              }
            }

            else if (bytes == 0) {

              close(fd);
              fds[i].fd = -1;
              buffers.erase(fd);
              connection_closed = true;
            }

            else {
              if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
              }

              if (errno == EINTR)
                continue;

              std::cerr << "recv() failed\n";

              close(fd);
              fds[i].fd = -1;
              buffers.erase(fd);
              connection_closed = true;
            }
          }
        }
      }

      if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {

        if (fd != server_fd_ && fds[i].fd >= 0) {
          close(fd);
          fds[i].fd = -1;
          buffers.erase(fd);
        }
      }
    }

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
