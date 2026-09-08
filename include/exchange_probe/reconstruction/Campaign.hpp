#pragma once

#include "exchange_probe/reconstruction/Capture.hpp"
#include "exchange_probe/reconstruction/Catalog.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace exchange_probe::reconstruction {

struct CampaignOptions final {
  std::filesystem::path outputRoot{};
  std::size_t sessions{3u};
  std::chrono::seconds duration{600};
  bool includeDepth{true};
};

struct CampaignEntry final {
  ProductEligibility product{};
  Symbol symbol{};
  std::size_t session{0u};
  bool captured{false};
  std::filesystem::path capturePath{};
  std::filesystem::path reportPath{};
  std::string error{};
};

struct CampaignResult final {
  ProductCatalog catalog{};
  std::vector<CampaignEntry> entries{};
  std::vector<std::string> productFailures{};
};

[[nodiscard]] bool runCampaign(const CampaignOptions& options,
                               CampaignResult& result) noexcept;

}  // namespace exchange_probe::reconstruction
