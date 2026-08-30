#include "exchange_probe/race/catalog.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace exchange_probe::race {
namespace {

void add(
    VenueFeedPlan& venue, std::string id, EventClass eventClass,
    Wire wire, unsigned depth = 0u, unsigned intervalMs = 0u,
    bool reconstruct = false, AnchorRole anchor = AnchorRole::None,
    bool experimental = false,
    TransportKind transport = TransportKind::RuntimeSubscribe,
    bool credentialsExpected = false) {
  venue.feeds.push_back(FeedPlan{
      .id = std::move(id),
      .eventClass = eventClass,
      .wire = wire,
      .transport = transport,
      .anchor = anchor,
      .depth = depth,
      .nominalIntervalMs = intervalMs,
      .reconstructBook = reconstruct,
      .experimental = experimental,
      .credentialsExpected = credentialsExpected,
  });
}

VenueFeedPlan bitget() {
  VenueFeedPlan out{
      .venue = Venue::Bitget,
      .name = "bitget",
      .instruments = {Venue::Bitget, "api.bitget.com",
                      "/api/v3/market/instruments?category=USDT-FUTURES"},
      .feeds = {},
  };
  add(out, "json_books", EventClass::Depth, Wire::Json, 0u, 50u, true,
      AnchorRole::FastDepth);
  add(out, "json_books1", EventClass::Bbo, Wire::Json, 1u, 1u, false,
      AnchorRole::DirectBbo);
  add(out, "json_books5", EventClass::Top5, Wire::Json, 5u, 10u, false);
  add(out, "json_books50", EventClass::Top50, Wire::Json, 50u, 20u, false);
  add(out, "json_public_trade", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::Trade);
  add(out, "sbe_books1", EventClass::Bbo, Wire::Sbe, 1u, 0u);
  add(out, "sbe_books50", EventClass::Top50, Wire::Sbe, 50u, 0u, true);
  add(out, "sbe_public_trade", EventClass::Trade, Wire::Sbe);
  return out;
}

VenueFeedPlan bybit() {
  VenueFeedPlan out{
      .venue = Venue::Bybit,
      .name = "bybit",
      .instruments = {Venue::Bybit, "api.bybit.com",
                      "/v5/market/instruments-info?category=linear&limit=1000"},
      .feeds = {},
  };
  add(out, "orderbook_1", EventClass::Bbo, Wire::Json, 1u, 10u, false,
      AnchorRole::DirectBbo);
  add(out, "orderbook_50", EventClass::Top50, Wire::Json, 50u, 20u, true,
      AnchorRole::FastDepth);
  add(out, "orderbook_200", EventClass::Depth, Wire::Json, 200u, 100u, true);
  add(out, "orderbook_1000", EventClass::Depth, Wire::Json, 1000u, 200u, true);
  add(out, "public_trade", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::Trade);
  return out;
}

VenueFeedPlan gate() {
  VenueFeedPlan out{
      .venue = Venue::Gate,
      .name = "gate",
      .instruments = {Venue::Gate, "api.gateio.ws",
                      "/api/v4/futures/usdt/contracts"},
      .feeds = {},
  };
  add(out, "json_trades", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::Trade);
  add(out, "sbe_trades", EventClass::Trade, Wire::Sbe);
  add(out, "json_book_ticker", EventClass::Bbo, Wire::Json, 1u, 0u, false,
      AnchorRole::DirectBbo);
  add(out, "sbe_book_ticker", EventClass::Bbo, Wire::Sbe, 1u);
  constexpr std::array<unsigned, 6u> legacy{1u, 5u, 10u, 20u, 50u, 100u};
  for (unsigned depth : legacy) {
    add(out, "json_order_book_l" + std::to_string(depth), EventClass::Depth,
        Wire::Json, depth, 0u, depth > 1u,
        depth == 20u ? AnchorRole::FastDepth : AnchorRole::None);
    add(out, "sbe_order_book_l" + std::to_string(depth), EventClass::Depth,
        Wire::Sbe, depth, 0u, depth > 1u);
  }
  struct UpdateVariant { unsigned interval; unsigned depth; };
  constexpr std::array<UpdateVariant, 4u> updates{{
      {20u, 20u}, {100u, 20u}, {100u, 50u}, {100u, 100u}}};
  for (const auto variant : updates) {
    const auto suffix = std::to_string(variant.interval) + "ms_l" +
                        std::to_string(variant.depth);
    add(out, "json_order_book_update_" + suffix, EventClass::Depth,
        Wire::Json, variant.depth, variant.interval, true);
    add(out, "sbe_order_book_update_" + suffix, EventClass::Depth,
        Wire::Sbe, variant.depth, variant.interval, true);
  }
  add(out, "json_obu_l50_20ms", EventClass::Top50, Wire::Json, 50u, 20u, true);
  add(out, "sbe_obu_l50_20ms", EventClass::Top50, Wire::Sbe, 50u, 20u, true);
  add(out, "json_obu_l400_100ms", EventClass::Depth, Wire::Json, 400u, 100u,
      true);
  add(out, "sbe_obu_l400_100ms", EventClass::Depth, Wire::Sbe, 400u, 100u,
      true);
  return out;
}

VenueFeedPlan okx() {
  VenueFeedPlan out{
      .venue = Venue::Okx,
      .name = "okx",
      .instruments = {Venue::Okx, "www.okx.com",
                      "/api/v5/public/instruments?instType=SWAP"},
      .feeds = {},
  };
  add(out, "bbo_tbt", EventClass::Bbo, Wire::Json, 1u, 10u, false,
      AnchorRole::DirectBbo);
  add(out, "books", EventClass::Depth, Wire::Json, 400u, 100u, true,
      AnchorRole::FastDepth);
  add(out, "books5", EventClass::Top5, Wire::Json, 5u, 100u);
  add(out, "books50_l2_tbt", EventClass::Top50, Wire::Json, 50u, 10u, true,
      AnchorRole::None, true, TransportKind::RuntimeSubscribe, true);
  add(out, "books_l2_tbt", EventClass::Depth, Wire::Json, 400u, 10u, true,
      AnchorRole::None, true, TransportKind::RuntimeSubscribe, true);
  add(out, "trades", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::Trade);
  add(out, "trades_all", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::None, true);
  return out;
}

VenueFeedPlan kucoin() {
  VenueFeedPlan out{
      .venue = Venue::Kucoin,
      .name = "kucoin",
      .instruments = {Venue::Kucoin, "api.kucoin.com",
                      "/api/ua/v1/market/instrument?tradeType=FUTURES"},
      .feeds = {},
  };
  add(out, "depth_1", EventClass::Bbo, Wire::Json, 1u, 10u, false,
      AnchorRole::DirectBbo);
  add(out, "depth_5", EventClass::Top5, Wire::Json, 5u, 100u);
  add(out, "depth_50", EventClass::Top50, Wire::Json, 50u, 100u);
  add(out, "depth_increment_deprecated", EventClass::Depth, Wire::Json, 0u,
      0u, true, AnchorRole::None, true);
  add(out, "depth_increment_10ms", EventClass::Depth, Wire::Json, 500u, 10u,
      true, AnchorRole::FastDepth);
  add(out, "public_trade", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::Trade);
  return out;
}

void add_binance_like_depths(VenueFeedPlan& out) {
  add(out, "book_ticker", EventClass::Bbo, Wire::Json, 1u, 0u, false,
      AnchorRole::DirectBbo);
  add(out, "diff_depth_default", EventClass::Depth, Wire::Json, 0u, 0u, true,
      AnchorRole::FastDepth);
  for (unsigned interval : {0u, 100u, 250u, 500u}) {
    add(out, "diff_depth_" + std::to_string(interval) + "ms",
        EventClass::Depth, Wire::Json, 0u, interval, true,
        AnchorRole::None, interval == 0u);
  }
  for (unsigned depth : {5u, 10u, 20u}) {
    add(out, "partial_depth_" + std::to_string(depth) + "_default",
        depth == 5u ? EventClass::Top5 : EventClass::Depth, Wire::Json,
        depth, 0u, false);
    for (unsigned interval : {0u, 100u, 250u, 500u}) {
      add(out, "partial_depth_" + std::to_string(depth) + "_" +
                   std::to_string(interval) + "ms",
          depth == 5u ? EventClass::Top5 : EventClass::Depth, Wire::Json,
          depth, interval, false, AnchorRole::None, interval == 0u);
    }
  }
  add(out, "agg_trade", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::Trade);
  add(out, "raw_trade", EventClass::Trade, Wire::Json, 0u, 0u, false,
      AnchorRole::None, true);
  for (const auto transport :
       {TransportKind::RawStream, TransportKind::RuntimeSubscribe,
        TransportKind::CombinedStream}) {
    const std::string suffix =
        transport == TransportKind::RawStream
            ? "raw"
            : transport == TransportKind::RuntimeSubscribe ? "subscribe"
                                                            : "combined";
    add(out, "transport_book_ticker_" + suffix, EventClass::Bbo, Wire::Json,
        1u, 0u, false, AnchorRole::None, false, transport);
    add(out, "transport_fast_depth_" + suffix, EventClass::Depth, Wire::Json,
        0u, 0u, true, AnchorRole::None, false, transport);
    add(out, "transport_agg_trade_" + suffix, EventClass::Trade, Wire::Json,
        0u, 0u, false, AnchorRole::None, false, transport);
  }
}

VenueFeedPlan binance() {
  VenueFeedPlan out{
      .venue = Venue::BinanceUsdM,
      .name = "binance-usdm",
      .instruments = {Venue::BinanceUsdM, "fapi.binance.com",
                      "/fapi/v1/exchangeInfo"},
      .feeds = {},
  };
  add_binance_like_depths(out);
  return out;
}

VenueFeedPlan aster() {
  VenueFeedPlan out{
      .venue = Venue::Aster,
      .name = "aster",
      .instruments = {Venue::Aster, "fapi.asterdex.com",
                      "/fapi/v3/exchangeInfo"},
      .feeds = {},
  };
  add_binance_like_depths(out);
  return out;
}

}  // namespace

