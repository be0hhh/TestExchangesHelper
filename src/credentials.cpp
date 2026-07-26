#include "exchange_probe/credentials.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string_view>
#include <utility>

extern char** environ;

namespace exchange_probe {
namespace {

void cleanse(std::string& value) noexcept {
  if (!value.empty()) {
    OPENSSL_cleanse(value.data(), value.size());
    value.clear();
  }
}

[[nodiscard]] std::string trim(std::string_view input) {
  const auto first = input.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = input.find_last_not_of(" \t\r\n");
  return std::string{input.substr(first, last - first + 1)};
}

[[nodiscard]] std::string append_query(
    std::string_view path,
    std::string_view query) {
  std::string result{path};
  result.push_back(path.find('?') == std::string_view::npos ? '?' : '&');
  result.append(query);
  return result;
}

[[nodiscard]] std::string_view query_of(std::string_view path) {
  const auto separator = path.find('?');
  return separator == std::string_view::npos
             ? std::string_view{}
             : path.substr(separator + 1);
}

[[nodiscard]] std::string_view path_without_query(std::string_view path) {
  const auto separator = path.find('?');
  return path.substr(0, separator);
}

[[nodiscard]] const EVP_MD* digest_for(std::string_view name) {
  return name == "sha512" ? EVP_sha512() : EVP_sha256();
}

[[nodiscard]] std::string hmac_bytes(
    std::string_view secret,
    std::string_view payload,
    std::string_view digest_name) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> result{};
  unsigned result_size = 0;
  const auto* bytes = HMAC(
      digest_for(digest_name),
      secret.data(),
      static_cast<int>(secret.size()),
      reinterpret_cast<const unsigned char*>(payload.data()),
      payload.size(),
      result.data(),
      &result_size);
  if (bytes == nullptr) {
    return {};
  }
  return std::string{
      reinterpret_cast<const char*>(result.data()),
      result_size};
}

[[nodiscard]] std::string hex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.resize(bytes.size() * 2);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const auto value = static_cast<unsigned char>(bytes[index]);
    result[index * 2] = digits[value >> 4U];
    result[index * 2 + 1] = digits[value & 0x0fU];
  }
  return result;
}

[[nodiscard]] std::string base64(std::string_view bytes) {
  if (bytes.empty()) {
    return {};
  }
  std::string result;
  result.resize(4U * ((bytes.size() + 2U) / 3U));
  const auto encoded = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(result.data()),
      reinterpret_cast<const unsigned char*>(bytes.data()),
      static_cast<int>(bytes.size()));
  if (encoded < 0) {
    return {};
  }
  result.resize(static_cast<std::size_t>(encoded));
  return result;
}

[[nodiscard]] std::string sha512_empty_hex() {
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned size = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    return {};
  }
  const bool ok =
      EVP_DigestInit_ex(context, EVP_sha512(), nullptr) == 1 &&
      EVP_DigestFinal_ex(context, output.data(), &size) == 1;
  EVP_MD_CTX_free(context);
  return ok ? hex(std::string_view{
                  reinterpret_cast<const char*>(output.data()),
                  size})
            : std::string{};
}

[[nodiscard]] std::string iso8601_milliseconds(std::int64_t now_ms) {
  const std::time_t seconds = static_cast<std::time_t>(now_ms / 1000);
  std::tm utc{};
  if (gmtime_r(&seconds, &utc) == nullptr) {
    return {};
  }
  char prefix[32]{};
  const auto length = std::strftime(
      prefix,
      sizeof(prefix),
      "%Y-%m-%dT%H:%M:%S",
      &utc);
  if (length == 0) {
    return {};
  }
  char result[40]{};
  const auto milliseconds = static_cast<int>((now_ms % 1000 + 1000) % 1000);
  const auto written = std::snprintf(
      result,
      sizeof(result),
      "%s.%03dZ",
      prefix,
      milliseconds);
  return written > 0 && static_cast<std::size_t>(written) < sizeof(result)
             ? std::string{result, static_cast<std::size_t>(written)}
             : std::string{};
}

