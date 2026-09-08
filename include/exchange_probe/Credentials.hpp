#pragma once

#include "exchange_probe/Model.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace exchange_probe {

class Credentials {
 public:
  Credentials() = default;
  ~Credentials();

  Credentials(const Credentials&) = delete;
  Credentials& operator=(const Credentials&) = delete;
  Credentials(Credentials&& other) noexcept;
  Credentials& operator=(Credentials&& other) noexcept;

  std::string key;
  std::string secret;
  std::string passphrase;
  std::string account_id;
};

struct SignedRequest {
  std::string path;
  std::map<std::string, std::string> headers;
};

class Environment final : public std::map<std::string, std::string> {
 public:
  Environment() = default;
  ~Environment();

  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;
  Environment(Environment&& other) noexcept;
  Environment& operator=(Environment&& other) noexcept;
};

[[nodiscard]] Environment load_environment(
    const std::optional<std::filesystem::path>& env_file,
    std::string& error);

[[nodiscard]] std::vector<unsigned> configured_slots(
    const Environment& environment,
    std::string_view prefix);

[[nodiscard]] std::optional<Credentials> resolve_credentials(
    const Environment& environment,
    std::string_view prefix,
    unsigned slot);

[[nodiscard]] std::optional<SignedRequest> sign_request(
    const RestCase& probe_case,
    const Credentials& credentials,
    std::int64_t now_ms,
    std::string& error);

}  // namespace exchange_probe
