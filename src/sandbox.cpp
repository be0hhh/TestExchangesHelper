#include "exchange_probe/app.hpp"

#include <boost/json/parse.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace exchange_probe {
namespace {

[[nodiscard]] bool safe_token(std::string_view value, bool path) {
  if (value.empty() || value.find('\r') != std::string_view::npos ||
      value.find('\n') != std::string_view::npos) {
    return false;
  }
  if (path) {
    return value.front() == '/' && value.find("://") == std::string_view::npos;
  }
  return value.find('/') == std::string_view::npos &&
         value.find(':') == std::string_view::npos &&
         value.find('@') == std::string_view::npos;
}

[[nodiscard]] std::string text(
    const boost::json::object& object,
    std::string_view key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string()
             ? std::string{value->as_string()}
             : std::string{};
}

[[nodiscard]] std::vector<std::string> capabilities(
    const boost::json::object& object) {
  std::vector<std::string> result;
  const auto* values = object.if_contains("capabilities");
  if (values == nullptr || !values->is_array()) {
    return result;
  }
  for (const auto& value : values->as_array()) {
    if (value.is_string() && !value.as_string().empty()) {
      result.emplace_back(value.as_string());
    }
  }
  return result;
}

}  // namespace

std::optional<ProductSpec> load_sandbox_profile(
    const std::filesystem::path& path,
    std::string& error) {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    error = "sandbox_file_unreadable";
    return std::nullopt;
  }
  if (size > kMaxSandboxBytes) {
    error = "sandbox_file_capacity_exceeded";
    return std::nullopt;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "sandbox_file_open_failed";
    return std::nullopt;
  }
  const std::string content{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  boost::system::error_code parse_error;
  const auto root = boost::json::parse(content, parse_error);
  if (parse_error || !root.is_object()) {
    error = "sandbox_json_invalid";
    return std::nullopt;
  }

  const auto& object = root.as_object();
  ProductSpec product{
      .venue = text(object, "venue"),
      .product = text(object, "product"),
      .notes = {"Untrusted public-only sandbox profile."},
  };
  if (!safe_token(product.venue, false) ||
      !safe_token(product.product, false)) {
    error = "sandbox_venue_or_product_invalid";
    return std::nullopt;
  }
  if (object.contains("private_rest") || object.contains("credentials") ||
      object.contains("auth") || object.contains("headers")) {
    error = "sandbox_private_or_secret_fields_forbidden";
    return std::nullopt;
  }

  if (const auto* rest_cases = object.if_contains("public_rest");
      rest_cases != nullptr) {
    if (!rest_cases->is_array()) {
      error = "sandbox_public_rest_not_array";
      return std::nullopt;
    }
    for (const auto& value : rest_cases->as_array()) {
      if (!value.is_object()) {
        error = "sandbox_rest_case_not_object";
        return std::nullopt;
      }
      const auto& item = value.as_object();
      RestCase probe_case{
          .name = text(item, "name"),
          .host = text(item, "host"),
          .path = text(item, "path"),
          .capabilities = capabilities(item),
          .selection = Selection::DiagnosticVariant,
      };
      if (!safe_token(probe_case.name, false) ||
          !safe_token(probe_case.host, false) ||
          !safe_token(probe_case.path, true)) {
        error = "sandbox_rest_case_invalid";
        return std::nullopt;
      }
      product.public_rest.push_back(std::move(probe_case));
    }
  }

  if (const auto* ws_cases = object.if_contains("public_ws");
      ws_cases != nullptr) {
    if (!ws_cases->is_array()) {
      error = "sandbox_public_ws_not_array";
      return std::nullopt;
    }
    for (const auto& value : ws_cases->as_array()) {
      if (!value.is_object()) {
        error = "sandbox_ws_case_not_object";
        return std::nullopt;
      }
      const auto& item = value.as_object();
      WsCase probe_case{
          .name = text(item, "name"),
          .host = text(item, "host"),
          .path = text(item, "path"),
          .subscribe = text(item, "subscribe"),
          .capabilities = capabilities(item),
          .selection = Selection::DiagnosticVariant,
          .wire = Wire::Json,
          .data_kind = WsDataKind::AnyJson,
          .require_data = true,
          .data_implies_ack = true,
      };
      if (!safe_token(probe_case.name, false) ||
          !safe_token(probe_case.host, false) ||
          !safe_token(probe_case.path, true) ||
          probe_case.subscribe.size() > 64U * 1024U) {
        error = "sandbox_ws_case_invalid";
        return std::nullopt;
      }
      boost::system::error_code subscribe_error;
      const auto subscribe =
          boost::json::parse(probe_case.subscribe, subscribe_error);
      if (subscribe_error ||
          (!subscribe.is_object() && !subscribe.is_array())) {
        error = "sandbox_ws_subscribe_invalid";
        return std::nullopt;
      }
      product.public_ws.push_back(std::move(probe_case));
    }
  }
  if (product.public_rest.empty() && product.public_ws.empty()) {
    error = "sandbox_has_no_public_cases";
    return std::nullopt;
  }
  finalize_capabilities(product);
  return product;
}

}  // namespace exchange_probe
