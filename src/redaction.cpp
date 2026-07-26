#include "exchange_probe/contracts.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {
namespace {

[[nodiscard]] bool sensitive_key(std::string_view name) {
  std::string normalized;
  normalized.reserve(name.size());
  for (const char character : name) {
    if ((character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9') ||
        character == '_') {
      normalized.push_back(
          character >= 'A' && character <= 'Z'
              ? static_cast<char>(character - 'A' + 'a')
              : character);
    }
  }
  static constexpr std::array exact{
      "apikey", "api_key", "key", "secret", "signature", "sign",
      "passphrase", "token", "listenkey", "authorization", "accountid",
      "account_id", "uid", "userid", "user_id", "wallet", "address",
  };
  if (std::find(exact.begin(), exact.end(), normalized) != exact.end()) {
    return true;
  }
  static constexpr std::array fragments{
      "secret", "signature", "passphrase", "privatekey",
      "listenkey", "authorization", "accesstoken",
  };
  return std::any_of(
      fragments.begin(),
      fragments.end(),
      [&](std::string_view part) {
        return normalized.find(part) != std::string::npos;
      });
}

[[nodiscard]] boost::json::value shape_impl(
    const boost::json::value& value,
    unsigned depth) {
  if (depth >= 4) {
    return boost::json::string{value.kind() == boost::json::kind::object
                                   ? "object"
                                   : value.kind() == boost::json::kind::array
                                         ? "array"
                                         : "scalar"};
  }
  if (value.is_object()) {
    boost::json::object result;
    std::vector<std::string_view> keys;
    keys.reserve(value.as_object().size());
    for (const auto& item : value.as_object()) {
      keys.emplace_back(item.key());
    }
    std::sort(keys.begin(), keys.end());
    for (const auto key : keys) {
      const auto& item = value.as_object().at(key);
      result[key] = sensitive_key(key)
                        ? boost::json::value(
                              boost::json::string{"<redacted>"})
                        : shape_impl(item, depth + 1);
    }
    return result;
  }
  if (value.is_array()) {
    std::set<std::string> keys;
    const auto& array = value.as_array();
    const auto sample = std::min<std::size_t>(array.size(), 32);
    for (std::size_t index = 0; index < sample; ++index) {
      if (!array[index].is_object()) {
        continue;
      }
      for (const auto& item : array[index].as_object()) {
        keys.insert(
            sensitive_key(item.key())
                ? std::string{"<redacted>"}
                : std::string{item.key()});
      }
    }
    boost::json::array item_keys;
    for (const auto& key : keys) {
      item_keys.emplace_back(key);
    }
    return boost::json::object{
        {"type", "array"},
        {"count", array.size()},
        {"item_keys", std::move(item_keys)},
    };
  }
  if (value.is_null()) {
    return boost::json::string{"null"};
  }
  if (value.is_bool()) {
    return boost::json::string{"bool"};
  }
  if (value.is_string()) {
    return boost::json::string{"string"};
  }
  if (value.is_double()) {
    return boost::json::string{"double"};
  }
  if (value.is_int64()) {
    return boost::json::string{"int64"};
  }
  if (value.is_uint64()) {
    return boost::json::string{"uint64"};
  }
  return boost::json::string{"unknown"};
}

}  // namespace

boost::json::value shape_of(const boost::json::value& value, unsigned depth) {
  return shape_impl(value, depth);
}

std::string bounded_public_frame(std::string_view payload) {
  constexpr std::string_view suffix = "...<truncated>";
  if (payload.size() <= kMaxRawPublicBytes) {
    return std::string{payload};
  }
  static_assert(kMaxRawPublicBytes > suffix.size());
  std::string result{
      payload.substr(0, kMaxRawPublicBytes - suffix.size())};
  result.append(suffix);
  return result;
}

}  // namespace exchange_probe
