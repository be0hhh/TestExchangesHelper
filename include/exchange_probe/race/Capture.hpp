#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace exchange_probe::race {

struct PublicCaptureOptions {
  std::filesystem::path outputDirectory;
  std::vector<std::string> requestedBases;
  std::vector<std::string> venues;
  unsigned sessions{1u};
  std::chrono::seconds warmup{5};
  std::chrono::seconds measured{20};
  bool validation{false};
};

struct PublicCaptureResult {
  bool complete{false};
  bool degraded{false};
  std::string error;
  unsigned sessionsCompleted{0u};
  unsigned availableInstruments{0u};
  unsigned unavailableInstruments{0u};
};

[[nodiscard]] PublicCaptureResult run_public_capture(
    const PublicCaptureOptions& options);

}  // namespace exchange_probe::race
