#include "exchange_probe/race/Writer.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <chrono>
#include <system_error>

namespace exchange_probe::race {

RecordWriter::RecordWriter(
    const std::filesystem::path& directory,
    const std::vector<ConnectionIngress*>& ingresses,
    const std::vector<ConnectionSpec>& specs)
    : directory_(directory), ingresses_(ingresses), specs_(specs) {
  if (ingresses_.size() != specs_.size()) {
    error_ = "writer_source_count_mismatch";
    return;
  }
  std::error_code filesystemError;
  std::filesystem::create_directories(directory_, filesystemError);
  if (filesystemError) {
    error_ = "writer_directory:" + filesystemError.message();
    return;
  }
  records_.open(directory_ / "records.bin", std::ios::binary);
  sources_.open(directory_ / "sources.json", std::ios::binary);
  if (!records_ || !sources_) {
    error_ = "writer_open_failed";
    return;
  }
  boost::json::array sources;
  for (const auto& spec : specs_) {
    sources.emplace_back(boost::json::object{
        {"source_id", spec.source.sourceId},
        {"connection_id", spec.source.connectionId},
        {"feed_id", spec.feedId},
        {"host", spec.host},
        {"path", spec.path},
        {"logical_cpu", spec.logicalCpu},
    });
  }
  sources_ << boost::json::serialize(boost::json::object{
      {"schema", "exchange.feed_race.sources"},
      {"schema_version", 1},
      {"sources", std::move(sources)},
  }) << '\n';
  sources_.flush();
  if (!sources_) error_ = "writer_sources_failed";
}

RecordWriter::~RecordWriter() { stop(); }

bool RecordWriter::start() {
  if (!ok() || thread_.joinable()) return false;
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread{&RecordWriter::run, this};
  return true;
}

void RecordWriter::stop() noexcept {
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) thread_.join();
  records_.flush();
}

void RecordWriter::run() noexcept {
  bool pending = true;
  while (!stop_.load(std::memory_order_acquire) || pending) {
    pending = false;
    bool progressed = false;
    for (auto* ingress : ingresses_) {
      RaceRecord record{};
      while (ingress->consume(record)) {
        records_.write(
            reinterpret_cast<const char*>(&record),
            static_cast<std::streamsize>(sizeof(record)));
        recordsWritten_.fetch_add(1u, std::memory_order_relaxed);
        progressed = true;
      }
      pending = pending || ingress->published() != ingress->consumed();
    }
    if (!progressed) std::this_thread::yield();
  }
}

bool write_connection_metadata(
    const std::filesystem::path& directory,
    const std::vector<ConnectionSpec>& specs,
    const std::vector<ConnectionCounters>& counters,
    const std::vector<ConnectionObservation>& observations,
    std::string& error) {
  if (specs.size() != counters.size() || specs.size() != observations.size()) {
    error = "connection_metadata_count_mismatch";
    return false;
  }
  std::ofstream output{directory / "connections.json", std::ios::binary};
  if (!output) {
    error = "connection_metadata_open_failed";
    return false;
  }
  boost::json::array connections;
  for (std::size_t index = 0u; index < specs.size(); ++index) {
    const auto& spec = specs[index];
    const auto& counter = counters[index];
    const auto& observation = observations[index];
    boost::json::array generations;
    for (std::size_t generationIndex = 0u;
         generationIndex < observation.generationCount; ++generationIndex) {
      const auto& generation = observation.generations[generationIndex];
      generations.emplace_back(boost::json::object{
          {"generation", generation.generation},
          {"final_status", static_cast<std::uint8_t>(generation.finalStatus)},
          {"resolved_ip", generation.resolvedIp},
          {"remote_ip", generation.remoteIp},
          {"local_ip", generation.localIp},
          {"ip_family", generation.ipFamily},
          {"tls_version", generation.tlsVersion},
          {"tls_cipher", generation.tlsCipher},
          {"failure_stage", generation.failureStage},
          {"failure_reason", generation.failureReason},
          {"dns_ns", generation.dnsNs},
          {"tcp_connect_ns", generation.tcpConnectNs},
          {"tls_handshake_ns", generation.tlsHandshakeNs},
          {"ws_handshake_ns", generation.wsHandshakeNs},
      });
    }
    connections.emplace_back(boost::json::object{
        {"source_id", spec.source.sourceId},
        {"connection_id", spec.source.connectionId},
        {"feed_id", spec.feedId},
        {"hostname", spec.host},
        {"port", spec.port},
        {"path", spec.path},
        {"logical_cpu", spec.logicalCpu},
        {"frames", counter.frames.load(std::memory_order_relaxed)},
        {"bytes", counter.bytes.load(std::memory_order_relaxed)},
        {"malformed", counter.malformed.load(std::memory_order_relaxed)},
        {"normalized", counter.normalized.load(std::memory_order_relaxed)},
        {"reconnects", counter.reconnects.load(std::memory_order_relaxed)},
        {"final_generation", counter.generation.load(std::memory_order_relaxed)},
        {"final_status", counter.status.load(std::memory_order_relaxed)},
        {"generation_overflow", observation.generationOverflow},
        {"generations", std::move(generations)},
    });
  }
  output << boost::json::serialize(boost::json::object{
      {"schema", "exchange.feed_race.connections"},
      {"schema_version", 1},
      {"connections", std::move(connections)},
  }) << '\n';
  output.flush();
  if (!output) {
    error = "connection_metadata_write_failed";
    return false;
  }
  return true;
}

}  // namespace exchange_probe::race
