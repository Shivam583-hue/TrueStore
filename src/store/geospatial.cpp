#include "command/command.hpp"
#include "resp/resp.hpp"
#include "store/store.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <stdexcept>

namespace {
constexpr double kMaxLatitude = 85.05112878;
constexpr double kCoordinateScale = 67108864;
constexpr double kEarthRadius = 6372797.560856;

struct Coordinates {
  double longitude;
  double latitude;
};

std::string error(const std::string &message) {
  return RespType::SimpleError("ERR " + message).to_bytes();
}

std::string arity_error(const std::string &command) {
  return error("wrong number of arguments for '" + command + "' command");
}

std::string wrong_type() {
  return RespType::SimpleError("WRONGTYPE Operation against a key holding the "
                               "wrong kind of value")
      .to_bytes();
}

bool parse_number(const std::string &text, double &value) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text.front()))) {
    return false;
  }
  try {
    std::size_t consumed;
    value = std::stod(text, &consumed);
    return consumed == text.size() && !std::isnan(value);
  } catch (const std::exception &) {
    return false;
  }
}

std::string parse_coordinates(const std::string &longitude,
                              const std::string &latitude, Coordinates &point) {
  if (!parse_number(longitude, point.longitude) ||
      !parse_number(latitude, point.latitude)) {
    return error("value is not a valid float");
  }
  if (point.longitude < -180 || point.longitude > 180 ||
      point.latitude < -kMaxLatitude || point.latitude > kMaxLatitude) {
    return error("invalid longitude,latitude pair " +
                 std::to_string(point.longitude) + "," +
                 std::to_string(point.latitude));
  }
  return {};
}

std::uint64_t encode_coordinates(Coordinates point) {
  const auto longitude = static_cast<std::uint64_t>((point.longitude + 180) /
                                                    360 * kCoordinateScale);
  const auto latitude = static_cast<std::uint64_t>(
      (point.latitude + kMaxLatitude) / (2 * kMaxLatitude) * kCoordinateScale);
  std::uint64_t score = 0;
  for (unsigned bit = 0; bit <= 26; ++bit) {
    score |= ((longitude >> bit) & 1) << (2 * bit + 1);
    score |= ((latitude >> bit) & 1) << (2 * bit);
  }
  return score;
}

std::optional<Coordinates> decode_coordinates(double score) {
  if (!std::isfinite(score) || score < 0 || score >= std::ldexp(1.0, 63)) {
    return std::nullopt;
  }
  const auto hash = static_cast<std::uint64_t>(score);
  std::uint64_t longitude = 0, latitude = 0;
  for (unsigned bit = 0; bit < 32; ++bit) {
    longitude |= ((hash >> (2 * bit + 1)) & 1) << bit;
    latitude |= ((hash >> (2 * bit)) & 1) << bit;
  }
  const double lon_min = -180 + longitude / kCoordinateScale * 360;
  const double lon_max = -180 + (longitude + 1) / kCoordinateScale * 360;
  const double lat_min =
      -kMaxLatitude + latitude / kCoordinateScale * (2 * kMaxLatitude);
  const double lat_max =
      -kMaxLatitude + (latitude + 1) / kCoordinateScale * (2 * kMaxLatitude);
  return Coordinates{
      std::clamp((lon_min + lon_max) / 2, -180.0, 180.0),
      std::clamp((lat_min + lat_max) / 2, -kMaxLatitude, kMaxLatitude)};
}

std::string format_number(double value, bool distance = false) {
  char buffer[128];
  const auto result =
      distance ? std::to_chars(std::begin(buffer), std::end(buffer), value,
                               std::chars_format::fixed, 4)
               : std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (result.ec != std::errc{}) {
    throw std::runtime_error("Failed to format geospatial number");
  }
  return std::string(buffer, result.ptr);
}

RespType coordinates_reply(Coordinates point) {
  return RespType::Array(
      {format_number(point.longitude), format_number(point.latitude)});
}

double unit_in_meters(const std::string &unit) {
  const auto name = to_upper(unit);
  if (name == "M") {
    return 1;
  }
  if (name == "KM") {
    return 1000;
  }
  if (name == "MI") {
    return 1609.34;
  }
  if (name == "FT") {
    return 0.3048;
  }
  return 0;
}

