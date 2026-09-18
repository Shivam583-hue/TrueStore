#include "command/command.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "resp/resp.hpp"
#include "store/store.hpp"

std::string to_upper(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return value;
}

bool is_write_command(const std::string &command) {
  static const std::unordered_set<std::string> write_commands = {
      "SET",     "SETNX",    "SETEX",     "PSETEX",  "GETSET",      "GETDEL",
      "APPEND",  "SETRANGE", "MSET",      "MSETNX",  "DEL",         "UNLINK",
      "INCR",    "DECR",     "INCRBY",    "DECRBY",  "INCRBYFLOAT", "EXPIRE",
      "PEXPIRE", "EXPIREAT", "PEXPIREAT", "PERSIST", "RPUSH",       "LPUSH",
      "RPUSHX",  "LPUSHX",   "LPOP",      "RPOP",    "LSET",        "LREM",
      "LTRIM",   "LINSERT",  "XADD",      "XDEL",    "XTRIM",       "SADD",
      "SREM",    "HSET",     "HDEL",      "ZADD",    "ZREM",        "RENAME",
      "COPY",    "FLUSHALL", "FLUSHDB"};

  return write_commands.count(command) > 0;
}

namespace {
std::string dispatch_command(const std::vector<std::string> &args,
                             Store &store, ClientState &client) {
  std::string command = to_upper(args[0]);

  if (command == "PING") {
    if (args.size() > 2) {
      return RespType::SimpleError(
                 "ERR wrong number of arguments for 'ping' command")
          .to_bytes();
    }
    if (!client.subscriptions.empty()) {
      return RespType::Array({"pong", args.size() == 2 ? args[1] : ""}).to_bytes();
    }
    return args.size() == 2 ? RespType::BulkString(args[1]).to_bytes()
                            : RespType::SimpleString("PONG").to_bytes();
  }

  if (command == "SUBSCRIBE")
    return store.handle_subscribe(args, client);

  if (command == "UNSUBSCRIBE")
    return store.handle_unsubscribe(args, client);

  if (command == "PUBLISH")
    return store.handle_publish(args);

  if (command == "ZADD")
    return store.handle_zadd(args);

  if (command == "ZRANK")
    return store.handle_zrank(args);

  if (command == "ZRANGE")
    return store.handle_zrange(args);

  if (command == "ZCARD")
    return store.handle_zcard(args);

  if (command == "ZSCORE")
    return store.handle_zscore(args);

  if (command == "ZREM")
    return store.handle_zrem(args);

  static const std::unordered_set<std::string> incompatible_with_sorted_set = {
      "GET", "INCR", "RPUSH", "LPUSH", "LLEN", "LRANGE", "LPOP",
      "XADD", "XRANGE"};
  if (args.size() >= 2 && incompatible_with_sorted_set.contains(command) &&
      store.key_type(args[1]) == "zset") {
    return RespType::SimpleError(
               "WRONGTYPE Operation against a key holding the wrong kind of value")
        .to_bytes();
  }

  if (command == "ECHO") {
    if (args.size() != 2) {
      return RespType::SimpleError(
                 "ERR wrong number of arguments for 'echo' command")
          .to_bytes();
    }
    return RespType::BulkString(args[1]).to_bytes();
  }

  if (command == "SET")
    return store.handle_set(args);

  if (command == "GET")
    return store.handle_get(args);

  if (command == "RPUSH")
    return store.handle_rpush(args);

  if (command == "LRANGE")
    return store.handle_lrange(args);

  if (command == "LPUSH")
    return store.handle_lpush(args);

  if (command == "LLEN")
    return store.handle_llen(args);

  if (command == "LPOP")
    return store.handle_lpop(args);

  if (command == "BLPOP")
    return store.handle_blpop(args);

  if (command == "TYPE")
    return store.handle_type(args);

  if (command == "KEYS")
    return store.handle_keys(args);

  if (command == "XADD")
    return store.handle_xadd(args);

  if (command == "XRANGE")
    return store.handle_xrange(args);

  if (command == "XREAD")
    return store.handle_xread(args);

  if (command == "INCR")
    return store.handle_incr(args);

  if (command == "INFO")
    return store.handle_info(args);

  if (command == "REPLCONF")
    return store.handle_replconf(args);

  if (command == "PSYNC")
    return store.handle_psync(args);

  if (command == "WAIT")
    return store.handle_wait(args);

  if (command == "CONFIG")
    return store.handle_config_get(args);

  return RespType::SimpleError("ERR unknown command '" + args[0] + "'")
      .to_bytes();
}
} // namespace

