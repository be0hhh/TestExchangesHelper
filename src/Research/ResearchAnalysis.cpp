#include "exchange_probe/Research.hpp"
#include "exchange_probe/Contracts.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Analysis/ResearchAnalysisInternal.hpp"

namespace exchange_probe {

using namespace research_analysis;

int analyze_research_bundle(
    const std::filesystem::path& directory,
    std::ostream& output,
    std::ostream& error_output) {
  std::string error;
  auto manifest = read_object(directory / "manifest.json", error);
  if (!manifest.has_value()) {
    error_output << "analysis_error: " << error << '\n';
    return 2;
  }
  if (text(*manifest, "schema") != kResearchBundleSchema) {
    error_output << "analysis_error: incompatible_bundle_schema\n";
    return 2;
  }
  if (u64(*manifest, "schema_version") != 3U) {
    error_output << "analysis_error: incompatible_bundle_version\n";
    return 2;
  }
  auto frames_index =
      read_frame_index(directory / "frames.jsonl", error);
  if (!error.empty()) {
    error_output << "analysis_error: " << error << '\n';
    return 2;
  }
  std::error_code frames_size_error;
  const auto frames_size = std::filesystem::file_size(
      directory / "frames.bin", frames_size_error);
  if (frames_size_error || frames_size > kMaximumFramesFileBytes) {
    error_output
        << "analysis_error: "
        << (frames_size_error ? "frames_size_failed"
                              : "frames_capacity_exceeded")
        << '\n';
    return 2;
  }
  for (const auto& frame : frames_index) {
    if (frame.offset + frame.length > frames_size) {
      error_output << "analysis_error: frame_index_out_of_bounds\n";
      return 2;
    }
  }
  std::ifstream frames{directory / "frames.bin", std::ios::binary};
  if (!frames) {
    error_output << "analysis_error: frames_open_failed\n";
    return 2;
  }
  const auto definitions = channel_definitions(*manifest);
  if (definitions.empty()) {
    error_output << "analysis_error: manifest_has_no_channels\n";
    return 2;
  }
  std::ofstream events_file{directory / "events.jsonl", std::ios::binary};
  std::ofstream states_file{
      directory / "state_transitions.jsonl", std::ios::binary};
  std::ofstream relations_file{
      directory / "relations.jsonl", std::ios::binary};
  if (!events_file || !states_file || !relations_file) {
    error_output << "analysis_error: output_open_failed\n";
    return 2;
  }
  std::vector<NormalizedEvent> events;
  events.reserve(frames_index.size());
  std::map<std::pair<unsigned, std::string>, BookState> books;
  std::unordered_map<std::uint64_t, FrameIndex> first_frame_hash;
  std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>>
      native_identity;
  std::uint64_t exact_frame_duplicates = 0U;
  std::uint64_t semantic_duplicates = 0U;
  std::uint64_t identity_collisions = 0U;
  std::uint64_t gaps = 0U;
  std::uint64_t out_of_order = 0U;
  std::uint64_t book_capacity_exceeded = 0U;
  std::uint64_t valid_bbo_transitions = 0U;
  std::uint64_t next_event_id = 0U;
  std::map<std::string, std::uint64_t> validity_counts;
  bool full_reconstruction_observed = false;
  for (const auto& frame : frames_index) {
    const auto definition = definitions.find(frame.channel);
    if (definition == definitions.end()) continue;
    auto payload = read_payload(frames, frame, error);
    if (!error.empty()) {
      error_output << "analysis_error: " << error << '\n';
      return 2;
    }
    const auto hash = fnv1a(payload);
    const auto [first, inserted] =
        first_frame_hash.emplace(hash, frame);
    if (!inserted && first->second.length == frame.length) {
      auto first_payload = read_payload(frames, first->second, error);
      if (!error.empty()) {
        error_output << "analysis_error: " << error << '\n';
        return 2;
      }
      if (first_payload == payload) ++exact_frame_duplicates;
    }
    std::string decoded_payload;
    std::string decode_error;
    std::string_view semantic_payload{payload};
    if (definition->second.compression == "gzip") {
      if (decode_gzip_bounded(
              payload, kMaxWsMessageBytes, decoded_payload,
              decode_error)) {
        semantic_payload = decoded_payload;
      }
    }
    boost::system::error_code parse_error;
    auto value = boost::json::parse(semantic_payload, parse_error);
    NormalizedEvent event;
    if (!decode_error.empty() || parse_error) {
      event = {
          .id = next_event_id++,
          .frame_id = frame.id,
          .round = frame.round,
          .channel = frame.channel,
          .kind = definition->second.kind,
          .monotonic_ns = frame.monotonic_ns,
          .utc_ns = frame.utc_ns,
          .semantic_hash = hash,
          .validity = !decode_error.empty()
                          ? "decompression_error:" + decode_error
                          : frame.binary ? "binary_adapter_required"
                                         : "json_parse_error",
      };
    } else {
      event = normalize_event(
          frame, definition->second, value, next_event_id++, hash);
      event.semantic_hash = semantic_event_hash(event);
    }
    enforce_normalized_capacity(event);
    if (!event.native_id.empty()) {
      const auto key =
          event.channel + ":" + std::to_string(event.round) + ":" +
          event.native_id;
      const auto [iterator, identity_inserted] =
          native_identity.emplace(key, std::pair{event.id, event.semantic_hash});
      if (!identity_inserted) {
        if (iterator->second.second == event.semantic_hash) {
          ++semantic_duplicates;
        } else {
          ++identity_collisions;
        }
      }
    }
    if (event.kind == ResearchChannelKind::BookTicker) {
      if (!event.bid_price.empty() && !event.ask_price.empty()) {
        ++valid_bbo_transitions;
        states_file << boost::json::serialize(boost::json::object{
            {"schema", "exchange.api_probe.state_transition.v1"},
            {"event_id", event.id},
            {"round", event.round},
            {"channel_id", event.channel},
            {"kind", "bbo"},
            {"bid_price", event.bid_price},
            {"bid_quantity", event.bid_quantity},
            {"ask_price", event.ask_price},
            {"ask_quantity", event.ask_quantity},
            {"monotonic_ns", event.monotonic_ns},
            {"exchange_event_ns",
             event.exchange_event_ns.has_value()
                 ? boost::json::value(*event.exchange_event_ns)
                 : boost::json::value{}},
            {"valid", true},
        }) << '\n';
      }
    } else if (event.kind == ResearchChannelKind::Depth && !parse_error) {
      const auto* data =
          definition->second.mapping.data_path.empty()
              ? &value
              : json_path(value, definition->second.mapping.data_path);
      if (data == nullptr) data = &value;
      auto& state = books[{event.round, event.channel}];
      if (event.snapshot) {
        state.bids.clear();
        state.asks.clear();
        state.invalid_gap = false;
        state.complete = true;
        state.capacity_exceeded = false;
      }
      if (!event.previous_native_id.empty() &&
          !state.last_native_id.empty() &&
          event.previous_native_id != state.last_native_id) {
        state.invalid_gap = true;
        ++gaps;
      }
      if (!event.native_id.empty() &&
          !state.last_native_id.empty() &&
          native_id_less(event.native_id, state.last_native_id)) {
        ++out_of_order;
      }
      apply_levels(
          state,
          json_path(*data, definition->second.mapping.bids_path),
          true);
      apply_levels(
          state,
          json_path(*data, definition->second.mapping.asks_path),
          false);
      if (!event.native_id.empty()) state.last_native_id = event.native_id;
      const auto [bid, ask] = best_levels(state);
      event.bid_price = bid;
      event.ask_price = ask;
      const bool valid = !state.invalid_gap && !bid.empty() && !ask.empty() &&
                         !state.capacity_exceeded &&
                         DecimalLess{}(bid, ask);
      if (state.capacity_exceeded) {
        event.validity = "book_capacity_exceeded";
        ++book_capacity_exceeded;
      }
      if (valid && (bid != state.last_bid || ask != state.last_ask)) {
        ++valid_bbo_transitions;
        states_file << boost::json::serialize(boost::json::object{
            {"schema", "exchange.api_probe.state_transition.v1"},
            {"event_id", event.id},
            {"round", event.round},
            {"channel_id", event.channel},
            {"kind", "depth_derived_bbo"},
            {"bid_price", bid},
            {"ask_price", ask},
            {"monotonic_ns", event.monotonic_ns},
            {"exchange_event_ns",
             event.exchange_event_ns.has_value()
                 ? boost::json::value(*event.exchange_event_ns)
                 : boost::json::value{}},
            {"valid", true},
            {"complete", state.complete},
        }) << '\n';
        state.last_bid = bid;
        state.last_ask = ask;
      }
      full_reconstruction_observed |= valid && state.complete;
      state.valid = valid;
    }
    ++validity_counts[event.validity];
    events_file << boost::json::serialize(event_json(event)) << '\n';
    const auto event_position = events_file.tellp();
    const auto state_position = states_file.tellp();
    const auto event_offset =
        static_cast<std::streamoff>(event_position);
    const auto state_offset =
        static_cast<std::streamoff>(state_position);
    if (event_offset < 0 || state_offset < 0 ||
        static_cast<std::uint64_t>(event_offset) >
            kMaximumDerivedJsonlBytes ||
        static_cast<std::uint64_t>(state_offset) >
            kMaximumDerivedJsonlBytes) {
      error_output << "analysis_error: derived_artifact_capacity_exceeded\n";
      return 2;
    }
    events.push_back(std::move(event));
  }

  std::uint64_t relation_id = 0U;
  bool relation_output_full = false;
  std::unordered_map<std::string, std::size_t> identity_index;
  std::unordered_map<std::string, std::size_t> exchange_time_index;
  std::unordered_map<std::string, std::size_t> transaction_time_index;
  std::unordered_map<std::string, std::size_t> state_index;
  std::deque<std::size_t> recent_market_events;
  std::uint64_t relation_candidates_dropped = 0U;
  constexpr std::size_t kMaximumMarketCandidatesPerEvent = 128U;
  for (std::size_t index = 0U; index < events.size(); ++index) {
    const auto& target = events[index];
    if (!target.native_id.empty()) {
      const auto key =
          std::to_string(target.round) + ":" + target.native_id;
      const auto previous = identity_index.find(key);
      if (previous != identity_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                "identity", "observed",
                events[previous->second], target,
                "same_native_id_cross_channel_unverified_scope")) {
          ++relation_candidates_dropped;
        }
      }
      identity_index[key] = index;
    }
    if (target.exchange_event_ns.has_value()) {
      const auto key =
          std::to_string(target.round) + ":" +
          std::to_string(*target.exchange_event_ns);
      const auto previous = exchange_time_index.find(key);
      if (previous != exchange_time_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                "exchange_time_cohort",
                "consistent", events[previous->second], target,
                "same_exchange_event_time")) {
          ++relation_candidates_dropped;
        }
      }
      exchange_time_index[key] = index;
    }
    if (target.exchange_transaction_ns.has_value()) {
      const auto key =
          std::to_string(target.round) + ":" +
          std::to_string(*target.exchange_transaction_ns);
      const auto previous = transaction_time_index.find(key);
      if (previous != transaction_time_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                "exchange_time_cohort",
                "consistent", events[previous->second], target,
                "same_exchange_transaction_time")) {
          ++relation_candidates_dropped;
        }
      }
      transaction_time_index[key] = index;
    }
    const bool target_book =
        target.kind == ResearchChannelKind::BookTicker ||
        target.kind == ResearchChannelKind::Depth;
    if (target_book && !target.bid_price.empty() &&
        !target.ask_price.empty()) {
      const auto key =
          std::to_string(target.round) + ":" + target.bid_price + ":" +
          target.ask_price;
      const auto previous = state_index.find(key);
      if (previous != state_index.end() &&
          events[previous->second].channel != target.channel) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                "state_convergence",
                "consistent", events[previous->second], target,
                "same_resulting_bbo")) {
          ++relation_candidates_dropped;
        }
      }
      state_index[key] = index;
    }
    while (!recent_market_events.empty() &&
           events[recent_market_events.front()].monotonic_ns +
                   kMarketEffectWindowNs <
               target.monotonic_ns) {
      recent_market_events.pop_front();
    }
    std::size_t considered = 0U;
    for (auto iterator = recent_market_events.rbegin();
         iterator != recent_market_events.rend();
         ++iterator) {
      if (considered++ >= kMaximumMarketCandidatesPerEvent) {
        ++relation_candidates_dropped;
        break;
      }
      const auto& source = events[*iterator];
      if (source.round != target.round ||
          source.channel == target.channel) {
        continue;
      }
      const bool source_book =
          source.kind == ResearchChannelKind::BookTicker ||
          source.kind == ResearchChannelKind::Depth;
      const auto& book = source_book ? source : target;
      const auto& trade =
          source.kind == ResearchChannelKind::Trade ? source : target;
      const bool book_trade_pair =
          (source_book && target.kind == ResearchChannelKind::Trade) ||
          (target_book && source.kind == ResearchChannelKind::Trade);
      const bool price_touches_or_crosses =
          !trade.price.empty() && !book.bid_price.empty() &&
          !book.ask_price.empty() &&
          (!DecimalLess{}(book.bid_price, trade.price) ||
           !DecimalLess{}(trade.price, book.ask_price));
      if (book_trade_pair && price_touches_or_crosses) {
        if (!emit_relation(
                relations_file, relation_id, relation_output_full,
                "market_effect", "heuristic",
                source, target,
                "trade_touches_or_crosses_bbo_within_receive_window")) {
          ++relation_candidates_dropped;
        }
      }
    }
    if (target_book || target.kind == ResearchChannelKind::Trade) {
      recent_market_events.push_back(index);
    }
  }

  events_file.flush();
  states_file.flush();
  relations_file.flush();
  if (!events_file || !states_file || !relations_file) {
    error_output << "analysis_error: artifact_write_failed\n";
    return 2;
  }
  boost::json::object validity_summary;
  for (const auto& [validity, count] : validity_counts) {
    validity_summary[validity] = count;
  }
  boost::json::object findings{
      {"schema", kResearchFindingsSchema},
      {"evidence_policy",
       "relations are observations; heuristic edges do not assert causality"},
      {"frames", frames_index.size()},
      {"events", events.size()},
      {"relations", relation_id},
      {"relation_output_full", relation_output_full},
      {"relation_candidates_dropped", relation_candidates_dropped},
      {"valid_bbo_transitions", valid_bbo_transitions},
      {"event_validity", std::move(validity_summary)},
      {"full_bbo_reconstruction",
       boost::json::object{
           {"status",
            full_reconstruction_observed
                ? "observed"
                : "requires_sequence_aligned_snapshot_evidence"},
       }},
      {"anomalies",
       boost::json::object{
           {"exact_frame_duplicates", exact_frame_duplicates},
           {"semantic_duplicates", semantic_duplicates},
           {"identity_collisions", identity_collisions},
           {"sequence_gaps", gaps},
           {"out_of_order", out_of_order},
           {"book_capacity_exceeded", book_capacity_exceeded},
       }},
      {"relation_modes",
       boost::json::array{
           "identity",
           "exchange_time_cohort",
           "market_effect",
           "state_convergence",
       }},
  };
  std::ofstream findings_file{
      directory / "findings.json", std::ios::binary};
  findings_file << boost::json::serialize(findings) << '\n';
  std::ofstream report{directory / "REPORT.md", std::ios::binary};
  report
      << "# Exchange channel research\n\n"
      << "- Evidence: diagnostic observation; causality is not asserted.\n"
      << "- Frames/events/relations: " << frames_index.size() << '/'
      << events.size() << '/' << relation_id << "\n"
      << "- Valid BBO transitions: " << valid_bbo_transitions << "\n"
      << "- Exact frame duplicates: " << exact_frame_duplicates << "\n"
      << "- Semantic duplicates: " << semantic_duplicates << "\n"
      << "- Identity collisions: " << identity_collisions << "\n"
      << "- Sequence gaps/out-of-order: " << gaps << '/' << out_of_order
      << "\n"
      << "- A usable BBO is distinct from a complete sequence-aligned "
         "book reconstruction.\n"
      << "- Exchange and local receive clocks remain separate unless clock "
         "calibration is explicitly present.\n";
  findings_file.flush();
  report.flush();
  if (!findings_file || !report) {
    error_output << "analysis_error: summary_write_failed\n";
    return 2;
  }
  output << "analysis_bundle=" << directory.string()
         << " events=" << events.size()
         << " relations=" << relation_id << '\n';
  return 0;
}

}  // namespace exchange_probe
