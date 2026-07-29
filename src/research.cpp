#include "exchange_probe/research.hpp"

#include <chrono>
#include <filesystem>
#include <ostream>
#include <string>

namespace exchange_probe {
namespace {

[[nodiscard]] std::filesystem::path default_research_output(
    const ResearchProductProfile& profile) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::path{"results"} /
         ("research-" + profile.venue + "-" + profile.product + "-" +
          std::to_string(now));
}

}  // namespace

int run_research(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  if (options.action == "analyze") {
    if (options.inputs.size() != 1U) {
      error_output
          << "research_error: analyze requires exactly one --input bundle\n";
      return 2;
    }
    return analyze_research_bundle(options.inputs.front(), output, error_output);
  }
  auto loaded = load_research_catalog(options.profile_root);
  if (!loaded.ok) {
    error_output << "research_error: " << loaded.error << '\n';
    return 2;
  }
  const auto* profile = find_research_profile(
      loaded.catalog, options.venues.front(), options.products.front());
  if (profile == nullptr) {
    error_output << "research_error: profile_not_found\n";
    return 2;
  }
  const auto directory =
      options.output_dir.value_or(default_research_output(*profile));
  const int capture_status = capture_research_bundle(
      options, *profile, directory, output, error_output);
  if (capture_status != 0) {
    error_output
        << "research_error: capture incomplete; run research analyze "
           "explicitly only after reviewing the bundle status\n";
    return capture_status;
  }
  const int analysis_status =
      analyze_research_bundle(directory, output, error_output);
  if (analysis_status != 0) return analysis_status;
  if (options.open_viewer) {
    const int viewer_status =
        serve_research_viewer(directory, true, output, error_output);
    if (viewer_status != 0) {
      error_output
          << "viewer_warning: bundle remains available at "
          << directory.string() << '\n';
    }
  }
  return 0;
}

int run_viewer(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  const auto root =
      options.inputs.empty() ? std::filesystem::path{"results"}
                             : options.inputs.front();
  return serve_research_viewer(
      root, options.open_viewer, output, error_output);
}

}  // namespace exchange_probe