[[nodiscard]] std::optional<std::string> lookup(
    const Environment& environment,
    std::string_view name) {
  const auto found = environment.find(std::string{name});
  if (found == environment.end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace

Environment::~Environment() {
  for (auto& [name, value] : *this) {
    static_cast<void>(name);
    cleanse(value);
  }
}

Environment::Environment(Environment&& other) noexcept
    : std::map<std::string, std::string>{std::move(other)} {}

Environment& Environment::operator=(Environment&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  for (auto& [name, value] : *this) {
    static_cast<void>(name);
    cleanse(value);
  }
  std::map<std::string, std::string>::operator=(std::move(other));
  return *this;
}

Credentials::~Credentials() {
  cleanse(key);
  cleanse(secret);
  cleanse(passphrase);
  cleanse(account_id);
}

Credentials::Credentials(Credentials&& other) noexcept
    : key(std::move(other.key)),
      secret(std::move(other.secret)),
      passphrase(std::move(other.passphrase)),
      account_id(std::move(other.account_id)) {
  cleanse(other.key);
  cleanse(other.secret);
  cleanse(other.passphrase);
  cleanse(other.account_id);
}

Credentials& Credentials::operator=(Credentials&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  cleanse(key);
  cleanse(secret);
  cleanse(passphrase);
  cleanse(account_id);
  key = std::move(other.key);
  secret = std::move(other.secret);
  passphrase = std::move(other.passphrase);
  account_id = std::move(other.account_id);
  cleanse(other.key);
  cleanse(other.secret);
  cleanse(other.passphrase);
  cleanse(other.account_id);
  return *this;
}

Environment load_environment(
    const std::optional<std::filesystem::path>& env_file,
    std::string& error) {
  Environment environment;
  static const std::regex credential_name{
      R"(^[A-Z0-9]+(?:_[A-Z0-9]+)*_API(?:_[0-9]+)?_(?:KEY|SECRET|PASSPHRASE|ACCOUNT_ID|UID|USER_ID)$)"};
  if (environ != nullptr) {
    for (char** entry = environ; *entry != nullptr; ++entry) {
      const std::string_view item{*entry};
      const auto separator = item.find('=');
      if (separator != std::string_view::npos && separator > 0) {
        const std::string name{item.substr(0, separator)};
        if (!std::regex_match(name, credential_name)) {
          continue;
        }
        environment.emplace(
            name,
            std::string{item.substr(separator + 1)});
      }
    }
  }
  if (!env_file.has_value()) {
    return environment;
  }

  std::ifstream input{*env_file};
  if (!input) {
    error = "env_file_open_failed";
    return {};
  }
  std::string line;
  while (std::getline(input, line)) {
    auto text = trim(line);
    if (text.empty() || text.front() == '#') {
      continue;
    }
    if (text.starts_with("export ")) {
      text = trim(std::string_view{text}.substr(7));
    }
    const auto separator = text.find('=');
    if (separator == std::string::npos || separator == 0) {
      error = "env_file_invalid_line";
      return {};
    }
    auto name = trim(std::string_view{text}.substr(0, separator));
    auto value = trim(std::string_view{text}.substr(separator + 1));
    if (name.empty() || !std::regex_match(name, credential_name)) {
      error = "env_file_credential_name_invalid";
      return {};
    }
    if (value.size() >= 2 &&
        ((value.front() == '\'' && value.back() == '\'') ||
         (value.front() == '"' && value.back() == '"'))) {
      value = value.substr(1, value.size() - 2);
    }
    if (!environment.contains(name)) {
      environment.emplace(std::move(name), std::move(value));
    }
  }
  if (!input.eof()) {
    error = "env_file_read_failed";
    return {};
  }
  return environment;
}

std::vector<unsigned> configured_slots(
    const Environment& environment,
    std::string_view prefix) {
  const std::regex pattern{
      "^" + std::string{prefix} + R"(_([0-9]+)_(KEY|SECRET)$)"};
  std::vector<unsigned> slots;
  if (environment.contains(std::string{prefix} + "_KEY") ||
      environment.contains(std::string{prefix} + "_SECRET")) {
    slots.push_back(0);
  }
  for (const auto& [name, ignored] : environment) {
    static_cast<void>(ignored);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) {
      continue;
    }
    unsigned slot = 0;
    const auto text = match[1].str();
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        slot);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
        slot >= 1 && slot <= 255 &&
        std::find(slots.begin(), slots.end(), slot) == slots.end()) {
      slots.push_back(slot);
    }
  }
  std::sort(slots.begin(), slots.end());
  return slots;
}

std::optional<Credentials> resolve_credentials(
    const Environment& environment,
    std::string_view prefix,
    unsigned slot) {
  const auto base =
      slot == 0
          ? std::string{prefix}
          : std::string{prefix} + "_" + std::to_string(slot);
  const auto key = lookup(environment, base + "_KEY");
  const auto secret = lookup(environment, base + "_SECRET");
  if (!key.has_value() || key->empty() ||
      !secret.has_value() || secret->empty()) {
    return std::nullopt;
  }
  Credentials result;
  result.key = *key;
  result.secret = *secret;
  result.passphrase = lookup(environment, base + "_PASSPHRASE").value_or("");
  result.account_id =
      lookup(environment, base + "_ACCOUNT_ID")
          .value_or(lookup(environment, base + "_UID")
                        .value_or(lookup(environment, base + "_USER_ID")
                                      .value_or("")));
  return result;
}

