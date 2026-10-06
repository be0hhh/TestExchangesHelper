#pragma once

#include "exchange_probe/Credentials.hpp"
#include "exchange_probe/Model.hpp"

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace exchange_probe {

struct CliOptions;

// One frozen unsigned public GET. An I/O failure or peer close permanently
// invalidates this instance; callers must explicitly construct a new round.
class PublicHttpConnection {
 public:
  explicit PublicHttpConnection(const RestCase& probe_case);
  ~PublicHttpConnection();
  PublicHttpConnection(const PublicHttpConnection&) = delete;
  PublicHttpConnection& operator=(const PublicHttpConnection&) = delete;
  [[nodiscard]] HttpResult get(std::chrono::steady_clock::time_point deadline);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] int capture_paired_rest_research(
    const CliOptions& options,
    const ProductSpec& profile,
    const std::filesystem::path& directory,
    std::ostream& output,
    std::ostream& error_output);

[[nodiscard]] HttpResult execute_http(
    const RestCase& probe_case,
    std::chrono::steady_clock::time_point deadline,
    const std::optional<SignedRequest>& signed_request = std::nullopt,
    const std::optional<std::string>& pinned_ip = std::nullopt);

[[nodiscard]] WsResult observe_ws(
    const WsCase& probe_case,
    std::chrono::steady_clock::time_point deadline,
    const std::optional<std::string>& pinned_ip = std::nullopt);

}  // namespace exchange_probe