std::string unit_error() {
  return error("unsupported unit provided. please use M, KM, FT, MI");
}

double distance_in_meters(Coordinates first, Coordinates second) {
  constexpr double radians = std::numbers::pi / 180;
  const double lat1 = first.latitude * radians;
  const double lat2 = second.latitude * radians;
  const double sin_lat =
      std::sin((second.latitude - first.latitude) * radians / 2);
  const double sin_lon =
      std::sin((second.longitude - first.longitude) * radians / 2);
  const double haversine =
      sin_lat * sin_lat + std::cos(lat1) * std::cos(lat2) * sin_lon * sin_lon;
  return 2 * kEarthRadius *
         std::asin(std::sqrt(std::clamp(haversine, 0.0, 1.0)));
}
}

std::string Store::handle_geoadd(const std::vector<std::string> &args) {
  if (args.size() < 5) {
    return arity_error("geoadd");
  }
  bool nx = false, xx = false, ch = false;
  std::size_t start = 2;
  while (start < args.size()) {
    const auto option = to_upper(args[start]);
    if (option == "NX") {
      nx = true;
    } else if (option == "XX") {
      xx = true;
    } else if (option == "CH") {
      ch = true;
    } else {
      break;
    }
    ++start;
  }
  if ((nx && xx) || start == args.size() || (args.size() - start) % 3 != 0) {
    return error("syntax error");
  }

  std::vector<std::pair<double, std::string>> entries;
  for (std::size_t i = start; i < args.size(); i += 3) {
    Coordinates point;
    const auto failure = parse_coordinates(args[i], args[i + 1], point);
    if (!failure.empty()) {
      return failure;
    }
    entries.emplace_back(static_cast<double>(encode_coordinates(point)),
                         args[i + 2]);
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  if (xx && !sorted_sets_.contains(args[1])) {
    return RespType::Integer(0).to_bytes();
  }

  auto &set = sorted_sets_[args[1]];
  long long added = 0, changed = 0;
  for (const auto &[score, member] : entries) {
    const auto existing = set.scores.find(member);
    const bool present = existing != set.scores.end();
    if ((nx && present) || (xx && !present) ||
        (present && existing->second == score)) {
      continue;
    }
    added += set.add(score, member);
    ++changed;
  }
  return RespType::Integer(ch ? changed : added).to_bytes();
}

std::string Store::handle_geopos(const std::vector<std::string> &args) {
  if (args.size() < 2) {
    return arity_error("geopos");
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto set = sorted_sets_.find(args[1]);
  std::vector<RespType> positions;
  for (std::size_t i = 2; i < args.size(); ++i) {
    std::optional<Coordinates> point;
    if (set != sorted_sets_.end()) {
      const auto member = set->second.scores.find(args[i]);
      if (member != set->second.scores.end()) {
        point = decode_coordinates(member->second);
      }
    }
    positions.push_back(point ? coordinates_reply(*point)
                              : RespType::NullArray());
  }
  return RespType::NestedArray(std::move(positions)).to_bytes();
}

std::string Store::handle_geodist(const std::vector<std::string> &args) {
  if (args.size() != 4 && args.size() != 5) {
    return arity_error("geodist");
  }
  const double unit = args.size() == 5 ? unit_in_meters(args[4]) : 1;
  if (unit == 0) {
    return unit_error();
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }
  const auto set = sorted_sets_.find(args[1]);
  if (set == sorted_sets_.end()) {
    return RespType::NullBulkString().to_bytes();
  }
  const auto first = set->second.scores.find(args[2]);
  const auto second = set->second.scores.find(args[3]);
  if (first == set->second.scores.end() || second == set->second.scores.end()) {
    return RespType::NullBulkString().to_bytes();
  }
  const auto from = decode_coordinates(first->second);
  const auto to = decode_coordinates(second->second);
  if (!from || !to) {
    return RespType::NullBulkString().to_bytes();
  }
  return RespType::BulkString(
             format_number(distance_in_meters(*from, *to) / unit, true))
      .to_bytes();
}

std::string Store::handle_geosearch(const std::vector<std::string> &args) {
  if (args.size() < 7) {
    return arity_error("geosearch");
  }
  if (wrong_sorted_set_type(args[1])) {
    return wrong_type();
  }

  std::optional<Coordinates> center;
  std::optional<std::string> from_member;
  std::optional<double> radius;
  double unit = 1;
  int sort = 0;
  long long count = 0;
  bool any = false, with_dist = false, with_hash = false, with_coord = false;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const auto option = to_upper(args[i]);
    if (option == "FROMLONLAT" && i + 2 < args.size() && !from_member) {
      Coordinates point;
      const auto failure = parse_coordinates(args[i + 1], args[i + 2], point);
      if (!failure.empty()) {
        return failure;
      }
      center = point;
      i += 2;
    } else if (option == "FROMMEMBER" && i + 1 < args.size() && !center) {
      from_member = args[++i];
    } else if (option == "BYRADIUS" && i + 2 < args.size()) {
      double value;
      if (!parse_number(args[i + 1], value) || !std::isfinite(value)) {
        return error("need numeric radius");
      }
      if (value < 0) {
        return error("radius cannot be negative");
      }
      unit = unit_in_meters(args[i + 2]);
      if (unit == 0) {
        return unit_error();
      }
      radius = value;
      i += 2;
    } else if (option == "ASC") {
      sort = 1;
    } else if (option == "DESC") {
      sort = -1;
    } else if (option == "COUNT" && i + 1 < args.size()) {
      const auto &text = args[++i];
      const auto result =
          std::from_chars(text.data(), text.data() + text.size(), count);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return error("value is not an integer or out of range");
      }
      if (count <= 0) {
        return error("COUNT must be > 0");
      }
    } else if (option == "ANY") {
      any = true;
    } else if (option == "WITHDIST") {
      with_dist = true;
    } else if (option == "WITHHASH") {
      with_hash = true;
    } else if (option == "WITHCOORD") {
      with_coord = true;
    } else {
      return error("syntax error");
    }
  }
  if (!center && !from_member) {
    return error("exactly one of FROMMEMBER or FROMLONLAT can be specified "
                 "for GEOSEARCH");
  }
  if (!radius) {
    return error("BYRADIUS is required for GEOSEARCH");
  }
  if (any && count == 0) {
    return error("the ANY argument requires COUNT argument");
  }
  const auto set = sorted_sets_.find(args[1]);
  if (set == sorted_sets_.end()) {
    return RespType::Array({}).to_bytes();
  }
  if (from_member) {
    const auto member = set->second.scores.find(*from_member);
    if (member != set->second.scores.end()) {
      center = decode_coordinates(member->second);
    }
    if (!center) {
      return error("could not decode requested zset member");
    }
  }

  struct Match {
    std::string member;
    double score;
    Coordinates point;
    double distance;
  };
  std::vector<Match> matches;
  for (const auto &[score, member] : set->second.ordered) {
    const auto point = decode_coordinates(score);
    if (!point) {
      continue;
    }
    const double distance = distance_in_meters(*center, *point) / unit;
    if (distance > *radius) {
      continue;
    }
    matches.push_back({member, score, *point, distance});
    if (any && matches.size() == static_cast<std::size_t>(count)) {
      break;
    }
  }
  if (count && !any && sort == 0) {
    sort = 1;
  }
  if (sort) {
    std::stable_sort(
        matches.begin(), matches.end(), [sort](const Match &a, const Match &b) {
          return sort > 0 ? a.distance < b.distance : a.distance > b.distance;
        });
  }
  if (count && matches.size() > static_cast<std::size_t>(count)) {
    matches.resize(static_cast<std::size_t>(count));
  }

  std::vector<RespType> results;
  for (const auto &match : matches) {
    if (!with_dist && !with_hash && !with_coord) {
      results.push_back(RespType::BulkString(match.member));
      continue;
    }
    std::vector<RespType> details{RespType::BulkString(match.member)};
    if (with_dist) {
      details.push_back(
          RespType::BulkString(format_number(match.distance, true)));
    }
    if (with_hash) {
      details.push_back(RespType::Integer(static_cast<long long>(match.score)));
    }
    if (with_coord) {
      details.push_back(coordinates_reply(match.point));
    }
    results.push_back(RespType::NestedArray(std::move(details)));
  }
  return RespType::NestedArray(std::move(results)).to_bytes();
}
