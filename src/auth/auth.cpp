#include "store/store.hpp"
#include "command/command.hpp"
#include "resp/resp.hpp"

#include <algorithm>
#include <array>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdexcept>

namespace {
std::string password_hash(const std::string &password) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
  unsigned int size = 0;
  if (EVP_Digest(password.data(), password.size(), digest.data(), &size,
                 EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("Failed to hash password");
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string hash;
  hash.reserve(size * 2);
  for (unsigned int i = 0; i < size; ++i) {
    hash.push_back(hex[digest[i] >> 4]);
    hash.push_back(hex[digest[i] & 15]);
  }
  return hash;
}

std::string arity_error(const std::string &command) {
  return RespType::SimpleError("ERR wrong number of arguments for '" + command +
                               "' command").to_bytes();
}
}

std::string Store::handle_acl(const std::vector<std::string> &args) {
  if (args.size() < 2) {
    return arity_error("acl");
  }
  const auto subcommand = to_upper(args[1]);
  if (subcommand == "WHOAMI") {
    if (args.size() != 2) {
      return arity_error("acl|whoami");
    }
    return RespType::BulkString("default").to_bytes();
  }
  if (subcommand == "GETUSER") {
    if (args.size() != 3) {
      return arity_error("acl|getuser");
    }
    if (args[2] != "default") {
      return RespType::NullBulkString().to_bytes();
    }
    std::vector<std::string> flags{default_user_enabled_ ? "on" : "off"};
    if (default_user_nopass_) {
      flags.push_back("nopass");
    }
    return RespType::NestedArray({
        RespType::BulkString("flags"), RespType::Array(std::move(flags)),
        RespType::BulkString("passwords"), RespType::Array(default_user_passwords_),
        RespType::BulkString("commands"), RespType::BulkString("+@all"),
        RespType::BulkString("keys"), RespType::BulkString("~*"),
        RespType::BulkString("channels"), RespType::BulkString("&*"),
        RespType::BulkString("selectors"), RespType::Array({})}).to_bytes();
  }
  if (subcommand != "SETUSER") {
    return RespType::SimpleError("ERR unknown ACL subcommand").to_bytes();
  }
  if (args.size() < 3) {
    return arity_error("acl|setuser");
  }
  if (args[2] != "default") {
    return RespType::SimpleError("ERR only the default user is supported")
        .to_bytes();
  }

  auto passwords = default_user_passwords_;
  bool enabled = default_user_enabled_, nopass = default_user_nopass_;
  for (std::size_t i = 3; i < args.size(); ++i) {
    const auto &rule = args[i];
    if (rule == "on" || rule == "off") {
      enabled = rule == "on";
    } else if (rule == "nopass" || rule == "resetpass") {
      passwords.clear();
      nopass = rule == "nopass";
    } else if (!rule.empty() && (rule[0] == '>' || rule[0] == '<' ||
                                 rule[0] == '#' || rule[0] == '!')) {
      std::string hash;
      if (rule[0] == '>' || rule[0] == '<') {
        hash = password_hash(rule.substr(1));
      } else {
        hash = rule.substr(1);
        if (hash.size() != 64 ||
            hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
          return RespType::SimpleError("ERR password hash must contain 64 lowercase hex characters")
              .to_bytes();
        }
      }
      if (rule[0] == '>' || rule[0] == '#') {
        if (std::find(passwords.begin(), passwords.end(), hash) == passwords.end()) {
          passwords.push_back(std::move(hash));
        }
        nopass = false;
      } else {
        std::erase(passwords, hash);
      }
    } else {
      return RespType::SimpleError("ERR unsupported ACL rule").to_bytes();
    }
  }
  default_user_enabled_ = enabled;
  default_user_nopass_ = nopass;
  default_user_passwords_ = std::move(passwords);
  return RespType::SimpleString("OK").to_bytes();
}

std::string Store::handle_auth(const std::vector<std::string> &args,
                              ClientState &client) {
  if (args.size() != 2 && args.size() != 3) {
    return arity_error("auth");
  }
  if (args.size() == 2 && default_user_nopass_) {
    return RespType::SimpleError(
        "ERR AUTH <password> called without any password configured for the "
        "default user. Are you sure your configuration is correct?").to_bytes();
  }
  bool matched = default_user_nopass_;
  const auto hash = password_hash(args.back());
  for (const auto &password : default_user_passwords_) {
    matched |= CRYPTO_memcmp(hash.data(), password.data(), hash.size()) == 0;
  }
  if (!default_user_enabled_ || (args.size() == 3 && args[1] != "default") ||
      !matched) {
    return RespType::SimpleError(
        "WRONGPASS invalid username-password pair or user is disabled.")
        .to_bytes();
  }
  client.authenticated = true;
  return RespType::SimpleString("OK").to_bytes();
}
