#include "exchange_probe/app.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace exchange_probe {
namespace {

[[nodiscard]] std::filesystem::path default_discovery_output(
    const ResearchProductProfile& profile) {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return std::filesystem::path{"results"} /
         ("discovery-" + profile.venue + "-" + profile.product + "-" +
          std::to_string(now));
}

[[nodiscard]] boost::json::object profile_scaffold(
    const CliOptions& options) {
  const auto venue =
      options.venues.empty() ? std::string{"example"} : options.venues.front();
  const auto product =
      options.products.empty() ? std::string{"spot"} : options.products.front();
  return {
      {"schema", kResearchProfileSchema},
      {"venue", venue},
      {"display_name", venue},
      {"provenance", "candidate"},
      {"products",
       boost::json::array{
           boost::json::object{
               {"id", product},
               {"default_symbol",
                options.symbol.empty() ? "BTCUSDT" : options.symbol},
               {"symbol_format", "{base}{quote}"},
               {"channels",
                boost::json::array{
                    boost::json::object{
                        {"id", "trades"},
                        {"name", "Public trades"},
                        {"kind", "trade"},
                        {"support", "candidate"},
                        {"transport", "ws"},
                        {"wire", "json"},
                        {"host", "stream.example.com"},
                        {"port", 443},
                        {"path", "/ws"},
                        {"subscribe", R"({"op":"subscribe","topic":"trades"})"},
                        {"subscribe_binary", false},
                        {"compression", "none"},
                        {"depth_semantics", "none"},
                        {"depth_levels", 0},
                        {"adapter", ""},
                        {"capabilities", boost::json::array{"live_trades"}},
                        {"mapping",
                         boost::json::object{
                             {"data", "data"},
                             {"symbol", "symbol"},
                             {"event_time", "E"},
                             {"transaction_time", "T"},
                             {"event_id", "id"},
                             {"price", "price"},
                             {"quantity", "quantity"},
                             {"side", "side"},
                         }},
                    },
                }},
               {"discovery", boost::json::array{}},
           },
       }},
  };
}

[[nodiscard]] std::optional<boost::json::object> read_bounded_object(
    const std::filesystem::path& path,
    std::string& error) {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size > kMaximumProfileBytes) {
    error = filesystem_error
                ? "proposal_size_failed:" + filesystem_error.message()
                : "proposal_capacity_exceeded";
    return std::nullopt;
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    error = "proposal_open_failed";
    return std::nullopt;
  }
  const std::string content{
      std::istreambuf_iterator<char>{input},
      std::istreambuf_iterator<char>{}};
  boost::system::error_code parse_error;
  auto value = boost::json::parse(content, parse_error);
  if (parse_error || !value.is_object()) {
    error = "proposal_json_invalid";
    return std::nullopt;
  }
  return value.as_object();
}

void emit_profile_rows(
    const std::vector<const ResearchProductProfile*>& profiles,
    bool jsonl,
    std::ostream& output) {
  for (const auto* profile : profiles) {
    const auto value = research_profile_json(*profile);
    if (jsonl) {
      output << boost::json::serialize(value) << '\n';
    } else {
      output << profile->venue << '\t' << profile->product << '\t'
             << profile->default_symbol << '\t'
             << profile->channels.size() << " channels\t"
             << profile->discovery.size() << " candidates\n";
    }
  }
}

[[nodiscard]] std::string replace_symbol(
    std::string value,
    std::string_view symbol) {
  constexpr std::string_view token{"{symbol}"};
  std::size_t position = 0U;
  while ((position = value.find(token, position)) != std::string::npos) {
    value.replace(position, token.size(), symbol);
    position += symbol.size();
  }
  return value;
}

[[nodiscard]] WsCase candidate_ws_case(
    const DiscoveryCandidate& candidate,
    std::string_view symbol) {
  return {
      .name = candidate.id,
      .host = candidate.host,
      .path = replace_symbol(candidate.path, symbol),
      .subscribe = replace_symbol(candidate.subscribe, symbol),
      .capabilities = {"discovery_candidate"},
      .selection = Selection::DiagnosticVariant,
      .wire = Wire::Json,
      .data_kind = WsDataKind::AnyJson,
      .subscribe_binary = candidate.subscribe_binary,
      .require_ack = false,
      .require_data = true,
      .data_implies_ack = true,
  };
}

[[nodiscard]] unsigned discovery_limit(PlacementMode mode) noexcept {
  switch (mode) {
    case PlacementMode::Low: return 16U;
    case PlacementMode::Standard: return 64U;
    case PlacementMode::High: return kMaximumDiscoveryCandidates;
  }
  return 16U;
}

