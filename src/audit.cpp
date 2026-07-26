#include "exchange_probe/app.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {
namespace {

struct AnchorResult {
  bool ok{false};
  std::string reason;
};

[[nodiscard]] std::string read_text(
    const std::filesystem::path& path,
    std::string& error) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "open_failed";
    return {};
  }
  std::string text{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  if (!input.good() && !input.eof()) {
    error = "read_failed";
    return {};
  }
  return text;
}

[[nodiscard]] AnchorResult check_anchor(
    const std::filesystem::path& root,
    const SourceAnchor& anchor) {
  if (anchor.path.empty()) {
    return {.reason = "anchor_not_declared"};
  }
  const auto path = root / anchor.path;
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(path, filesystem_error)) {
    return {.reason = "file_missing"};
  }
  if (anchor.symbol.empty() && anchor.literals.empty()) {
    return {.ok = true, .reason = {}};
  }
  std::string read_error;
  const auto text = read_text(path, read_error);
  if (!read_error.empty()) {
    return {.reason = read_error};
  }
  if (!anchor.symbol.empty() &&
      text.find(anchor.symbol) == std::string::npos) {
    return {.reason = "symbol_missing"};
  }
  for (const auto& literal : anchor.literals) {
    if (text.find(literal) == std::string::npos) {
      return {.reason = "literal_missing:" + literal};
    }
  }
  return {.ok = true, .reason = {}};
}

void append_anchor_issue(
    boost::json::array& issues,
    std::string_view venue,
    std::string_view product,
    std::string_view probe_case,
    std::string_view anchor_kind,
    const SourceAnchor& anchor,
    const AnchorResult& result) {
  if (result.ok) {
    return;
  }
  issues.emplace_back(boost::json::object{
      {"venue", venue},
      {"product", product},
      {"case", probe_case},
      {"anchor_kind", anchor_kind},
      {"path", anchor.path},
      {"symbol", anchor.symbol},
      {"reason", result.reason},
  });
}

[[nodiscard]] std::set<std::string> registered_families(
    const std::filesystem::path& root,
    boost::json::array& issues) {
  const auto path = root / "src/src/exchanges/register_all.cpp";
  std::string error;
  const auto text = read_text(path, error);
  if (!error.empty()) {
    issues.emplace_back(boost::json::object{
        {"anchor_kind", "registry"},
        {"path", "src/src/exchanges/register_all.cpp"},
        {"reason", error},
    });
    return {};
  }
  std::set<std::string> families;
  const std::string marker = "exchanges/";
  std::size_t cursor = 0;
  while ((cursor = text.find(marker, cursor)) != std::string::npos) {
    cursor += marker.size();
    const auto end = text.find('/', cursor);
    if (end == std::string::npos) {
      break;
    }
    const auto family = text.substr(cursor, end - cursor);
    if (!family.empty()) {
      families.insert(family);
    }
    cursor = end + 1;
  }
  return families;
}

}  // namespace

boost::json::object audit_profiles(
    const std::vector<ProductSpec>& products,
    const std::filesystem::path& source_root) {
  boost::json::array issues;
  std::size_t anchors_checked = 0;
  std::size_t anchors_confirmed = 0;
  const auto registered = registered_families(source_root, issues);

  std::set<std::string> profiled_families;
  for (const auto& product : products) {
    profiled_families.insert(product.venue);
    const auto audit_anchor = [&](std::string_view case_name,
                                  std::string_view kind,
                                  const SourceAnchor& anchor) {
      ++anchors_checked;
      const auto result = check_anchor(source_root, anchor);
      anchors_confirmed += result.ok ? 1U : 0U;
      append_anchor_issue(
          issues,
          product.venue,
          product.product,
          case_name,
          kind,
          anchor,
          result);
    };
    if (!product.credential_anchor.path.empty()) {
      audit_anchor("credentials", "credential", product.credential_anchor);
    }
    for (const auto& probe_case : product.public_rest) {
      if (!probe_case.core_anchor.path.empty()) {
        audit_anchor(probe_case.name, "rest_core", probe_case.core_anchor);
      } else if (probe_case.selection == Selection::CoreSelected) {
        append_anchor_issue(
            issues,
            product.venue,
            product.product,
            probe_case.name,
            "rest_core",
            probe_case.core_anchor,
            {.reason = "core_selected_anchor_not_declared"});
      }
      if (!probe_case.parser_anchor.path.empty()) {
        audit_anchor(
            probe_case.name, "rest_parser", probe_case.parser_anchor);
      }
    }
    for (const auto& probe_case : product.private_rest) {
      if (!probe_case.core_anchor.path.empty()) {
        audit_anchor(
            probe_case.name, "private_rest", probe_case.core_anchor);
      }
    }
    for (const auto& probe_case : product.public_ws) {
      if (!probe_case.core_anchor.path.empty()) {
        audit_anchor(probe_case.name, "ws_core", probe_case.core_anchor);
      } else if (probe_case.selection == Selection::CoreSelected) {
        append_anchor_issue(
            issues,
            product.venue,
            product.product,
            probe_case.name,
            "ws_core",
            probe_case.core_anchor,
            {.reason = "core_selected_anchor_not_declared"});
      }
      if (!probe_case.parser_anchor.path.empty()) {
        audit_anchor(probe_case.name, "ws_parser", probe_case.parser_anchor);
      }
    }
    for (const auto& probe_case : product.fix_sessions) {
      audit_anchor(probe_case.name, "fix_session", probe_case.core_anchor);
      audit_anchor(probe_case.name, "fix_payload", probe_case.payload_anchor);
    }
  }

  for (const auto& family : registered) {
    if (family != "finam" && !profiled_families.contains(family)) {
      issues.emplace_back(boost::json::object{
          {"anchor_kind", "registry"},
          {"venue", family},
          {"reason", "registered_family_not_profiled"},
      });
    }
  }
  for (const auto& family : profiled_families) {
    if (!registered.empty() && !registered.contains(family)) {
      issues.emplace_back(boost::json::object{
          {"anchor_kind", "registry"},
          {"venue", family},
          {"reason", "profiled_family_not_registered"},
      });
    }
  }

  return {
      {"schema_version", 2},
      {"kind", "source_audit"},
      {"source_root", source_root.string()},
      {"ok", issues.empty()},
      {"summary",
       boost::json::object{
           {"products", products.size()},
           {"registered_families", registered.size()},
           {"profiled_families", profiled_families.size()},
           {"anchors_checked", anchors_checked},
           {"anchors_confirmed", anchors_confirmed},
           {"issues", issues.size()},
       }},
      {"issues", std::move(issues)},
  };
}

}  // namespace exchange_probe
