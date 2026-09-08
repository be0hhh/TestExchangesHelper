#include "exchange_probe/race/Book.hpp"

#include <algorithm>

namespace exchange_probe::race {

void BoundedLocalBook::invalidate(BookValidity reason) noexcept {
  validity_ = reason;
}

void BoundedLocalBook::sort_side(
    std::array<BookLevel, kMaximumBookLevels>& levels,
    std::size_t count, BookSide side) noexcept {
  std::sort(
      levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(count),
      [side](const BookLevel& lhs, const BookLevel& rhs) {
        return side == BookSide::Bid ? lhs.price > rhs.price
                                     : lhs.price < rhs.price;
      });
}

bool BoundedLocalBook::apply_snapshot(
    const BookLevel* bids, std::size_t bidCount,
    const BookLevel* asks, std::size_t askCount,
    std::uint64_t sequence) noexcept {
  if (bids == nullptr || asks == nullptr || bidCount == 0u || askCount == 0u ||
      bidCount > kMaximumBookLevels || askCount > kMaximumBookLevels ||
      sequence == 0u) {
    validity_ = BookValidity::InvalidGap;
    return false;
  }
  const bool startsNewGeneration = validity_ != BookValidity::Valid;
  for (std::size_t i = 0u; i < bidCount; ++i) {
    if (bids[i].price <= 0 || bids[i].quantity <= 0) {
      validity_ = BookValidity::InvalidGap;
      return false;
    }
    bids_[i] = bids[i];
  }
  for (std::size_t i = 0u; i < askCount; ++i) {
    if (asks[i].price <= 0 || asks[i].quantity <= 0) {
      validity_ = BookValidity::InvalidGap;
      return false;
    }
    asks_[i] = asks[i];
  }
  bidCount_ = bidCount;
  askCount_ = askCount;
  sort_side(bids_, bidCount_, BookSide::Bid);
  sort_side(asks_, askCount_, BookSide::Ask);
  if (bids_[0].price >= asks_[0].price) {
    validity_ = BookValidity::InvalidGap;
    return false;
  }
  lastSequence_ = sequence;
  if (startsNewGeneration) ++resyncGeneration_;
  validity_ = BookValidity::Valid;
  return true;
}

bool BoundedLocalBook::sequence_continuous(
    const SequenceUpdate& update) const noexcept {
  if (update.last == 0u || update.last < update.first) return false;
  if (update.hasPrevious) return update.previous == lastSequence_;
  const std::uint64_t expected = lastSequence_ + 1u;
  return update.first <= expected && update.last >= expected;
}

bool BoundedLocalBook::update_side(
    std::array<BookLevel, kMaximumBookLevels>& levels,
    std::size_t& count, BookSide side, BookLevel level) noexcept {
  std::size_t found = count;
  for (std::size_t i = 0u; i < count; ++i) {
    if (levels[i].price == level.price) {
      found = i;
      break;
    }
  }
  if (level.quantity == 0) {
    if (found == count) return true;
    for (std::size_t i = found + 1u; i < count; ++i) {
      levels[i - 1u] = levels[i];
    }
    --count;
    levels[count] = BookLevel{};
    return true;
  }
  if (found != count) {
    levels[found] = level;
    return true;
  }
  if (count == kMaximumBookLevels) {
    const bool improvesTail = side == BookSide::Bid
                                  ? level.price > levels[count - 1u].price
                                  : level.price < levels[count - 1u].price;
    if (!improvesTail) return true;
    levels[count - 1u] = level;
    return true;
  }
  levels[count++] = level;
  return true;
}

BookApplyResult BoundedLocalBook::apply_delta(
    BookSide side, BookLevel level,
    const SequenceUpdate& sequence) noexcept {
  const BookMutation mutation{side, level};
  return apply_delta_batch(&mutation, 1u, sequence);
}

BookApplyResult BoundedLocalBook::apply_delta_batch(
    const BookMutation* mutations, std::size_t mutationCount,
    const SequenceUpdate& sequence) noexcept {
  BookApplyResult output{};
  output.before = bbo();
  if (validity_ != BookValidity::Valid || mutations == nullptr ||
      mutationCount == 0u || mutationCount > kMaximumBookLevels * 2u) {
    return output;
  }
  for (std::size_t index = 0u; index < mutationCount; ++index) {
    if (mutations[index].level.price <= 0 ||
        mutations[index].level.quantity < 0) {
      return output;
    }
  }
  if (!sequence_continuous(sequence)) {
    ++gaps_;
    validity_ = BookValidity::InvalidGap;
    output.gap = true;
    return output;
  }
  const auto before5 = top_fingerprint(5u);
  const auto before50 = top_fingerprint(50u);
  for (std::size_t index = 0u; index < mutationCount; ++index) {
    const auto side = mutations[index].side;
    auto& levels = side == BookSide::Bid ? bids_ : asks_;
    auto& count = side == BookSide::Bid ? bidCount_ : askCount_;
    if (!update_side(levels, count, side, mutations[index].level)) {
      validity_ = BookValidity::InvalidOverflow;
      return output;
    }
  }
  sort_side(bids_, bidCount_, BookSide::Bid);
  sort_side(asks_, askCount_, BookSide::Ask);
  if (bidCount_ == 0u || askCount_ == 0u ||
      bids_[0].price >= asks_[0].price) {
    validity_ = BookValidity::InvalidGap;
    return output;
  }
  lastSequence_ = sequence.last;
  output.after = bbo();
  output.top5Fingerprint = top_fingerprint(5u);
  output.top50Fingerprint = top_fingerprint(50u);
  output.bboChanged = !(output.before == output.after);
  output.top5Changed = before5 != output.top5Fingerprint;
  output.top50Changed = before50 != output.top50Fingerprint;
  output.accepted = true;
  return output;
}

BboState BoundedLocalBook::bbo() const noexcept {
  if (validity_ != BookValidity::Valid || bidCount_ == 0u || askCount_ == 0u) {
    return {};
  }
  return BboState{
      .bidPrice = bids_[0].price,
      .bidQuantity = bids_[0].quantity,
      .askPrice = asks_[0].price,
      .askQuantity = asks_[0].quantity,
  };
}

std::uint64_t BoundedLocalBook::top_fingerprint(
    std::size_t depth) const noexcept {
  if (validity_ != BookValidity::Valid || depth == 0u) return 0u;
  std::uint64_t output = fingerprint_seed();
  const auto bids = std::min(depth, bidCount_);
  const auto asks = std::min(depth, askCount_);
  output = fingerprint_value(output, static_cast<std::int64_t>(bids));
  for (std::size_t i = 0u; i < bids; ++i) {
    output = fingerprint_value(output, bids_[i].price);
    output = fingerprint_value(output, bids_[i].quantity);
  }
  output = fingerprint_value(output, static_cast<std::int64_t>(asks));
  for (std::size_t i = 0u; i < asks; ++i) {
    output = fingerprint_value(output, asks_[i].price);
    output = fingerprint_value(output, asks_[i].quantity);
  }
  return output;
}

bool ExistingTradeBbo::configure(std::int64_t tickSize) noexcept {
  Price value{};
  value.raw = tickSize;
  return cxet::composite::configureBboReconstruction(state_, value);
}

void ExistingTradeBbo::reset() noexcept {
  cxet::composite::resetBboReconstruction(state_);
}

bool ExistingTradeBbo::seed(
    const BboState& bbo, std::uint64_t parentRecvMonoNs) noexcept {
  cxet::composite::BookTickerRuntimeV1 input{};
  input.ts.raw = parentRecvMonoNs;
  input.bid.px.raw = bbo.bidPrice;
  input.bid.qty.raw = bbo.bidQuantity;
  input.ask.px.raw = bbo.askPrice;
  input.ask.qty.raw = bbo.askQuantity;
  return cxet::composite::applyBboReconstructionBookTicker(
      state_, input,
      cxet::composite::BboReconstructionTimestampOrigin::Exchange);
}

bool ExistingTradeBbo::apply_trade(
    std::int64_t price, std::int64_t quantity, std::uint8_t aggressorSide,
    std::uint64_t parentRecvMonoNs, BboState& output) noexcept {
  if (aggressorSide != 1u && aggressorSide != 2u) return false;
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = parentRecvMonoNs;
  trade.price.raw = price;
  trade.qty.raw = quantity;
  trade.side = aggressorSide == 1u ? Side::Buy() : Side::Sell();
  if (!cxet::composite::applyBboReconstructionKnownInitiatorTrade(
          state_, trade,
          cxet::composite::BboReconstructionTimestampOrigin::Exchange)) {
    return false;
  }
  const auto view = cxet::composite::bboReconstructionView(state_);
  if (!cxet::composite::validBboReconstruction(view)) return false;
  output = BboState{
      .bidPrice = view.bookTicker.bid.px.raw,
      .bidQuantity = view.bookTicker.bid.qty.raw,
      .askPrice = view.bookTicker.ask.px.raw,
      .askQuantity = view.bookTicker.ask.qty.raw,
  };
  return true;
}

}  // namespace exchange_probe::race
