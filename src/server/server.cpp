#include "server.hpp"

#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

            // TODO:clean up if necessary, place holder code for debugging.
            // std::cout << "New connection: " << client_fd << '\n';

            fds[nfds].fd = client_fd;
            fds[nfds].events = POLLIN;
            ++nfds;
          }
        }

        else {
          while (true) {
            char buffer[80];

            ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);

            if (bytes > 0) {
              // TODO: debugging code
              // std::cout << "Bytes received: " << bytes << '\n';
              //
              // std::cout.write(buffer, bytes);
              // std::cout << '\n';

              const char *response = "+PONG\r\n";
              // ssize_t sent = send(fd, response, strlen(response), 0);
              ssize_t sent = send(fd, response, std::strlen(response), 0);

              if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "send() failed\n";
              }
            }

            else if (bytes == 0) {

              // TODO: debugging code
              // std::cout << "Client disconnected\n";

              close(fd);
              fds[i].fd = -1;
              break;
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
              break;
            }
          }
        }
      }

      if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {

        if (fd != server_fd_ && fds[i].fd >= 0) {
          close(fd);
          fds[i].fd = -1;
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
