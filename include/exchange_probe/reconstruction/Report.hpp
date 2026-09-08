#pragma once

#include "exchange_probe/reconstruction/Analysis.hpp"

#include <filesystem>
#include <string>

namespace exchange_probe::reconstruction {

struct CaptureAnalysis final {
  CaptureHeader header{};
  CaptureFooter footer{};
  AnalysisCounters strict{};
  AnalysisCounters hybrid{};
  AnalysisCounters receiveCounterfactual{};
};

[[nodiscard]] bool analyzeCapture(const std::filesystem::path& capturePath,
                                  CaptureAnalysis& output,
                                  std::string& error) noexcept;
[[nodiscard]] bool writeMarkdownReport(
    const std::filesystem::path& path,
    const CaptureAnalysis& analysis,
    std::string& error) noexcept;

}  // namespace exchange_probe::reconstruction
