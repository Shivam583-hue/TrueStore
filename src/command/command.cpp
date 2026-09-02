#include "command/command.hpp"

#include <cctype>
#include <string>
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

namespace {
std::string dispatch_command(const std::vector<std::string> &args,
                             Store &store) {
  std::string command = to_upper(args[0]);

  if (command == "PING") {
    return RespType::SimpleString("PONG").to_bytes();
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

  if (command == "XADD")
    return store.handle_xadd(args);

  if (command == "XRANGE")
    return store.handle_xrange(args);

  if (command == "XREAD")
    return store.handle_xread(args);

  if (command == "INCR")
    return store.handle_incr(args);

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

  if (command == "DISCARD") {
    if (!client.in_multi) {
      return RespType::SimpleError("ERR DISCARD without MULTI").to_bytes();
    }

    client.in_multi = false;
    client.queued.clear();
    return RespType::SimpleString("OK").to_bytes();
  }

  if (command == "EXEC") {
    if (!client.in_multi) {
      return RespType::SimpleError("ERR EXEC without MULTI").to_bytes();
    }

    const std::vector<std::vector<std::string>> queued =
        std::move(client.queued);

    client.in_multi = false;
    client.queued.clear();

    std::string reply = "*" + std::to_string(queued.size()) + "\r\n";

    for (const std::vector<std::string> &queued_args : queued) {
      std::string result = dispatch_command(queued_args, store);

      if (store.take_pending_block()) {
        result = RespType::NullArray().to_bytes();
      }

      reply += result;
    }

    return reply;
  }

  if (client.in_multi) {
    client.queued.push_back(args);
    return RespType::SimpleString("QUEUED").to_bytes();
  }

  return dispatch_command(args, store);
}
