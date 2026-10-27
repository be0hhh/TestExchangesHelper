#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace exchange_probe {

class TbankCredential {
 public:
  TbankCredential() = default;
  ~TbankCredential();
  TbankCredential(const TbankCredential&) = delete;
  TbankCredential& operator=(const TbankCredential&) = delete;
  TbankCredential(TbankCredential&& other) noexcept;
  TbankCredential& operator=(TbankCredential&& other) noexcept;

  [[nodiscard]] std::string_view token() const noexcept {
    return {token_.data(), size_};
  }

 private:
  friend std::optional<TbankCredential> load_tbank_credential(
      const std::filesystem::path&, unsigned, std::string&);
  std::array<char, 511> token_{};
  std::size_t size_{0};
};

[[nodiscard]] std::optional<TbankCredential> load_tbank_credential(
    const std::filesystem::path& path, unsigned slot, std::string& error);

}  // namespace exchange_probe
