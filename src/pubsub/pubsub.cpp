#include "command/command.hpp"
#include "resp/resp.hpp"
#include "store/store.hpp"

namespace {
std::string subscription_reply(const std::string &kind, RespType channel,
                               std::size_t count) {
  return RespType::NestedArray({RespType::BulkString(kind), std::move(channel),
                               RespType::Integer(static_cast<long long>(count))})
      .to_bytes();
}
}

std::string Store::handle_subscribe(const std::vector<std::string> &args,
                                    ClientState &client) {
  if (args.size() < 2) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'subscribe' command")
        .to_bytes();
  }

  std::string reply;
  for (std::size_t i = 1; i < args.size(); ++i) {
    client.subscriptions.insert(args[i]);
    subscribers_[args[i]].insert(client.fd);
    reply += subscription_reply("subscribe", RespType::BulkString(args[i]),
                                client.subscriptions.size());
  }
  return reply;
}

std::string Store::handle_unsubscribe(const std::vector<std::string> &args,
                                      ClientState &client) {
  std::vector<std::string> channels(args.begin() + 1, args.end());
  if (channels.empty()) {
    channels.assign(client.subscriptions.begin(), client.subscriptions.end());
  }
  if (channels.empty()) {
    return subscription_reply("unsubscribe", RespType::NullBulkString(), 0);
  }

  std::string reply;
  for (const auto &channel : channels) {
    client.subscriptions.erase(channel);
    auto it = subscribers_.find(channel);
    if (it != subscribers_.end()) {
      it->second.erase(client.fd);
      if (it->second.empty()) {
        subscribers_.erase(it);
      }
    }
    reply += subscription_reply("unsubscribe", RespType::BulkString(channel),
                                client.subscriptions.size());
  }
  return reply;
}

std::string Store::handle_publish(const std::vector<std::string> &args) {
  if (args.size() != 3) {
    return RespType::SimpleError(
               "ERR wrong number of arguments for 'publish' command")
        .to_bytes();
  }
  const auto it = subscribers_.find(args[1]);
  if (it == subscribers_.end()) {
    return RespType::Integer(0).to_bytes();
  }

  const auto message = RespType::Array({"message", args[1], args[2]}).to_bytes();
  for (int fd : it->second) {
    pending_messages_.emplace_back(fd, message);
  }
  return RespType::Integer(static_cast<long long>(it->second.size())).to_bytes();
}

void Store::remove_subscriber(int fd) {
  for (auto it = subscribers_.begin(); it != subscribers_.end();) {
    it->second.erase(fd);
    if (it->second.empty()) {
      it = subscribers_.erase(it);
    } else {
      ++it;
    }
  }
}
