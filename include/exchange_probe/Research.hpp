#pragma once

#include "exchange_probe/App.hpp"
#include "exchange_probe/ResearchProfile.hpp"

#include <filesystem>
#include <iosfwd>
#include <string>

namespace exchange_probe {

inline constexpr std::string_view kResearchBundleSchema =
    "exchange.api_probe.bundle.v3";
inline constexpr std::string_view kResearchFrameSchema =
    "exchange.api_probe.frame_index.v1";
inline constexpr std::string_view kResearchEventSchema =
    "exchange.api_probe.event.v1";
inline constexpr std::string_view kResearchRelationSchema =
    "exchange.api_probe.relation.v1";
inline constexpr std::string_view kResearchFindingsSchema =
    "exchange.api_probe.findings.v1";

[[nodiscard]] int capture_research_bundle(
    const CliOptions& options,
    const ResearchProductProfile& profile,
    const std::filesystem::path& directory,
    std::ostream& output,
    std::ostream& error_output);

[[nodiscard]] int analyze_research_bundle(
    const std::filesystem::path& directory,
    std::ostream& output,
    std::ostream& error_output);

[[nodiscard]] int serve_research_viewer(
    const std::filesystem::path& root,
    bool open_browser,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace exchange_probe
