#pragma once

#include "exchange_probe/race/analysis.hpp"

#include <filesystem>
#include <string>

namespace exchange_probe::race {

[[nodiscard]] bool write_analysis_report(
    const AnalysisResult& analysis, const std::filesystem::path& directory,
    std::string& error);

}  // namespace exchange_probe::race
