#include <string>
#include <vector>

#include "resp/resp.hpp"
#include "store/store.hpp"

std::string to_upper(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return value;
}

std::string handle_command(const std::vector<std::string> &args, Store &store) {
  if (args.empty()) {
    return RespType::SimpleError("ERR empty command").to_bytes();
  }

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

  if (command == "SET") {
    return store.handle_set(args);
  }

  if (command == "GET") {
    return store.handle_get(args);
  }

  if (command == "RPUSH") {
    return store.handle_rpush(args);
  }

  return RespType::SimpleError("ERR unknown command '" + args[0] + "'")
      .to_bytes();
}
