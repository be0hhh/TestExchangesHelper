#pragma once

#include "exchange_probe/credentials.hpp"
#include "exchange_probe/model.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace exchange_probe {

[[nodiscard]] ProxyConfig proxy_for_host(std::string_view host);

[[nodiscard]] HttpResult execute_http(
    const RestCase& probe_case,
    std::chrono::steady_clock::time_point deadline,
    const std::optional<SignedRequest>& signed_request = std::nullopt);

[[nodiscard]] WsResult observe_ws(
    const WsCase& probe_case,
    std::chrono::steady_clock::time_point deadline);

}  // namespace exchange_probe
