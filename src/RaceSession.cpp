#include "exchange_probe/race/Run.hpp"

#include "exchange_probe/race/Clock.hpp"
#include "exchange_probe/race/Writer.hpp"

#include <chrono>
#include <iostream>
#include <thread>

namespace exchange_probe::race {

GroupRunResult run_group_session(
    const GroupRunOptions& options,
    const std::vector<GroupConnection>& connections) {
  GroupRunResult result{};
  if (connections.empty() || options.outputDirectory.empty()) {
    result.error = "group_configuration_invalid";
    return result;
  }
  ReadyBarrier ready{connections.size()};
  SessionControl control{};
  std::vector<std::unique_ptr<ConnectionIngress>> ingressOwners;
  std::vector<ConnectionIngress*> ingresses;
  std::vector<ConnectionSpec> specs;
  std::vector<RawSampleStore> samples(connections.size());
  std::vector<ConnectionCounters> counters(connections.size());
  result.observations.resize(connections.size());
  ingressOwners.reserve(connections.size());
  ingresses.reserve(connections.size());
  specs.reserve(connections.size());
  for (const auto& connection : connections) {
    ingressOwners.push_back(std::make_unique<ConnectionIngress>());
    ingresses.push_back(ingressOwners.back().get());
    specs.push_back(connection.spec);
  }
  RecordWriter writer{options.outputDirectory, ingresses, specs};
  if (!writer.start()) {
    result.error = writer.error();
    return result;
  }
  std::vector<ConnectionRuntime> runtimes(connections.size());
  std::vector<std::thread> receivers;
  receivers.reserve(connections.size());
  for (std::size_t index = 0u; index < connections.size(); ++index) {
    runtimes[index] = ConnectionRuntime{
        .spec = &connections[index].spec,
        .ingress = ingresses[index],
        .ready = &ready,
        .readyIndex = index,
        .control = &control,
        .normalizer = connections[index].normalizer,
        .resetNormalizer = connections[index].resetNormalizer,
        .normalizerState = connections[index].normalizerState,
        .samples = &samples[index],
        .counters = &counters[index],
        .observation = &result.observations[index],
    };
    receivers.emplace_back([&runtimes, index] {
      capture_connection(runtimes[index]);
    });
  }
  const auto readyDeadline =
      std::chrono::steady_clock::now() + options.readyTimeout;
  auto nextStatus =
      std::chrono::steady_clock::now() + options.statusInterval;
  while (!ready.ready() && std::chrono::steady_clock::now() < readyDeadline) {
    if (options.printStatus && std::chrono::steady_clock::now() >= nextStatus) {
      std::cerr << "exchange-feed-race ready " << ready.terminal_count()
                << '/' << connections.size() << '\n';
      nextStatus += options.statusInterval;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  if (!ready.ready()) {
    result.error = "ready_timeout";
    control.stop.store(true, std::memory_order_release);
  } else {
    result.readyReached = true;
    if (options.warmup.count() > 0) std::this_thread::sleep_for(options.warmup);
    const auto start = now_mono_raw_ns();
    control.measuredStartMonoNs.store(start, std::memory_order_release);
    control.measuredEndMonoNs.store(
        start + static_cast<std::uint64_t>(options.measured.count()) *
                    1'000'000'000u,
        std::memory_order_release);
    const auto measuredDeadline =
        std::chrono::steady_clock::now() + options.measured;
    while (std::chrono::steady_clock::now() < measuredDeadline) {
      if (options.printStatus && std::chrono::steady_clock::now() >= nextStatus) {
        std::uint64_t frames = 0u;
        std::uint64_t drops = 0u;
        for (std::size_t index = 0u; index < counters.size(); ++index) {
          frames += counters[index].frames.load(std::memory_order_relaxed);
          drops += ingresses[index]->drops();
        }
        std::cerr << "exchange-feed-race measured frames=" << frames
                  << " drops=" << drops << '\n';
        nextStatus += options.statusInterval;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    control.stop.store(true, std::memory_order_release);
  }
  for (auto& receiver : receivers) {
    if (receiver.joinable()) receiver.join();
  }
  writer.stop();
  std::string metadataError;
  if (!write_connection_metadata(
          options.outputDirectory, specs, counters, result.observations,
          metadataError)) {
    result.error = metadataError;
  }
  result.complete = result.readyReached && result.error.empty();
  for (std::size_t index = 0u; index < ingressOwners.size(); ++index) {
    const auto status = static_cast<FeedStatus>(
        counters[index].status.load(std::memory_order_relaxed));
    result.degraded = result.degraded || ingressOwners[index]->degraded() ||
                      counters[index].reconnects.load(
                          std::memory_order_relaxed) != 0u ||
                      status == FeedStatus::Disconnected ||
                      status == FeedStatus::Failed ||
                      status == FeedStatus::Degraded;
  }
  return result;
}

}  // namespace exchange_probe::race
