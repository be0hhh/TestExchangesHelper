#pragma once

#include "exchange_probe/race/transport.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace exchange_probe::race {

class RecordWriter {
 public:
  RecordWriter(
      const std::filesystem::path& directory,
      const std::vector<ConnectionIngress*>& ingresses,
      const std::vector<ConnectionSpec>& specs);
  RecordWriter(const RecordWriter&) = delete;
  RecordWriter& operator=(const RecordWriter&) = delete;
  ~RecordWriter();

  [[nodiscard]] bool start();
  void stop() noexcept;
  [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] std::uint64_t records_written() const noexcept {
    return recordsWritten_.load(std::memory_order_relaxed);
  }

 private:
  void run() noexcept;

  std::filesystem::path directory_;
  std::vector<ConnectionIngress*> ingresses_;
  std::vector<ConnectionSpec> specs_;
  std::ofstream records_;
  std::ofstream sources_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<std::uint64_t> recordsWritten_{0u};
  std::string error_;
};

[[nodiscard]] bool write_connection_metadata(
    const std::filesystem::path& directory,
    const std::vector<ConnectionSpec>& specs,
    const std::vector<ConnectionCounters>& counters,
    const std::vector<ConnectionObservation>& observations,
    std::string& error);

}  // namespace exchange_probe::race
