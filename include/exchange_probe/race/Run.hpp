#pragma once

#include "exchange_probe/race/Transport.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace exchange_probe::race {

struct GroupRunOptions {
  std::chrono::seconds warmup{20};
  std::chrono::seconds measured{600};
  std::chrono::seconds readyTimeout{60};
  std::chrono::seconds statusInterval{2};
  std::filesystem::path outputDirectory;
  bool printStatus{true};
};

struct GroupConnection {
  ConnectionSpec spec;
  NormalizerFn normalizer{nullptr};
  NormalizerResetFn resetNormalizer{nullptr};
  void* normalizerState{nullptr};
};

struct GroupRunResult {
  bool readyReached{false};
  bool complete{false};
  bool degraded{false};
  std::string error;
  std::vector<ConnectionObservation> observations;
};

[[nodiscard]] GroupRunResult run_group_session(
    const GroupRunOptions& options,
    const std::vector<GroupConnection>& connections);

}  // namespace exchange_probe::race
