#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class Store {
public:
  std::map<std::string, std::string> gStorage;
  std::map<std::string, std::chrono::steady_clock::time_point> gExpirations;
  std::unordered_map<std::string, std::vector<std::string>> dynamicVector;

  std::string handleSet(const std::vector<std::string> &args);
  std::string handleGet(const std::vector<std::string> &args);
  bool isExpired(const std::string &key);
  std::string handleRPUSH(const std::vector<std::string> &args);
};
