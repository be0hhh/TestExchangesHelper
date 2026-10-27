#include "exchange_probe/TbankCredential.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>
#include <utility>

namespace exchange_probe {
namespace {
constexpr std::size_t kMaxFileBytes = 1024U * 1024U;
constexpr std::size_t kMaxLineBytes = 8192U;

struct LineBuffer {
  std::array<char, kMaxLineBytes> data{};
  ~LineBuffer() { OPENSSL_cleanse(data.data(), data.size()); }
};

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
  return value;
}
}  // namespace

TbankCredential::~TbankCredential() { OPENSSL_cleanse(token_.data(), token_.size()); }
TbankCredential::TbankCredential(TbankCredential&& other) noexcept {
  *this = std::move(other);
}
TbankCredential& TbankCredential::operator=(TbankCredential&& other) noexcept {
  if (this != &other) {
    OPENSSL_cleanse(token_.data(), token_.size());
    std::copy_n(other.token_.data(), other.size_, token_.data());
    size_ = other.size_;
    OPENSSL_cleanse(other.token_.data(), other.token_.size());
    other.size_ = 0;
  }
  return *this;
}

std::optional<TbankCredential> load_tbank_credential(
    const std::filesystem::path& path, unsigned slot, std::string& error) {
  error.clear();
  if (slot == 0U || slot > 255U) {
    error = "credential_slot_invalid";
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "env_file_open_failed";
    return std::nullopt;
  }
  const std::string key = "TBANK_API_" + std::to_string(slot) + "_API";
  TbankCredential credential;
  LineBuffer line;
  std::size_t size = 0, total = 0;
  bool found = false;
  auto process = [&]() -> bool {
    auto text = trim({line.data.data(), size});
    if (text.empty() || text.front() == '#') return true;
    if (text.starts_with("export ")) text = trim(text.substr(7));
    const auto equal = text.find('=');
    const auto name = trim(text.substr(0, equal));
    if (name != key) return true;
    if (found || equal == std::string_view::npos) return false;
    found = true;
    auto value = trim(text.substr(equal + 1));
    if (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
      if (value.size() < 2 || value.back() != value.front()) return false;
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty() || value.size() > credential.token_.size()) return false;
    for (const char character : value) {
      const auto c = static_cast<unsigned char>(character);
      if (c < 33U || c > 126U) return false;
    }
    std::copy(value.begin(), value.end(), credential.token_.begin());
    credential.size_ = value.size();
    return true;
  };
  char ch;
  while (input.get(ch)) {
    if (++total > kMaxFileBytes) {
      error = "env_file_too_large";
      return std::nullopt;
    }
    if (ch == '\n') {
      if (!process()) { error = "credential_invalid_or_duplicate"; return std::nullopt; }
      OPENSSL_cleanse(line.data.data(), line.data.size());
      size = 0;
    } else if (size == line.data.size()) {
      error = "env_file_line_too_large";
      return std::nullopt;
    } else {
      line.data[size++] = ch;
    }
  }
  if (!input.eof()) { error = "env_file_read_failed"; return std::nullopt; }
  if (size && !process()) { error = "credential_invalid_or_duplicate"; return std::nullopt; }
  if (!found) { error = "credential_missing"; return std::nullopt; }
  return credential;
}
}  // namespace exchange_probe