[[nodiscard]] unsigned discovery_attempts(PlacementMode mode) noexcept {
  return mode == PlacementMode::Low ? 1U : 2U;
}

[[nodiscard]] std::chrono::milliseconds discovery_pause(
    PlacementMode mode) noexcept {
  return mode == PlacementMode::Low
             ? std::chrono::milliseconds{500}
             : std::chrono::milliseconds{250};
}

}  // namespace

int run_profile_command(
    const CliOptions& options,
  std::ostream& output,
  std::ostream& error_output) {
  if (options.action == "scaffold") {
    const auto scaffold = profile_scaffold(options);
    if (!options.output_dir.has_value()) {
      output << boost::json::serialize(scaffold) << '\n';
      return 0;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(
        *options.output_dir, filesystem_error);
    if (filesystem_error) {
      error_output << "profile_error: output_directory_create_failed:"
                   << filesystem_error.message() << '\n';
      return 2;
    }
    const auto venue =
        options.venues.empty() ? std::string{"example"}
                               : options.venues.front();
    const auto path = *options.output_dir / (venue + ".json");
    if (std::filesystem::exists(path, filesystem_error)) {
      error_output << "profile_error: scaffold_output_exists\n";
      return 2;
    }
    if (filesystem_error) {
      error_output << "profile_error: scaffold_output_inspection_failed:"
                   << filesystem_error.message() << '\n';
      return 2;
    }
    std::ofstream file{path, std::ios::binary};
    file << boost::json::serialize(scaffold) << '\n';
    file.flush();
    if (!file) {
      error_output << "profile_error: scaffold_write_failed\n";
      return 2;
    }
    output << "profile_scaffold=" << path.string() << '\n';
    return 0;
  }
  auto loaded = load_research_catalog(options.profile_root);
  if (!loaded.ok) {
    error_output << "profile_error: " << loaded.error << '\n';
    return 2;
  }
  if (options.action == "validate") {
    const auto result = validate_research_catalog(loaded.catalog);
    output << boost::json::serialize(result) << '\n';
    return result.at("ok").as_bool() ? 0 : 1;
  }
  if (options.action == "promote") {
    std::string proposal_error;
    auto proposal =
        read_bounded_object(options.inputs.front(), proposal_error);
    if (!proposal.has_value() ||
        proposal->if_contains("schema") == nullptr ||
        !proposal->at("schema").is_string() ||
        proposal->at("schema").as_string() !=
            "exchange.api_probe.profile_proposal.v1" ||
        proposal->if_contains("venue") == nullptr ||
        !proposal->at("venue").is_string() ||
        proposal->if_contains("product") == nullptr ||
        !proposal->at("product").is_string() ||
        proposal->if_contains("accepted_candidates") == nullptr ||
        !proposal->at("accepted_candidates").is_array()) {
      error_output << "profile_error: "
                   << (proposal_error.empty()
                           ? "proposal_contract_invalid"
                           : proposal_error)
                   << '\n';
      return 2;
    }
    const auto venue = std::string{proposal->at("venue").as_string()};
    const auto product = std::string{proposal->at("product").as_string()};
    const auto* existing =
        find_research_profile(loaded.catalog, venue, product);
    if (existing == nullptr) {
      error_output << "profile_error: proposal_profile_not_found\n";
      return 2;
    }
    boost::json::object review{
        {"schema", "exchange.api_probe.promotion_review.v1"},
        {"venue", venue},
        {"product", product},
        {"catalog_mutated", false},
        {"existing_profile", research_profile_json(*existing)},
        {"accepted_candidates", proposal->at("accepted_candidates")},
        {"required_review",
         boost::json::array{
             "wire_identity",
             "subscription_ack",
             "timestamp_units",
             "sequence_semantics",
             "depth_reconstruction",
             "duplicate_identity_scope",
         }},
    };
    const auto directory =
        options.output_dir.value_or(std::filesystem::path{"proposals"});
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
      error_output << "profile_error: output_directory_create_failed:"
                   << filesystem_error.message() << '\n';
      return 2;
    }
    const auto path =
        directory / (venue + "-" + product + "-promotion-review.json");
    if (std::filesystem::exists(path, filesystem_error)) {
      error_output << "profile_error: promotion_review_exists\n";
      return 2;
    }
    if (filesystem_error) {
      error_output
          << "profile_error: promotion_review_inspection_failed:"
          << filesystem_error.message() << '\n';
      return 2;
    }
    std::ofstream file{path, std::ios::binary};
    file << boost::json::serialize(review) << '\n';
    file.flush();
    if (!file) {
      error_output << "profile_error: promotion_review_write_failed\n";
      return 2;
    }
    output << "promotion_review=" << path.string()
           << " catalog_mutated=false\n";
    return 0;
  }
  const auto venue =
      options.venues.empty() ? std::string_view{} : options.venues.front();
  const auto product =
      options.products.empty() ? std::string_view{} : options.products.front();
  auto matches = search_research_catalog(
      loaded.catalog,
      options.action == "search" ? options.query : std::string_view{},
      venue,
      product,
      options.cases.empty() ? std::string_view{} : options.cases.front());
  if (options.action == "show" && matches.size() != 1U) {
    error_output << "profile_error: show requires exactly one matching profile\n";
    return 2;
  }
  emit_profile_rows(matches, options.jsonl || options.action == "show", output);
  return matches.empty() ? 1 : 0;
}

