#include "aof/aof.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {
std::vector<std::filesystem::path>
read_manifest(const std::filesystem::path &path) {
  std::ifstream manifest(path);
  if (!manifest) {
    throw std::runtime_error("Failed to open AOF manifest: " + path.string());
  }

  std::vector<std::pair<long long, std::filesystem::path>> entries;
  std::string line;
  while (std::getline(manifest, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream entry(line);
    std::string file_key, filename, seq_key, type_key, type, extra;
    long long seq;
    if (!(entry >> file_key >> filename >> seq_key >> seq >> type_key >> type) ||
        file_key != "file" || seq_key != "seq" || type_key != "type" ||
        type != "i" || seq <= 0 || (entry >> extra) ||
        std::filesystem::path(filename).filename() != filename ||
        filename == "." || filename == "..") {
      throw std::runtime_error("Invalid incremental AOF manifest: " +
                               path.string());
    }
    entries.emplace_back(seq, path.parent_path() / filename);
  }

  if (manifest.bad() || entries.empty()) {
    throw std::runtime_error("Failed to read incremental AOF manifest: " +
                             path.string());
  }

  std::sort(entries.begin(), entries.end());
  std::vector<std::filesystem::path> files;
  long long previous = 0;
  for (const auto &[seq, file] : entries) {
    if (seq == previous) {
      throw std::runtime_error("Duplicate AOF sequence number");
    }
    previous = seq;
    files.push_back(file);
  }
  return files;
}
}

Aof::~Aof() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

void Aof::open(const Config &config) {
  if (config.appendonly != "yes") {
    return;
  }

  fsync_ = config.appendfsync;
  if (fsync_ != "always" && fsync_ != "everysec" && fsync_ != "no") {
    throw std::runtime_error("Invalid appendfsync option: " + fsync_);
  }
  last_sync_ = std::chrono::steady_clock::now();

  const auto directory =
      std::filesystem::path(config.dir) / config.appenddirname;
  std::filesystem::create_directories(directory);
  const auto manifest_path = directory / (config.appendfilename + ".manifest");

  if (!std::filesystem::exists(manifest_path)) {
    const auto filename = config.appendfilename + ".1.incr.aof";
    std::ofstream initial_file(directory / filename,
                              std::ios::binary | std::ios::app);
    initial_file.close();
    if (!initial_file) {
      throw std::runtime_error("Failed to create incremental AOF file");
    }

    const auto temporary_path = manifest_path.string() + ".tmp";
    std::ofstream manifest(temporary_path, std::ios::trunc);
    manifest << "file " << filename << " seq 1 type i\n";
    manifest.close();
    if (!manifest) {
      throw std::runtime_error("Failed to write AOF manifest");
    }
    std::filesystem::rename(temporary_path, manifest_path);
  }

  files_ = read_manifest(manifest_path);
  for (const auto &file : files_) {
    if (!std::filesystem::is_regular_file(file)) {
      throw std::runtime_error("Missing incremental AOF file: " + file.string());
    }
  }

  fd_ = ::open(files_.back().c_str(), O_WRONLY | O_APPEND);
  if (fd_ < 0) {
    throw std::system_error(errno, std::generic_category(),
                           "Failed to open incremental AOF file");
  }
}

void Aof::replay(Store &store) const {
  ClientState client;
  for (const auto &file : files_) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
      throw std::runtime_error("Failed to read AOF: " + file.string());
    }

    std::string buffer;
    char chunk[4096];
    while (input.read(chunk, sizeof(chunk)) || input.gcount() > 0) {
      buffer.append(chunk, static_cast<std::size_t>(input.gcount()));
      while (auto command = parse_command(buffer)) {
        const auto &[args, consumed] = *command;
        const auto response = handle_command(args, store, client);
        const auto blocked = store.take_pending_block();
        store.take_pending_propagations();
        if (blocked || (!response.empty() && response[0] == '-')) {
          throw std::runtime_error("Failed to replay AOF command in " +
                                   file.string() + ": " + response);
        }
        buffer.erase(0, consumed);
      }
    }
    if (input.bad() || !buffer.empty() || client.in_multi) {
      throw std::runtime_error("Incomplete AOF: " + file.string());
    }
  }
}

void Aof::append(const std::vector<std::string> &args) {
  if (fd_ >= 0) {
    write(RespType::Array(args).to_bytes());
  }
}

void Aof::append_transaction(
    const std::vector<std::vector<std::string>> &commands) {
  if (fd_ < 0 || commands.empty()) {
    return;
  }

  std::string bytes = RespType::Array({"MULTI"}).to_bytes();
  for (const auto &args : commands) {
    bytes += RespType::Array(args).to_bytes();
  }
  bytes += RespType::Array({"EXEC"}).to_bytes();
  write(bytes);
}

void Aof::write(const std::string &bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written =
        ::write(fd_, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::system_error(errno, std::generic_category(),
                             "Failed to write AOF");
    }
    if (written == 0) {
      throw std::runtime_error("Failed to complete AOF write");
    }
    offset += static_cast<std::size_t>(written);
  }
  dirty_ = true;
  sync_if_due();
}

void Aof::sync_if_due() {
  if (!dirty_ || fsync_ == "no") {
    return;
  }
  if (fsync_ == "everysec" &&
      std::chrono::steady_clock::now() - last_sync_ < std::chrono::seconds(1)) {
    return;
  }

  while (::fsync(fd_) < 0) {
    if (errno != EINTR) {
      throw std::system_error(errno, std::generic_category(),
                             "Failed to sync AOF");
    }
  }
  dirty_ = false;
  last_sync_ = std::chrono::steady_clock::now();
}

int Aof::sync_timeout_ms() const {
  if (!dirty_ || fsync_ != "everysec") {
    return -1;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
      last_sync_ + std::chrono::seconds(1) - std::chrono::steady_clock::now());
  return remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;
}
