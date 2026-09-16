#include "command/command.hpp"
#include "resp/resp.hpp"
#include "store/store.hpp"

std::string Store::handle_config_get(const std::vector<std::string> &args) {
  if (args.size() != 3 || to_upper(args[1]) != "GET") {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'config|get' command")
        .to_bytes();
  }

  std::string param = to_upper(args[2]);

  if (param == "DIR") {
    return RespType::Array({"dir", dir_}).to_bytes();
  }

  if (param == "APPENDONLY") {
    return RespType::Array({"appendonly", appendonly_}).to_bytes();
  }

  if (param == "APPENDDIRNAME") {
    return RespType::Array({"appenddirname", appenddirname_}).to_bytes();
  }

  if (param == "APPENDFILENAME") {
    return RespType::Array({"appendfilename", appendfilename_}).to_bytes();
  }

  if (param == "APPENDFSYNC") {
    return RespType::Array({"appendfsync", appendfsync_}).to_bytes();
  }

  return RespType::Array(std::vector<std::string>{}).to_bytes();
}
