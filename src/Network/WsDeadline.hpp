#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <utility>

namespace exchange_probe::net_detail {

// Cold diagnostic WebSocket operation. The caller initiates exactly one Asio
// operation and supplies nonblocking socket cancellation. Both its completion
// and our timer cancellation are drained before borrowed buffers leave scope;
// unrelated Beast idle timer work is not drained after a successful read.
template<class Start, class Cancel>
[[nodiscard]] boost::system::error_code run_ws_deadline(
    boost::asio::io_context& context,
    std::chrono::steady_clock::time_point deadline,
    Start&& start, Cancel&& cancel) {
  if (std::chrono::steady_clock::now() >= deadline) {
    cancel();
    return boost::asio::error::timed_out;
  }
  boost::asio::steady_timer timer{context};
  timer.expires_at(deadline);
  bool operation_complete = false;
  bool timer_complete = false;
  bool timed_out = false;
  boost::system::error_code result;
  timer.async_wait([&](const boost::system::error_code& error) {
    timer_complete = true;
    if (!error && !operation_complete) {
      timed_out = true;
      cancel();
    }
  });
  try {
    std::forward<Start>(start)([&](const boost::system::error_code& error) {
      result = error;
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        cancel();
      }
      operation_complete = true;
      timer.cancel();
    });
  } catch (...) {
    // An initiating function that throws has not started its operation.
    cancel();
    timer.cancel();
    while (!timer_complete) {
      context.restart();
      context.run_one();
    }
    throw;
  }
  while (!operation_complete || !timer_complete) {
    context.restart();
    context.run_one();
  }
  context.restart();
  return timed_out ? boost::system::error_code{boost::asio::error::timed_out}
                   : result;
}
}  // namespace exchange_probe::net_detail