std::string handle_command(const std::vector<std::string> &args, Store &store,
                           ClientState &client) {
  if (args.empty()) {
    return RespType::SimpleError("ERR empty command").to_bytes();
  }

  std::string command = to_upper(args[0]);

  if (!client.subscriptions.empty()) {
    static const std::unordered_set<std::string> allowed = {
        "SUBSCRIBE", "UNSUBSCRIBE", "PSUBSCRIBE", "PUNSUBSCRIBE",
        "PING", "QUIT", "RESET"};
    if (!allowed.contains(command)) {
      std::string name = args[0];
      for (char &c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return RespType::SimpleError("ERR Can't execute '" + name +
                                   "' in subscribed mode").to_bytes();
    }
  }

  if (command == "QUIT" || command == "RESET") {
    if (args.size() != 1) {
      return RespType::SimpleError("ERR wrong number of arguments").to_bytes();
    }
    store.remove_subscriber(client.fd);
    client.subscriptions.clear();
    client.in_multi = false;
    client.queued.clear();
    client.watched.clear();
    client.close_after_reply = command == "QUIT";
    return RespType::SimpleString(command == "QUIT" ? "OK" : "RESET").to_bytes();
  }

  if (command == "MULTI") {
    if (args.size() != 1) {
      return RespType::SimpleError(
                 "ERR wrong number of arguments for 'multi' command")
          .to_bytes();
    }

    if (client.in_multi) {
      return RespType::SimpleError("ERR MULTI calls can not be nested")
          .to_bytes();
    }

    client.in_multi = true;
    return RespType::SimpleString("OK").to_bytes();
  }

  if (command == "WATCH") {
    if (args.size() < 2) {
      return RespType::SimpleError(
                 "ERR wrong number of arguments for 'watch' command")
          .to_bytes();
    }

    if (client.in_multi) {
      return RespType::SimpleError("ERR WATCH inside MULTI is not allowed")
          .to_bytes();
    }

    for (std::size_t i = 1; i < args.size(); ++i) {
      client.watched.emplace_back(args[i], store.peek(args[i]));
    }

    return RespType::SimpleString("OK").to_bytes();
  }

  if (command == "UNWATCH") {
    client.watched.clear();
    return RespType::SimpleString("OK").to_bytes();
  }

  if (command == "DISCARD") {
    if (!client.in_multi) {
      return RespType::SimpleError("ERR DISCARD without MULTI").to_bytes();
    }

    client.in_multi = false;
    client.queued.clear();
    client.watched.clear();
    return RespType::SimpleString("OK").to_bytes();
  }

  if (command == "EXEC") {
    if (!client.in_multi) {
      return RespType::SimpleError("ERR EXEC without MULTI").to_bytes();
    }

    const std::vector<std::vector<std::string>> queued =
        std::move(client.queued);

    bool dirty = false;

    for (const auto &[key, snapshot] : client.watched) {
      if (store.peek(key) != snapshot) {
        dirty = true;
        break;
      }
    }

    client.in_multi = false;
    client.queued.clear();
    client.watched.clear();

    if (dirty) {
      return RespType::NullArray().to_bytes();
    }

    std::string reply = "*" + std::to_string(queued.size()) + "\r\n";

    for (const std::vector<std::string> &queued_args : queued) {
      std::string result = dispatch_command(queued_args, store, client);

      if (store.take_pending_block()) {
        result = RespType::NullArray().to_bytes();
      }

      if (is_write_command(to_upper(queued_args[0])) &&
          (result.empty() || result[0] != '-')) {
        store.queue_propagation(queued_args);
      }

      reply += result;
    }

    return reply;
  }

  if (client.in_multi) {
    client.queued.push_back(args);
    return RespType::SimpleString("QUEUED").to_bytes();
  }

  return dispatch_command(args, store, client);
}
