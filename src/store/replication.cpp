#include "resp/resp.hpp"
#include "store/store.hpp"

#include <random>

namespace {
std::string generate_replid() {
  static const char *hex_digits = "0123456789abcdef";

  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<int> dist(0, 15);

  std::string id(40, '0');

  for (char &c : id) {
    c = hex_digits[dist(gen)];
  }

  return id;
}

std::string hex_to_bytes(const std::string &hex) {
  std::string bytes;
  bytes.reserve(hex.size() / 2);

  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(
        static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }

  return bytes;
}

const std::string kEmptyRdbHex =
    "524544495330303131fa0972656469732d76657205372e322e30fa0a7265646973"
    "2d62697473c040fa056374696d65c26d08bc65fa08757365642d6d656dc2b0c410"
    "00fa08616f662d62617365c000fff06e3bfec0ff5aa2";
} // namespace

void Store::init(const Config &config) {
  appendonly_ = config.appendonly;
  appenddirname_ = config.appenddirname;
  appendfilename_ = config.appendfilename;
  appendfsync_ = config.appendfsync;

  is_replica_ = config.is_replica;
  master_host_ = config.master_host;
  master_port_ = config.master_port;
  dir_ = config.dir;
  dbfilename_ = config.dbfilename;
  master_replid_ = generate_replid();
  master_repl_offset_ = 0;
}

std::string Store::handle_replconf(const std::vector<std::string> &args) {
  (void)args;

  return RespType::SimpleString("OK").to_bytes();
}

std::string Store::handle_wait(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'wait' command")
        .to_bytes();
  }

  long long numreplicas;
  long long timeout_ms;

  try {
    numreplicas = std::stoll(args[1]);
    timeout_ms = std::stoll(args[2]);
  } catch (...) {
    return RespType::SimpleError("ERR value is not an integer or out of range")
        .to_bytes();
  }

  BlockRequest block;
  block.kind = BlockKind::Wait;
  block.count = numreplicas > 0 ? static_cast<std::size_t>(numreplicas) : 0;
  block.timeout = timeout_ms / 1000.0;

  pending_block_ = block;

  return {};
}

std::string Store::handle_psync(const std::vector<std::string> &args) {
  (void)args;

  std::string fullresync = "FULLRESYNC " + master_replid_ + " " +
                           std::to_string(master_repl_offset_);

  std::string reply = RespType::SimpleString(fullresync).to_bytes();

  std::string rdb = hex_to_bytes(kEmptyRdbHex);
  reply += "$" + std::to_string(rdb.size()) + "\r\n" + rdb;

  return reply;
}

std::string Store::handle_info(const std::vector<std::string> &args) {
  (void)args;

  std::string info = "# Replication\r\n";

  if (is_replica_) {
    info += "role:slave\r\n";
    info += "master_host:" + master_host_ + "\r\n";
    info += "master_port:" + std::to_string(master_port_) + "\r\n";
  } else {
    info += "role:master\r\n";
  }

  info += "master_replid:" + master_replid_ + "\r\n";
  info += "master_repl_offset:" + std::to_string(master_repl_offset_) + "\r\n";

  return RespType::BulkString(info).to_bytes();
}
