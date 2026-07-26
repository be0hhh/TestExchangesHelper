#pragma once

#include "exchange_probe/model.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace exchange_probe {

struct WsValidation {
  bool matched{false};
  std::string error;
};

[[nodiscard]] ContractEvidence validate_rest_contract(
    RestContract contract,
    const boost::json::value& value,
    std::string_view expected_symbol);

[[nodiscard]] WsValidation validate_ws_ack(
    const WsCase& probe_case,
    const boost::json::value& value,
    bool json_present,
    bool binary);

[[nodiscard]] WsValidation validate_ws_data(
    const WsCase& probe_case,
    const boost::json::value& value,
    bool json_present,
    bool binary,
    std::string_view payload);

[[nodiscard]] bool decode_gzip_bounded(
    std::string_view input,
    std::size_t output_limit,
    std::string& output,
    std::string& error);

[[nodiscard]] boost::json::value shape_of(
    const boost::json::value& value,
    unsigned depth = 0);

[[nodiscard]] std::string bounded_public_frame(std::string_view payload);

}  // namespace exchange_probe
