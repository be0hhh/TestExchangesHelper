#pragma once
#include "exchange_probe/App.hpp"
#include "exchange_probe/TbankGrpcDiagnostic.hpp"
#include <functional>
#include <string>
namespace exchange_probe::tbank_grpc::detail {
// Test seam; production resolves once and freezes the first numeric endpoint.
struct MarketSourcesRuntime {
  std::function<std::uint64_t()> now;
  std::function<void()> idle;
  std::function<std::string(std::uint64_t)> resolve;
  std::function<std::uint64_t()> realtime_now;
};
int run_market_sources(const CliOptions&, const Factories&, std::ostream&, std::ostream&,
                       const MarketSourcesRuntime&);
}
