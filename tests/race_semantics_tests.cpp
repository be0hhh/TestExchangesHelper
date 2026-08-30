#include "exchange_probe/race/book.hpp"
#include "exchange_probe/race/match.hpp"

#include <cassert>

int main() {
  using namespace exchange_probe::race;

  TradeIdentity one{.tradeId = 12u};
  TradeIdentity same{.tradeId = 12u};
  TradeIdentity aggregate{
      .aggregateId = 7u,
      .firstTradeId = 10u,
      .lastTradeId = 14u,
      .count = 5u,
  };
  assert(match_trade_identity(one, same) == TradeMatchKind::ExactSingle);
  assert(match_trade_identity(one, aggregate) ==
         TradeMatchKind::AggregateContainsSingle);
  assert(match_trade_identity(TradeIdentity{.tradeId = 99u}, aggregate) ==
         TradeMatchKind::None);
  const TradeIdentity uuidA{
      .nativeFirst = 0x1234u, .nativeSecond = 0x5678u,
      .tradeId = 99u, .nativeShape = 2u};
  const TradeIdentity uuidB{
      .nativeFirst = 0x1234u, .nativeSecond = 0x5678u,
      .tradeId = 100u, .nativeShape = 2u};
  assert(match_trade_identity(uuidA, uuidB) == TradeMatchKind::ExactSingle);

  const BboState bbo0{100, 10, 101, 12};
  const BboState bbo1{100, 9, 101, 12};
  assert(bbo_change_mask(bbo0, bbo0) == 0u);
  assert(bbo_change_mask(bbo0, bbo1) == kBidQuantityChanged);
  const BboTransition transition{bbo0, bbo1, 20u, kBidQuantityChanged};
  assert(same_bbo_transition(transition, transition));
  assert(!same_bbo_transition(
      transition, BboTransition{bbo1, bbo0, 21u, kBidQuantityChanged}));

  const BookLevel bids[]{{100, 10}, {99, 20}, {98, 30}, {97, 40}, {96, 50},
                         {95, 60}};
  const BookLevel asks[]{{101, 12}, {102, 22}, {103, 32}, {104, 42}, {105, 52},
                         {106, 62}};
  BoundedLocalBook book;
  assert(book.apply_snapshot(bids, 6u, asks, 6u, 100u));
  assert(book.validity() == BookValidity::Valid);
  assert(book.resync_generation() == 1u);
  assert(book.apply_snapshot(bids, 6u, asks, 6u, 101u));
  assert(book.resync_generation() == 1u);
  const auto initial5 = book.top_fingerprint(5u);
  const auto deep = book.apply_delta(
      BookSide::Bid, BookLevel{95, 61}, SequenceUpdate{102u, 102u, 101u, true});
  assert(deep.accepted);
  assert(!deep.bboChanged);
  assert(!deep.top5Changed);
  assert(deep.top50Changed);

  const auto top = book.apply_delta(
      BookSide::Bid, BookLevel{100, 9}, SequenceUpdate{103u, 103u, 102u, true});
  assert(top.accepted && top.bboChanged && top.top5Changed);
  assert(book.top_fingerprint(5u) != initial5);

  const BookMutation batch[]{{BookSide::Bid, {100, 8}},
                             {BookSide::Ask, {101, 10}}};
  const auto batched = book.apply_delta_batch(
      batch, 2u, SequenceUpdate{104u, 104u, 103u, true});
  assert(batched.accepted && batched.bboChanged);
  assert(book.last_sequence() == 104u);

  const auto gap = book.apply_delta(
      BookSide::Ask, BookLevel{101, 11}, SequenceUpdate{106u, 106u, 105u, true});
  assert(gap.gap);
  assert(book.validity() == BookValidity::InvalidGap);
  assert(book.bbo() == BboState{});
  assert(book.apply_snapshot(bids, 6u, asks, 6u, 200u));
  assert(book.resync_generation() == 2u);

  ExistingTradeBbo reconstructed;
  assert(reconstructed.configure(1));
  assert(reconstructed.seed(bbo0, 1'000u));
  BboState afterTrade{};
  assert(reconstructed.apply_trade(101, 5, 1u, 1'001u, afterTrade));
  assert(afterTrade.askPrice == 101);
  assert(afterTrade.askQuantity == 7);
  assert(!reconstructed.apply_trade(100, 1, 0u, 1'002u, afterTrade));
  assert(reconstructed.apply_trade(100, 1, 1u, 1'002u, afterTrade));
  assert(afterTrade.bidPrice == 99);
  assert(afterTrade.askPrice == 100);

  BoundedLocalBook invalidSnapshot;
  const BookLevel invalidBids[]{{100, 0}};
  assert(!invalidSnapshot.apply_snapshot(
      invalidBids, 1u, asks, 1u, 300u));
  assert(invalidSnapshot.validity() == BookValidity::InvalidGap);
}
