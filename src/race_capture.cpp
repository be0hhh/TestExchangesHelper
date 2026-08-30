#include "exchange_probe/race/capture.hpp"

#include "race_capture_internal.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

namespace exchange_probe::race {
namespace capture_detail {

bool write_availability(
    const std::filesystem::path& directory, const char* venue,
    const std::vector<InstrumentResolution>& resolutions,
    std::string& error) {
  std::error_code filesystemError;
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    error = "availability_directory:" + filesystemError.message();
    return false;
  }
  boost::json::array instruments;
  for (const auto& resolution : resolutions) {
    instruments.emplace_back(boost::json::object{
        {"requested_base", resolution.requestedBase},
        {"availability", static_cast<std::uint8_t>(resolution.availability)},
        {"native_symbol", resolution.nativeSymbol},
        {"reason", resolution.reason},
    });
  }
  std::ofstream output{directory / "availability.json", std::ios::binary};
  if (!output) {
    error = "availability_open_failed";
    return false;
  }
  output << boost::json::serialize(boost::json::object{
      {"schema", "exchange.feed_race.availability"},
      {"schema_version", 1},
      {"venue", venue},
      {"instruments", std::move(instruments)},
  }) << '\n';
  output.flush();
  if (output) return true;
  error = "availability_write_failed";
  return false;
}

unsigned logical_cpu(std::size_t connectionOrdinal) noexcept {
  const auto concurrency = std::max(1u, std::thread::hardware_concurrency());
  return static_cast<unsigned>(connectionOrdinal % concurrency);
}

}  // namespace capture_detail

PublicCaptureResult run_public_capture(const PublicCaptureOptions& options) {
  PublicCaptureResult result{};
  if (options.outputDirectory.empty() || options.requestedBases.empty() ||
      options.requestedBases.size() > 64u || options.venues.empty() ||
      options.venues.size() > 7u || options.sessions == 0u ||
      options.sessions > 100u || options.warmup.count() < 0 ||
      options.warmup > std::chrono::minutes{10} ||
      options.measured.count() <= 0 ||
      options.measured > std::chrono::hours{1}) {
    result.error = "capture_options_invalid";
    return result;
  }
  for (const auto& base : options.requestedBases) {
    if (base.empty() || base.size() > 32u) {
      result.error = "capture_symbol_bounds";
      return result;
    }
  }
  std::set<std::string> uniqueVenues;
  for (const auto& venue : options.venues) {
    if (!uniqueVenues.insert(venue).second) {
      result.error = "capture_venue_duplicate:" + venue;
      return result;
    }
    PublicCaptureResult venueResult{};
    if (venue == "bitget") {
      venueResult = capture_detail::capture_bitget(options);
    } else if (venue == "bybit") {
      venueResult = capture_detail::capture_bybit(options);
    } else {
      result.error = "capture_venue_not_implemented:" + venue;
      return result;
    }
    result.degraded = result.degraded || venueResult.degraded;
    result.sessionsCompleted += venueResult.sessionsCompleted;
    result.availableInstruments += venueResult.availableInstruments;
    result.unavailableInstruments += venueResult.unavailableInstruments;
    if (!venueResult.complete) {
      result.error = venueResult.error;
      return result;
    }
  }
  result.complete = true;
  return result;
}

}  // namespace exchange_probe::race
