#pragma once

#include "exchange_probe/Contracts.hpp"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

namespace exchange_probe::net_detail {

[[nodiscard]] inline std::string application_pong(
    std::string_view heartbeat,
    const boost::json::value& value,
    std::string& error) {
  if (heartbeat != "htx" || !value.is_object()) return {};
  const auto* ping = value.as_object().if_contains("ping");
  if (ping == nullptr) return {};
  if ((!ping->is_uint64() && !ping->is_int64()) ||
      (ping->is_int64() && ping->as_int64() < 0)) {
    error = "htx_application_ping_invalid";
    return {};
  }
  return boost::json::serialize(boost::json::object{{"pong", *ping}});
}

[[nodiscard]] inline std::string htx_application_pong(
    std::string_view payload, bool gzip, std::string& error) {
  if (payload.size() > kMaxWsMessageBytes) {
    error = "htx_application_payload_capacity_exceeded";
    return {};
  }
  std::string decoded;
  if (gzip) {
    if (!decode_gzip_bounded(payload, kMaxWsMessageBytes, decoded, error)) {
      return {};
    }
    payload = decoded;
  }
  boost::system::error_code parse_error;
  const auto value = boost::json::parse(payload, parse_error);
  if (parse_error) {
    error = "htx_application_json_invalid";
    return {};
  }
  return application_pong("htx", value, error);
}

}  // namespace exchange_probe::net_detail
