#include "exchange_probe/reconstruction/analysis.hpp"

#include "primitives/composite/BookTickerSideTapeRuntimeV1.hpp"
#include "primitives/composite/TradeRuntimeV1.hpp"

#include <limits>

namespace exchange_probe::reconstruction {
namespace {

using CoreOrigin =
    cxet::composite::BboReconstructionTimestampOrigin;

[[nodiscard]] CoreOrigin coreOrigin(TimestampOrigin origin) noexcept {
  if (origin == TimestampOrigin::Exchange) return CoreOrigin::Exchange;
  if (origin == TimestampOrigin::Receive) return CoreOrigin::Receive;
  return CoreOrigin::Unknown;
}

[[nodiscard]] bool knownSide(std::uint8_t side) noexcept {
  return side == 1u || side == 2u;
}

[[nodiscard]] std::uint64_t nextLogical(std::uint64_t& value) noexcept {
  if (value == std::numeric_limits<std::uint64_t>::max()) return 0u;
  return ++value;
}

}  // namespace

bool Analyzer::configure(std::int64_t tickSizeRaw,
                         ReconstructionPolicy policy) noexcept {
  *this = Analyzer{};
  if (tickSizeRaw <= 0) return false;
  policy_ = policy;
  Price tick{};
  tick.raw = tickSizeRaw;
  return cxet::composite::configureBboReconstruction(state_, tick) &&
         cxet::composite::configureBboReconstruction(rawBbo_, tick);
}

bool Analyzer::apply(const Observation& observation) noexcept {
  ++counters_.observations;
  if (observation.kind == ObservationKind::BookTickerSide) {
    return applyBookTickerSide(observation);
  }
  if (observation.kind == ObservationKind::Trade) {
    return applyTrade(observation);
  }
  if (observation.kind == ObservationKind::DepthBbo) {
    if (observation.bidPriceRaw <= 0 || observation.askPriceRaw <= 0 ||
        observation.bidPriceRaw >= observation.askPriceRaw) {
      return false;
    }
    comparePending(observation, observation.bidPriceRaw,
                   observation.askPriceRaw, true);
    return true;
  }
  if (observation.kind == ObservationKind::Reset) {
    cxet::composite::resetBboReconstruction(state_);
    cxet::composite::resetBboReconstruction(rawBbo_);
    pending_ = {};
    pendingComparison_ = false;
    pendingComparisonUsesExchangeTime_ = false;
    pendingExchangeTimestamp_ = 0u;
    pendingReceiveTimestamp_ = 0u;
    logicalTimestamp_ = 0u;
    lastRawBookExchangeTimestamp_ = 0u;
    lastRawBookReceiveTimestamp_ = 0u;
    lastRawTradeExchangeTimestamp_ = 0u;
    lastRawTradeReceiveTimestamp_ = 0u;
    ++counters_.terminalResets;
    return true;
  }
  return false;
}

bool Analyzer::applyBookTickerSide(
    const Observation& observation) noexcept {
  if (!knownSide(observation.side) ||
      (observation.action != 1u && observation.action != 2u)) {
    return false;
  }

  cxet::composite::BookTickerSideUpdateRuntimeV1 rawUpdate{};
  rawUpdate.eventId.raw = observation.eventId;
  rawUpdate.ts.raw = observation.exchangeTimestampNs;
  rawUpdate.side = observation.side == 1u ? Side::Buy() : Side::Sell();
  rawUpdate.action = observation.action == 1u
      ? cxet::composite::BookTickerSideAction::Upsert
      : cxet::composite::BookTickerSideAction::Delete;
  rawUpdate.level.px.raw = observation.priceRaw;
  rawUpdate.level.qty.raw = observation.qtyRaw;
  if (!cxet::composite::applyBboReconstructionBookTickerSide(
          rawBbo_, rawUpdate, coreOrigin(observation.timestampOrigin))) {
    return false;
  }

  const auto rawView = cxet::composite::bboReconstructionView(rawBbo_);
  if (observation.coalesceNext == 0u &&
      cxet::composite::validBboReconstruction(rawView)) {
    comparePending(observation, rawView.bookTicker.bid.px.raw,
                   rawView.bookTicker.ask.px.raw, false);
  }
  ++counters_.rawBookTickerUpdates;
  lastRawBookReceiveTimestamp_ = observation.receiveMonotonicNs;
  if (observation.timestampOrigin == TimestampOrigin::Exchange &&
      observation.exchangeTimestampNs != 0u) {
    lastRawBookExchangeTimestamp_ = observation.exchangeTimestampNs;
  } else {
    lastRawBookExchangeTimestamp_ = 0u;
  }

  cxet::composite::BookTickerSideUpdateRuntimeV1 update = rawUpdate;
  CoreOrigin origin = coreOrigin(observation.timestampOrigin);
  if (policy_ == ReconstructionPolicy::ReceiveOrderCounterfactual) {
    update.ts.raw = nextLogical(logicalTimestamp_);
    origin = CoreOrigin::Exchange;
  } else if (policy_ == ReconstructionPolicy::HybridEligibility) {
    update.ts.raw = nextLogical(logicalTimestamp_);
    origin = CoreOrigin::Exchange;
  }
  if (update.ts.raw == 0u) return false;
  return cxet::composite::applyBboReconstructionBookTickerSide(
      state_, update, origin);
}

bool Analyzer::applyTrade(const Observation& observation) noexcept {
  ++counters_.trades;
  if (!knownSide(observation.side)) {
    ++counters_.unknownAggressor;
    ++counters_.rejectedTrades;
    return false;
  }

  const bool exactTimestamp =
      observation.timestampOrigin == TimestampOrigin::Exchange &&
      observation.exchangeTimestampNs != 0u;
  bool eligible = true;
  if (policy_ == ReconstructionPolicy::StrictExchange) {
    if (!exactTimestamp || lastRawBookExchangeTimestamp_ == 0u) {
      ++counters_.missingTimestamp;
      eligible = false;
    } else if (observation.exchangeTimestampNs ==
               lastRawBookExchangeTimestamp_) {
      ++counters_.equalBookTickerTimestamp;
      eligible = false;
    } else if (observation.exchangeTimestampNs <
                   lastRawBookExchangeTimestamp_ ||
               (lastRawTradeExchangeTimestamp_ != 0u &&
                observation.exchangeTimestampNs <
                    lastRawTradeExchangeTimestamp_)) {
      ++counters_.timestampRollback;
      eligible = false;
    }
  } else if (policy_ == ReconstructionPolicy::HybridEligibility &&
             exactTimestamp && lastRawBookExchangeTimestamp_ != 0u) {
    if (observation.exchangeTimestampNs ==
        lastRawBookExchangeTimestamp_) {
      ++counters_.equalBookTickerTimestamp;
      eligible = false;
    } else if (observation.exchangeTimestampNs <
                   lastRawBookExchangeTimestamp_ ||
               (lastRawTradeExchangeTimestamp_ != 0u &&
                observation.exchangeTimestampNs <
                    lastRawTradeExchangeTimestamp_)) {
      ++counters_.timestampRollback;
      eligible = false;
    }
  } else if (policy_ == ReconstructionPolicy::HybridEligibility) {
    if (observation.receiveMonotonicNs == 0u ||
        lastRawBookReceiveTimestamp_ == 0u) {
      ++counters_.missingTimestamp;
      eligible = false;
    } else if (observation.receiveMonotonicNs <
                   lastRawBookReceiveTimestamp_ ||
               (lastRawTradeReceiveTimestamp_ != 0u &&
                observation.receiveMonotonicNs <
                    lastRawTradeReceiveTimestamp_)) {
      ++counters_.timestampRollback;
      eligible = false;
    }
  }
  if (!eligible) {
    ++counters_.rejectedTrades;
    return false;
  }

  const auto before = cxet::composite::bboReconstructionView(state_);
  if (!cxet::composite::validBboReconstruction(before)) {
    ++counters_.rejectedTrades;
    return false;
  }
  const bool isBuy = observation.side == 1u;
  const auto touchPrice =
      isBuy ? before.bookTicker.ask.px.raw : before.bookTicker.bid.px.raw;
  const auto touchQty =
      isBuy ? before.bookTicker.ask.qty.raw : before.bookTicker.bid.qty.raw;
  const bool touchQtyKnown =
      (isBuy ? before.askQtyKnown.raw : before.bidQtyKnown.raw) ==
      cxet::composite::kBboReconstructionFlagTrue.raw;
  ReconstructionMode mode = ReconstructionMode::Reverse;
  if (observation.priceRaw == touchPrice) {
    mode = ReconstructionMode::Nibbling;
  } else if (isBuy ? observation.priceRaw > touchPrice
                   : observation.priceRaw < touchPrice) {
    mode = ReconstructionMode::Direct;
  }

  cxet::composite::TradeRuntimeV1 trade{};
  trade.eventId.raw = observation.eventId;
  trade.price.raw = observation.priceRaw;
  trade.qty.raw = observation.qtyRaw;
  trade.side = isBuy ? Side::Buy() : Side::Sell();
  CoreOrigin origin = CoreOrigin::Exchange;
  if (policy_ == ReconstructionPolicy::StrictExchange) {
    trade.ts.raw = observation.exchangeTimestampNs;
  } else {
    trade.ts.raw = nextLogical(logicalTimestamp_);
  }
  if (trade.ts.raw == 0u ||
      !cxet::composite::applyBboReconstructionKnownInitiatorTrade(
          state_, trade, origin)) {
    ++counters_.rejectedTrades;
    return false;
  }

  ++counters_.acceptedTrades;
  if (mode == ReconstructionMode::Direct) ++counters_.direct;
  if (mode == ReconstructionMode::Reverse) ++counters_.reverse;
  if (mode == ReconstructionMode::Nibbling) {
    ++counters_.nibbling;
    if (touchQtyKnown && touchQty > 0 && observation.qtyRaw >= touchQty) {
      ++counters_.fullNibbling;
    }
  }
  if (exactTimestamp) {
    lastRawTradeExchangeTimestamp_ = observation.exchangeTimestampNs;
  }
  lastRawTradeReceiveTimestamp_ = observation.receiveMonotonicNs;
  pending_ = cxet::composite::bboReconstructionView(state_);
  pendingComparison_ = true;
  pendingComparisonUsesExchangeTime_ =
      policy_ == ReconstructionPolicy::StrictExchange ||
      (policy_ == ReconstructionPolicy::HybridEligibility &&
       exactTimestamp && lastRawBookExchangeTimestamp_ != 0u);
  pendingExchangeTimestamp_ = observation.exchangeTimestampNs;
  pendingReceiveTimestamp_ = observation.receiveMonotonicNs;
  return true;
}

void Analyzer::comparePending(const Observation& observation,
                              std::int64_t bidPriceRaw,
                              std::int64_t askPriceRaw,
                              bool depth) noexcept {
  if (!pendingComparison_) return;
  if (pendingComparisonUsesExchangeTime_) {
    if (observation.timestampOrigin != TimestampOrigin::Exchange ||
        observation.exchangeTimestampNs == 0u ||
        observation.exchangeTimestampNs <= pendingExchangeTimestamp_) {
      return;
    }
  } else if (observation.receiveMonotonicNs == 0u ||
             observation.receiveMonotonicNs <= pendingReceiveTimestamp_) {
    return;
  }
  const bool confirms =
      bidPriceRaw <= pending_.bookTicker.bid.px.raw &&
      askPriceRaw >= pending_.bookTicker.ask.px.raw;
  if (depth) {
    if (confirms) ++counters_.depthConfirmations;
    else ++counters_.depthContradictions;
  } else {
    if (confirms) ++counters_.nextBboConfirmations;
    else ++counters_.nextBboContradictions;
  }
  pendingComparison_ = false;
  pendingComparisonUsesExchangeTime_ = false;
  pendingExchangeTimestamp_ = 0u;
  pendingReceiveTimestamp_ = 0u;
}

}  // namespace exchange_probe::reconstruction
