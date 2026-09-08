#include "exchange_probe/race/Catalog.hpp"
#include "exchange_probe/race/Instruments.hpp"

#include <algorithm>
#include <cassert>
#include <set>
#include <string>

int main() {
  using namespace exchange_probe::race;
  const auto venues = requested_venue_plans();
  assert(venues.size() == 7u);
  std::set<Venue> venueIds;
  for (const auto& venue : venues) {
    assert(!venue.name.empty());
    assert(!venue.instruments.host.empty());
    assert(!venue.feeds.empty());
    assert(venueIds.insert(venue.venue).second);
    std::set<std::string> feedIds;
    for (const auto& feed : venue.feeds) {
      assert(feedIds.insert(feed.id).second);
    }
    const auto groups = build_race_groups(venue.feeds, 5u);
    assert(!groups.empty());
    for (const auto& group : groups) {
      assert(group.feedIndices.size() <= 5u);
      unsigned bboAnchors = 0u;
      unsigned tradeAnchors = 0u;
      unsigned depthAnchors = 0u;
      for (const auto index : group.feedIndices) {
        assert(index < venue.feeds.size());
        bboAnchors += venue.feeds[index].anchor == AnchorRole::DirectBbo;
        tradeAnchors += venue.feeds[index].anchor == AnchorRole::Trade;
        depthAnchors += venue.feeds[index].anchor == AnchorRole::FastDepth;
      }
      assert(bboAnchors == 1u);
      assert(tradeAnchors == 1u);
      assert(depthAnchors == 1u);
    }
  }

  const std::vector<InstrumentRow> rows{
      {"MAGMA-USDT-SWAP", "MAGMA", "USDT", "SWAP", "online"},
      {"BTRUSDT", "BTR", "USDT", "PERPETUAL", "PreLaunch"},
      {"ETH-USD-SWAP", "ETH", "USD", "SWAP", "online"},
  };
  const auto magma = resolve_usdt_perpetual(rows, "magma");
  assert(magma.availability == InstrumentAvailability::Available);
  assert(magma.nativeSymbol == "MAGMA-USDT-SWAP");
  const auto btr = resolve_usdt_perpetual(rows, "BTR");
  assert(btr.availability == InstrumentAvailability::Unavailable);
  assert(resolve_usdt_perpetual(rows, "ETH").availability ==
         InstrumentAvailability::Unavailable);

  const std::string bybitJson = R"({
    "result":{"list":[
      {"symbol":"MAGMAUSDT","baseCoin":"MAGMA","quoteCoin":"USDT",
       "contractType":"LinearPerpetual","status":"Trading"}
    ]}
  })";
  std::vector<InstrumentRow> parsed;
  std::string error;
  assert(parse_instrument_response(
      Venue::Bybit, bybitJson.data(), bybitJson.size(), parsed, error));
  assert(parsed.size() == 1u);
  assert(parsed[0].nativeSymbol == "MAGMAUSDT");
}
