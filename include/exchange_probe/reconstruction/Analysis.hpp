#pragma once

#include "exchange_probe/reconstruction/Types.hpp"

#include "trading_core/Algorithm/Market/BboReconstruction.hpp"

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
  [[nodiscard]] trading_core::BboReconstructionView view() const noexcept {
    return trading_core::bboReconstructionView(state_);
  }

 private:
  [[nodiscard]] bool applyBookTickerSide(
      const Observation& observation) noexcept;
  [[nodiscard]] bool applyTrade(const Observation& observation) noexcept;
  void comparePending(const Observation& observation,
                      std::int64_t bidPriceRaw,
                      std::int64_t askPriceRaw,
                      bool depth) noexcept;

  trading_core::BboReconstructionState state_{};
  trading_core::BboReconstructionState rawBbo_{};
  trading_core::BboReconstructionView pending_{};
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
