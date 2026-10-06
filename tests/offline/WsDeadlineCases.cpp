#include "OfflineCase.hpp"
#include "../../src/Network/WsDeadline.hpp"

#include <boost/asio/post.hpp>
#include <functional>

namespace {
using Clock = std::chrono::steady_clock;
void expiredDeadlineDoesNotStartOperation() {
  boost::asio::io_context context;
  bool started = false, cancelled = false;
  const auto error = exchange_probe::net_detail::run_ws_deadline(
      context, Clock::now(), [&](auto) { started = true; },
      [&] { cancelled = true; });
  CXET_CHECK(error == boost::asio::error::timed_out && !started && cancelled);
  CXET_CHECK(context.poll() == 0u);
}
void completionDrainsOnlyOwnedTimer() {
  boost::asio::io_context context;
  bool cancelled = false;
  unsigned completions = 0u;
  const auto error = exchange_probe::net_detail::run_ws_deadline(
      context, Clock::now() + std::chrono::seconds{1},
      [&](auto done) { boost::asio::post(context, [&, done] { ++completions; done({}); }); },
      [&] { cancelled = true; });
  CXET_CHECK(!error && !cancelled && completions == 1u);
  CXET_CHECK(context.poll() == 0u);
}
void timeoutDrainsCancelledOperation() {
  boost::asio::io_context context;
  std::function<void(boost::system::error_code)> completion;
  bool cancelled = false;
  unsigned completions = 0u;
  const auto error = exchange_probe::net_detail::run_ws_deadline(
      context, Clock::now() + std::chrono::milliseconds{1},
      [&](auto done) { completion = done; },
      [&] {
        cancelled = true;
        boost::asio::post(context, [&] {
          ++completions;
          completion(boost::asio::error::operation_aborted);
        });
      });
  CXET_CHECK(error == boost::asio::error::timed_out && cancelled && completions == 1u);
  CXET_CHECK(context.poll() == 0u);
}
}  // namespace
int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      cxet::testing::Case{"ws.deadline_expired_refuses_operation_start", expiredDeadlineDoesNotStartOperation},
      cxet::testing::Case{"ws.deadline_completion_drains_owned_timer", completionDrainsOnlyOwnedTimer},
      cxet::testing::Case{"ws.deadline_timeout_drains_cancelled_operation", timeoutDrainsCancelledOperation},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
