#pragma once

#include "exchange_probe/App.hpp"
#include "exchange_probe/TbankGrpcDiagnostic.hpp"

#include <cstdint>
#include <functional>
#include <iosfwd>

namespace exchange_probe::tbank_grpc::detail {
struct HandoffClock {
  std::function<std::uint64_t()> now;
  std::function<void()> idle;
};
int run_ping_handoff(const CliOptions&, const Factories&, std::ostream&, std::ostream&,
                     const HandoffClock&);
} // namespace exchange_probe::tbank_grpc::detail