int run_discovery(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  auto loaded = load_research_catalog(options.profile_root);
  if (!loaded.ok) {
    error_output << "discovery_error: " << loaded.error << '\n';
    return 2;
  }
  const auto* profile = find_research_profile(
      loaded.catalog, options.venues.front(), options.products.front());
  if (profile == nullptr) {
    error_output << "discovery_error: profile_not_found\n";
    return 2;
  }
  const auto limit = std::min<std::size_t>(
      profile->discovery.size(), discovery_limit(options.placement_mode));
  if (limit == 0U) {
    error_output << "discovery_error: profile_has_no_candidates\n";
    return 2;
  }
  const auto directory =
      options.output_dir.value_or(default_discovery_output(*profile));
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    error_output << "discovery_error: output_directory_create_failed:"
                 << filesystem_error.message() << '\n';
    return 2;
  }
  const auto first_entry =
      std::filesystem::directory_iterator{directory, filesystem_error};
  if (filesystem_error) {
    error_output << "discovery_error: output_directory_inspection_failed:"
                 << filesystem_error.message() << '\n';
    return 2;
  }
  if (first_entry != std::filesystem::directory_iterator{}) {
    error_output << "discovery_error: output_directory_not_empty\n";
    return 2;
  }
  std::ofstream evidence{directory / "evidence.jsonl", std::ios::binary};
  if (!evidence) {
    error_output << "discovery_error: evidence_open_failed\n";
    return 2;
  }
  boost::json::array accepted;
  const auto symbol =
      options.symbol.empty() ? profile->default_symbol : options.symbol;
  for (std::size_t index = 0U; index < limit; ++index) {
    const auto& candidate = profile->discovery[index];
    Observation best;
    bool success = false;
    for (unsigned attempt = 0U;
         attempt < discovery_attempts(options.placement_mode);
         ++attempt) {
      auto limits = options.limits;
      limits.attempts = 1U;
      limits.raw_public = true;
      const ProductSpec product{
          .venue = profile->venue,
          .product = profile->product,
      };
      const auto probe_case = candidate_ws_case(candidate, symbol);
      best = run_public_ws(product, probe_case, limits);
      auto row = observation_json(best);
      row["schema"] = "exchange.api_probe.discovery_evidence.v1";
      row["candidate"] = candidate.id;
      row["attempt"] = attempt + 1U;
      evidence << boost::json::serialize(row) << '\n';
      success = best.expectation_met;
      if (success) break;
      if (attempt + 1U < discovery_attempts(options.placement_mode)) {
        std::this_thread::sleep_for(discovery_pause(options.placement_mode));
      }
    }
    if (success) {
      accepted.emplace_back(boost::json::object{
          {"id", candidate.id},
          {"channel", candidate.channel_id},
          {"host", candidate.host},
          {"port", candidate.port},
          {"path", candidate.path},
          {"subscribe", candidate.subscribe},
          {"subscribe_binary", candidate.subscribe_binary},
          {"provenance", "observed"},
          {"note", candidate.note},
          {"evidence_file", "evidence.jsonl"},
      });
    }
    if (index + 1U < limit) {
      std::this_thread::sleep_for(discovery_pause(options.placement_mode));
    }
  }
  boost::json::object proposal{
      {"schema", "exchange.api_probe.profile_proposal.v1"},
      {"venue", profile->venue},
      {"product", profile->product},
      {"source_profile_schema", kResearchProfileSchema},
      {"promotion", "explicit_review_required"},
      {"accepted_candidates", std::move(accepted)},
  };
  std::ofstream proposal_file{
      directory / "profile-proposal.json", std::ios::binary};
  proposal_file << boost::json::serialize(proposal) << '\n';
  proposal_file.flush();
  evidence.flush();
  if (!proposal_file || !evidence) {
    error_output << "discovery_error: artifact_write_failed\n";
    return 2;
  }
  output << "discovery_bundle=" << directory.string()
         << " candidates=" << limit
         << " accepted="
         << proposal.at("accepted_candidates").as_array().size() << '\n';
  return 0;
}

}  // namespace exchange_probe
