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
};