std::optional<SignedRequest> sign_request(
    const RestCase& probe_case,
    const Credentials& credentials,
    std::int64_t now_ms,
    std::string& error) {
  SignedRequest request{.path = probe_case.path};
  switch (probe_case.auth) {
    case AuthKind::None:
      error = "private_auth_not_declared";
      return std::nullopt;
    case AuthKind::BinanceHmac: {
      const auto unsigned_path = append_query(
          probe_case.path,
          "timestamp=" + std::to_string(now_ms) + "&recvWindow=5000");
      const auto signature =
          hex(hmac_bytes(credentials.secret, query_of(unsigned_path), "sha256"));
      if (signature.empty()) {
        error = "binance_hmac_failed";
        return std::nullopt;
      }
      request.path = append_query(unsigned_path, "signature=" + signature);
      request.headers.emplace("X-MBX-APIKEY", credentials.key);
      return request;
    }
    case AuthKind::BybitHmac: {
      const auto timestamp = std::to_string(now_ms);
      constexpr std::string_view window = "5000";
      const auto signature = hex(hmac_bytes(
          credentials.secret,
          timestamp + credentials.key + std::string{window} +
              std::string{query_of(probe_case.path)},
          "sha256"));
      if (signature.empty()) {
        error = "bybit_hmac_failed";
        return std::nullopt;
      }
      request.headers = {
          {"X-BAPI-API-KEY", credentials.key},
          {"X-BAPI-TIMESTAMP", timestamp},
          {"X-BAPI-RECV-WINDOW", std::string{window}},
          {"X-BAPI-SIGN", signature},
      };
      return request;
    }
    case AuthKind::OkxHmac: {
      const auto timestamp = iso8601_milliseconds(now_ms);
      const auto signature = base64(hmac_bytes(
          credentials.secret,
          timestamp + "GET" + probe_case.path,
          "sha256"));
      if (timestamp.empty() || signature.empty() ||
          credentials.passphrase.empty()) {
        error = "okx_auth_fields_unavailable";
        return std::nullopt;
      }
      request.headers = {
          {"OK-ACCESS-KEY", credentials.key},
          {"OK-ACCESS-SIGN", signature},
          {"OK-ACCESS-TIMESTAMP", timestamp},
          {"OK-ACCESS-PASSPHRASE", credentials.passphrase},
      };
      return request;
    }
    case AuthKind::GateHmac: {
      const auto timestamp = std::to_string(now_ms / 1000);
      const auto body_hash = sha512_empty_hex();
      const auto sign_base =
          std::string{"GET\n"} + std::string{path_without_query(probe_case.path)} +
          "\n" + std::string{query_of(probe_case.path)} + "\n" +
          body_hash + "\n" + timestamp;
      const auto signature =
          hex(hmac_bytes(credentials.secret, sign_base, "sha512"));
      if (body_hash.empty() || signature.empty()) {
        error = "gate_hmac_failed";
        return std::nullopt;
      }
      request.headers = {
          {"KEY", credentials.key},
          {"Timestamp", timestamp},
          {"SIGN", signature},
      };
      return request;
    }
    case AuthKind::KucoinHmac: {
      const auto timestamp = std::to_string(now_ms);
      const auto signature = base64(hmac_bytes(
          credentials.secret,
          timestamp + "GET" + probe_case.path,
          "sha256"));
      const auto passphrase = base64(hmac_bytes(
          credentials.secret,
          credentials.passphrase,
          "sha256"));
      if (signature.empty() || passphrase.empty()) {
        error = "kucoin_auth_fields_unavailable";
        return std::nullopt;
      }
      request.headers = {
          {"KC-API-KEY", credentials.key},
          {"KC-API-SIGN", signature},
          {"KC-API-TIMESTAMP", timestamp},
          {"KC-API-PASSPHRASE", passphrase},
          {"KC-API-KEY-VERSION", "2"},
      };
      return request;
    }
    case AuthKind::BitgetHmac: {
      const auto timestamp = std::to_string(now_ms);
      const auto signature = base64(hmac_bytes(
          credentials.secret,
          timestamp + "GET" + probe_case.path,
          "sha256"));
      if (signature.empty() || credentials.passphrase.empty()) {
        error = "bitget_auth_fields_unavailable";
        return std::nullopt;
      }
      request.headers = {
          {"ACCESS-KEY", credentials.key},
          {"ACCESS-SIGN", signature},
          {"ACCESS-TIMESTAMP", timestamp},
          {"ACCESS-PASSPHRASE", credentials.passphrase},
      };
      return request;
    }
  }
  error = "unknown_auth_kind";
  return std::nullopt;
}

}  // namespace exchange_probe
