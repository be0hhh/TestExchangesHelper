#pragma once

#include <functional>
#include <cstdint>

namespace exchange_probe::tbank_grpc::detail {

// Service the already-connected peer only while the shared stop is clear.
// A callback may set the stop, in which case no next network step may run.
inline bool service_other_unless_stopped(const std::function<void()>& service_other,
                                         const std::function<bool()>& stop_requested) {
  if (stop_requested && stop_requested()) return false;
  if (service_other) service_other();
  return !stop_requested || !stop_requested();
}

inline void record_tls_read(bool frame_based, std::uint64_t now, std::uint64_t& last_receive) noexcept {
  if (!frame_based) last_receive = now;
}
inline void record_http2_frame(bool frame_based, bool ping_ack, bool matched,
                               std::uint64_t now, std::uint64_t& last_receive) noexcept {
  if ((frame_based && !ping_ack) || (ping_ack && matched)) last_receive = now;
}

}  // namespace exchange_probe::tbank_grpc::detail
