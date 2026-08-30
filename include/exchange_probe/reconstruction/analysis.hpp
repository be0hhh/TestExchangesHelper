#pragma once

#include "exchange_probe/reconstruction/types.hpp"

#include "primitives/composite/BboReconstruction.hpp"

namespace exchange_probe::reconstruction {

class Analyzer final {
 public:
  [[nodiscard]] bool configure(
      std::int64_t tickSizeRaw,
      ReconstructionPolicy policy) noexcept;
  [[nodiscard]] bool apply(const Observation& observation) noexcept;

  [[nodiscard]] const AnalysisCounters& counters() const noexcept {
    return counters_;
  }
  [[nodiscard]] cxet::composite::BboReconstructionView view() const noexcept {
    return cxet::composite::bboReconstructionView(state_);
  }

 private:
  [[nodiscard]] bool applyBookTickerSide(
      const Observation& observation) noexcept;
  [[nodiscard]] bool applyTrade(const Observation& observation) noexcept;
  void comparePending(const Observation& observation,
                      std::int64_t bidPriceRaw,
                      std::int64_t askPriceRaw,
                      bool depth) noexcept;

  cxet::composite::BboReconstructionState state_{};
  cxet::composite::BboReconstructionState rawBbo_{};
  cxet::composite::BboReconstructionView pending_{};
  AnalysisCounters counters_{};
  ReconstructionPolicy policy_{ReconstructionPolicy::StrictExchange};
  std::uint64_t logicalTimestamp_{0u};
  std::uint64_t lastRawBookExchangeTimestamp_{0u};
  std::uint64_t lastRawBookReceiveTimestamp_{0u};
  std::uint64_t lastRawTradeExchangeTimestamp_{0u};
  std::uint64_t lastRawTradeReceiveTimestamp_{0u};
  std::uint64_t pendingExchangeTimestamp_{0u};
  std::uint64_t pendingReceiveTimestamp_{0u};
  bool pendingComparison_{false};
  bool pendingComparisonUsesExchangeTime_{false};
};

}  // namespace exchange_probe::reconstruction