std::vector<VenueFeedPlan> requested_venue_plans() {
  std::vector<VenueFeedPlan> result;
  result.reserve(7u);
  result.push_back(bitget());
  result.push_back(bybit());
  result.push_back(gate());
  result.push_back(okx());
  result.push_back(kucoin());
  result.push_back(binance());
  result.push_back(aster());
  return result;
}

std::vector<RaceGroup> build_race_groups(
    const std::vector<FeedPlan>& feeds, std::size_t receiverBudget) {
  std::vector<std::size_t> anchors;
  std::vector<std::size_t> candidates;
  for (std::size_t index = 0u; index < feeds.size(); ++index) {
    if (feeds[index].anchor == AnchorRole::None) {
      candidates.push_back(index);
    } else {
      anchors.push_back(index);
    }
  }
  if (feeds.empty()) return {};
  if (anchors.empty() || receiverBudget < anchors.size()) {
    throw std::invalid_argument{"receiver budget cannot hold race anchors"};
  }
  const std::size_t candidateCapacity = receiverBudget - anchors.size();
  if (!candidates.empty() && candidateCapacity == 0u) {
    throw std::invalid_argument{"receiver budget leaves no candidate slot"};
  }
  std::vector<RaceGroup> result;
  if (candidates.empty()) {
    result.push_back(RaceGroup{.id = 1u, .feedIndices = anchors});
    return result;
  }
  for (std::size_t begin = 0u; begin < candidates.size();
       begin += candidateCapacity) {
    RaceGroup group{};
    group.id = static_cast<std::uint16_t>(result.size() + 1u);
    group.feedIndices = anchors;
    const auto end = std::min(candidates.size(), begin + candidateCapacity);
    group.feedIndices.insert(
        group.feedIndices.end(), candidates.begin() +
            static_cast<std::ptrdiff_t>(begin),
        candidates.begin() + static_cast<std::ptrdiff_t>(end));
    result.push_back(std::move(group));
  }
  return result;
}

}  // namespace exchange_probe::race
