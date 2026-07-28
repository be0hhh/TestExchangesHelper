#include "exchange_probe/app.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

inline constexpr std::size_t kMaxAnchorClosureFiles = 128U;
inline constexpr std::size_t kMaxAnchorClosureBytes = 8U * 1024U * 1024U;

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

[[nodiscard]] std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return std::string{value.substr(first, last - first + 1U)};
}

[[nodiscard]] std::optional<std::string> quoted_include(
    std::string_view line) {
  const auto normalized = trim(line);
  constexpr std::string_view marker = "#include";
  if (!std::string_view{normalized}.starts_with(marker)) {
    return std::nullopt;
  }
  const auto quote = normalized.find('"', marker.size());
  if (quote == std::string::npos) {
    return std::nullopt;
  }
  const auto end = normalized.find('"', quote + 1U);
  if (end == std::string::npos || end == quote + 1U) {
    return std::nullopt;
  }
  return normalized.substr(quote + 1U, end - quote - 1U);
}

[[nodiscard]] bool safe_exchange_include(std::string_view path) noexcept {
  if (!path.starts_with("src/src/exchanges/") &&
      !path.starts_with("exchanges/")) {
    return false;
  }
  for (const auto& component : std::filesystem::path{path}) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::filesystem::path resolve_include(
    const std::filesystem::path& root,
    const std::filesystem::path& owner,
    std::string_view include) {
  if (include.starts_with("src/src/")) {
    return root / include;
  }
  if (include.starts_with("exchanges/")) {
    return root / "src/src" / include;
  }
  return owner.parent_path() / include;
}

[[nodiscard]] std::string read_anchor_closure(
    const std::filesystem::path& root,
    const std::filesystem::path& initial,
    std::string& error) {
  std::vector<std::filesystem::path> pending{initial};
  std::set<std::filesystem::path> visited;
  std::string combined;
  while (!pending.empty()) {
    const auto path = pending.back();
    pending.pop_back();
    std::error_code canonical_error;
    const auto normalized = std::filesystem::weakly_canonical(
        path, canonical_error);
    if (canonical_error || !visited.emplace(normalized).second) {
      continue;
    }
    if (visited.size() > kMaxAnchorClosureFiles) {
      error = "anchor_closure_file_capacity_exceeded";
      return {};
    }
    std::string read_error;
    const auto text = read_text(path, read_error);
    if (!read_error.empty()) {
      error = read_error;
      return {};
    }
    if (combined.size() + text.size() > kMaxAnchorClosureBytes) {
      error = "anchor_closure_byte_capacity_exceeded";
      return {};
    }
    combined.append(text);
    combined.push_back('\n');

    std::size_t cursor = 0;
    while (cursor < text.size()) {
      const auto end = text.find('\n', cursor);
      const auto line = text.substr(
          cursor,
          end == std::string::npos ? text.size() - cursor : end - cursor);
      if (const auto include = quoted_include(line);
          include.has_value() && safe_exchange_include(*include)) {
        const auto included = resolve_include(root, path, *include);
        std::error_code file_error;
        if (std::filesystem::is_regular_file(included, file_error)) {
          pending.push_back(included);
        }
      }
      if (end == std::string::npos) {
        break;
      }
      cursor = end + 1U;
    }
  }
  return combined;
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
  const auto text = read_anchor_closure(root, path, read_error);
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
  std::size_t cursor = 0;
  while (cursor < text.size()) {
    const auto end = text.find('\n', cursor);
    const auto line = text.substr(
        cursor,
        end == std::string::npos ? text.size() - cursor : end - cursor);
    const auto include = quoted_include(line);
    constexpr std::string_view prefix = "exchanges/";
    if (include.has_value() && include->starts_with(prefix)) {
      const auto family_begin = prefix.size();
      const auto family_end = include->find('/', family_begin);
      if (family_end != std::string::npos) {
        const auto family =
            include->substr(family_begin, family_end - family_begin);
        const auto filename = include->substr(family_end + 1U);
        if (!family.empty() &&
            filename.starts_with("register_") &&
            filename.ends_with(".hpp")) {
          families.insert(family);
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    cursor = end + 1U;
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
      {"schema_version", 3},
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
