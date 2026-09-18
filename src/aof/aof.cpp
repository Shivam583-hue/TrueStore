#include "aof/aof.hpp"

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
} // namespace

Aof::~Aof() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

void Aof::open(const Config &config) {
  if (config.appendonly != "yes") {
    return;
  }

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
