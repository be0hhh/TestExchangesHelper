#include "exchange_probe/Research.hpp"
#include "exchange_probe/ResearchProfile.hpp"

#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "research test failed: " << message << '\n';
    std::exit(1);
  }
}

void write_line(
    std::ofstream& output,
    const boost::json::object& value) {
  output << boost::json::serialize(value) << '\n';
}

}  // namespace

int main() {
  using namespace exchange_probe;
  const auto catalog =
      load_research_catalog(EXCHANGE_PROBE_TEST_PROFILE_ROOT);
  require(catalog.ok, "built-in catalog validates");
  require(catalog.catalog.products.size() == 26U, "26 product profiles");

  const char* requested_root =
      std::getenv("EXCHANGE_PROBE_TEST_BUNDLE_DIR");
  const bool keep_bundle =
      requested_root != nullptr && requested_root[0] != '\0';
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = keep_bundle
                        ? std::filesystem::path{requested_root}
                        : std::filesystem::temp_directory_path() /
                              ("exchange-probe-research-" +
                               std::to_string(suffix));
  if (keep_bundle) {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  std::filesystem::create_directories(root);

  const std::string trade =
      R"({"E":1000,"t":7,"s":"BTCUSDT","p":"100.01","q":"1"})";
  const std::string book =
      R"({"E":1000,"u":9,"s":"BTCUSDT","b":"99.99","B":"2","a":"100.01","A":"3"})";
  {
    std::ofstream frames{root / "frames.bin", std::ios::binary};
    frames << trade << book;
  }
  {
    std::ofstream index{root / "frames.jsonl", std::ios::binary};
    write_line(index, {
        {"schema", kResearchFrameSchema},
        {"frame_id", 0},
        {"round", 1},
        {"channel_id", "trades"},
        {"opcode", "text"},
        {"monotonic_ns", 1'000'000'000},
        {"utc_ns", 2'000'000'000},
        {"offset", 0},
        {"length", trade.size()},
    });
    write_line(index, {
        {"schema", kResearchFrameSchema},
        {"frame_id", 1},
        {"round", 1},
        {"channel_id", "book"},
        {"opcode", "text"},
        {"monotonic_ns", 1'060'000'000},
        {"utc_ns", 2'060'000'000},
        {"offset", trade.size()},
        {"length", book.size()},
    });
  }
  {
    boost::json::object trade_mapping{
        {"data", "$"}, {"symbol", "s"}, {"event_time", "E"},
        {"event_time_unit", "ms"}, {"event_id", "t"},
        {"price", "p"}, {"quantity", "q"},
    };
    boost::json::object book_mapping{
        {"data", "$"}, {"symbol", "s"}, {"event_time", "E"},
        {"event_time_unit", "ms"}, {"event_id", "u"},
        {"bid_price", "b"}, {"bid_quantity", "B"},
        {"ask_price", "a"}, {"ask_quantity", "A"},
    };
    boost::json::array channels{
        boost::json::object{
            {"id", "trades"}, {"kind", "trade"}, {"support", "exact"},
            {"transport", "ws"}, {"wire", "json"}, {"mapping", trade_mapping},
        },
        boost::json::object{
            {"id", "book"}, {"kind", "book_ticker"}, {"support", "exact"},
            {"transport", "ws"}, {"wire", "json"}, {"mapping", book_mapping},
        },
    };
    std::ofstream manifest{root / "manifest.json", std::ios::binary};
    write_line(manifest, {
        {"schema", kResearchBundleSchema},
        {"schema_version", 3},
        {"run_id", "offline-60ms"},
        {"status", "complete"},
        {"artifact_complete", true},
        {"venue", "fixture"},
        {"product", "futures"},
        {"symbol", "BTCUSDT"},
        {"channels", channels},
    });
  }

  std::ostringstream output;
  std::ostringstream errors;
  require(
      analyze_research_bundle(root, output, errors) == 0,
      "offline analysis succeeds");
  std::ifstream relations{root / "relations.jsonl", std::ios::binary};
  const std::string relation_text{
      std::istreambuf_iterator<char>{relations},
      std::istreambuf_iterator<char>{}};
  require(
      relation_text.find("\"receive_lag_ns\":60000000") !=
          std::string::npos,
      "exact 60 ms receive lag preserved");
  require(
      relation_text.find("\"causality\":\"not_asserted\"") !=
          std::string::npos,
      "causality remains unasserted");

  if (keep_bundle) {
    std::cout << root.string() << '\n';
  } else {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  return 0;
}
