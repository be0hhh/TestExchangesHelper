#include "exchange_probe/Research.hpp"

#include "ViewerAssets.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <spawn.h>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

namespace exchange_probe {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

inline constexpr std::size_t kMaximumViewerBundles = 4096U;
inline constexpr std::size_t kMaximumViewerJsonBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t kMaximumPreviewLines = 20'000U;

struct BundleEntry {
  std::filesystem::path directory;
  boost::json::object manifest;
};

[[nodiscard]] std::optional<boost::json::object> read_object(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > kMaximumViewerJsonBytes) return std::nullopt;
  std::ifstream input{path, std::ios::binary};
  if (!input) return std::nullopt;
  const std::string content{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  boost::system::error_code parse_error;
  auto value = boost::json::parse(content, parse_error);
  if (parse_error || !value.is_object()) return std::nullopt;
  return value.as_object();
}

[[nodiscard]] std::vector<BundleEntry> scan_bundles(
    const std::filesystem::path& root) {
  std::vector<BundleEntry> result;
  std::error_code error;
  if (std::filesystem::is_regular_file(root / "manifest.json", error)) {
    if (auto manifest = read_object(root / "manifest.json");
        manifest.has_value() &&
        manifest->if_contains("schema") != nullptr &&
        manifest->at("schema").is_string() &&
        manifest->at("schema").as_string() == kResearchBundleSchema) {
      result.push_back({root, std::move(*manifest)});
    }
    return result;
  }
  for (std::filesystem::recursive_directory_iterator iterator{root, error};
       !error &&
       iterator != std::filesystem::recursive_directory_iterator{} &&
       result.size() < kMaximumViewerBundles;
       iterator.increment(error)) {
    if (!iterator->is_regular_file() ||
        iterator->path().filename() != "manifest.json") {
      continue;
    }
    auto manifest = read_object(iterator->path());
    if (manifest.has_value() &&
        manifest->if_contains("schema") != nullptr &&
        manifest->at("schema").is_string() &&
        manifest->at("schema").as_string() == kResearchBundleSchema) {
      result.push_back(
          {iterator->path().parent_path(), std::move(*manifest)});
    }
  }
  std::sort(
      result.begin(),
      result.end(),
      [](const BundleEntry& lhs, const BundleEntry& rhs) {
        return lhs.directory.string() > rhs.directory.string();
      });
  return result;
}

[[nodiscard]] std::map<std::string, std::string> query_parameters(
    std::string_view target) {
  std::map<std::string, std::string> result;
  const auto question = target.find('?');
  if (question == std::string_view::npos) return result;
  std::size_t begin = question + 1U;
  while (begin < target.size()) {
    const auto ampersand = target.find('&', begin);
    const auto part = target.substr(
        begin,
        ampersand == std::string_view::npos
            ? target.size() - begin
            : ampersand - begin);
    const auto equal = part.find('=');
    if (equal != std::string_view::npos) {
      result.emplace(
          std::string{part.substr(0U, equal)},
          std::string{part.substr(equal + 1U)});
    }
    if (ampersand == std::string_view::npos) break;
    begin = ampersand + 1U;
  }
  return result;
}

[[nodiscard]] std::optional<std::size_t> size_value(
    const std::map<std::string, std::string>& values,
    std::string_view key) {
  const auto iterator = values.find(std::string{key});
  if (iterator == values.end()) return std::nullopt;
  std::size_t result = 0U;
  const auto [pointer, error] = std::from_chars(
      iterator->second.data(),
      iterator->second.data() + iterator->second.size(),
      result);
  if (error != std::errc{} ||
      pointer != iterator->second.data() + iterator->second.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] bool allowed_file(std::string_view name) noexcept {
  static constexpr std::string_view allowed[]{
      "manifest.json",
      "findings.json",
      "REPORT.md",
      "events.jsonl",
      "relations.jsonl",
      "state_transitions.jsonl",
      "sessions.jsonl",
      "controls.jsonl",
      "frames.jsonl",
  };
  return std::find(std::begin(allowed), std::end(allowed), name) !=
         std::end(allowed);
}

[[nodiscard]] std::string read_bounded_file(
    const std::filesystem::path& path,
    std::size_t maximum_bytes = kMaximumViewerJsonBytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum_bytes) return {};
  std::ifstream input{path, std::ios::binary};
  if (!input) return {};
  return {
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string read_preview(
    const std::filesystem::path& path,
    std::size_t line_limit) {
  std::ifstream input{path, std::ios::binary};
  if (!input) return {};
  std::string result;
  std::string line;
  for (std::size_t count = 0U;
       count < std::min(line_limit, kMaximumPreviewLines) &&
       std::getline(input, line);
       ++count) {
    if (result.size() + line.size() + 1U > kMaximumViewerJsonBytes) break;
    result += line;
    result.push_back('\n');
  }
  return result;
}

template <typename Body>
[[nodiscard]] http::response<http::string_body> response(
    http::status status,
    std::string_view content_type,
    Body&& body) {
  http::response<http::string_body> result{status, 11};
  result.set(http::field::server, "exchange-api-probe-viewer");
  result.set(http::field::content_type, content_type);
  result.set(http::field::cache_control, "no-store");
  result.body() = std::forward<Body>(body);
  result.prepare_payload();
  return result;
}

[[nodiscard]] http::response<http::string_body> route_request(
    const http::request<http::string_body>& request,
    const std::filesystem::path& root,
    const std::vector<BundleEntry>& bundles) {
  if (request.method() != http::verb::get) {
    return response(http::status::method_not_allowed, "text/plain", "GET only\n");
  }
  const std::string target{request.target()};
  if (target == "/" || target == "/index.html") {
    return response(http::status::ok, "text/html; charset=utf-8",
                    std::string{viewer_assets::kHtml});
  }
  if (target == "/app.css") {
    return response(http::status::ok, "text/css; charset=utf-8",
                    std::string{viewer_assets::kCss});
  }
  if (target == "/app.js") {
    return response(http::status::ok, "text/javascript; charset=utf-8",
                    std::string{viewer_assets::kJs});
  }
  if (target == "/api/catalog") {
    boost::json::array rows;
    for (std::size_t index = 0U; index < bundles.size(); ++index) {
      const auto& bundle = bundles[index];
      const auto string_member = [&](std::string_view key) {
        const auto* value = bundle.manifest.if_contains(key);
        return value != nullptr && value->is_string()
                   ? std::string{value->as_string()}
                   : std::string{};
      };
      rows.emplace_back(boost::json::object{
          {"index", index},
          {"venue", string_member("venue")},
          {"product", string_member("product")},
          {"symbol", string_member("symbol")},
          {"status", string_member("status")},
          {"path", bundle.directory.lexically_relative(root).string()},
      });
    }
    return response(
        http::status::ok,
        "application/json",
        boost::json::serialize(rows));
  }
  if (target.starts_with("/api/file?") ||
      target.starts_with("/api/preview?")) {
    const auto parameters = query_parameters(target);
    const auto bundle_index = size_value(parameters, "bundle");
    const auto name = parameters.find("name");
    if (!bundle_index.has_value() || *bundle_index >= bundles.size() ||
        name == parameters.end() || !allowed_file(name->second)) {
      return response(http::status::bad_request, "text/plain",
                      "invalid bundle or file\n");
    }
    const auto path = bundles[*bundle_index].directory / name->second;
    std::string body;
    if (target.starts_with("/api/preview?")) {
      body = read_preview(path, size_value(parameters, "limit").value_or(5000U));
    } else {
      body = read_bounded_file(path);
    }
    if (body.empty() && !std::filesystem::exists(path)) {
      return response(http::status::not_found, "text/plain", "not found\n");
    }
    return response(
        http::status::ok,
        name->second.ends_with(".md") ? "text/markdown; charset=utf-8"
                                      : "application/json",
        std::move(body));
  }
  return response(http::status::not_found, "text/plain", "not found\n");
}

void open_browser(std::string url, std::ostream& error_output) {
  pid_t process = 0;
  std::vector<char> url_buffer(url.begin(), url.end());
  url_buffer.push_back('\0');
  char command[] = "xdg-open";
  char* arguments[]{command, url_buffer.data(), nullptr};
  const int status =
      posix_spawnp(&process, command, nullptr, nullptr, arguments, environ);
  if (status != 0) {
    error_output << "viewer_warning: xdg-open failed: " << status << '\n';
  }
}

}  // namespace

int serve_research_viewer(
    const std::filesystem::path& root,
    bool open_browser_flag,
    std::ostream& output,
    std::ostream& error_output) {
  const auto bundles = scan_bundles(root);
  if (bundles.empty()) {
    error_output << "viewer_error: no neutral research bundles found\n";
    return 2;
  }
  try {
    asio::io_context context;
    tcp::acceptor acceptor{
        context, tcp::endpoint{asio::ip::make_address("127.0.0.1"), 0U}};
    const auto port = acceptor.local_endpoint().port();
    const auto url = "http://127.0.0.1:" + std::to_string(port) + "/";
    output << "viewer=" << url << " bundles=" << bundles.size() << '\n';
    output.flush();
    if (open_browser_flag) open_browser(url, error_output);
    for (;;) {
      tcp::socket socket{context};
      acceptor.accept(socket);
      beast::flat_buffer buffer;
      http::request<http::string_body> request;
      boost::system::error_code read_error;
      http::read(socket, buffer, request, read_error);
      if (read_error) continue;
      auto result = route_request(request, root, bundles);
      boost::system::error_code write_error;
      http::write(socket, result, write_error);
      boost::system::error_code shutdown_error;
      socket.shutdown(tcp::socket::shutdown_send, shutdown_error);
    }
  } catch (const std::exception& exception) {
    error_output << "viewer_error: " << exception.what() << '\n';
    return 2;
  }
}

}  // namespace exchange_probe
