#include "OfflineCase.hpp"
#include "exchange_probe/Contracts.hpp"
#include "exchange_probe/App.hpp"
#include "exchange_probe/Research.hpp"
#include <boost/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace exchange_probe;
void restSymbol() {
  const auto row=boost::json::parse(R"({"symbol":"AAA","quoteVolume":"42","priceChangePercent":"1"})");
  const auto good=validate_rest_contract(RestContract::BinanceTicker24h,row,"AAA");
  const auto bad=validate_rest_contract(RestContract::BinanceTicker24h,row,"BBB");
  CXET_CHECK(good.logical_success && good.schema_success && good.symbol_success);
  CXET_CHECK(!bad.symbol_success);
}
void htxPublicEnvelope() {
  // Official public REST envelope shapes; payload values are synthetic.
  // https://huobiapi.github.io/docs/spot/v1/en/
  // https://huobiapi.github.io/docs/usdt_swap/v1/en/
  // This contract proves envelope shape only, including an empty data array;
  // it deliberately does not validate payload numeric fields or symbol identity.
  for (const auto* text : {
      R"({"status":"ok","data":[{"id":7,"price":"synthetic"}]})",
      R"({"status":"ok","data":[]})",
      R"({"status":"ok","data":{"value":"synthetic"}})",
      R"({"status":"ok","tick":{"bids":[["100","2"]],"asks":[["102","3"]]}})"}) {
    const auto evidence = validate_rest_contract(
        RestContract::HtxPublicEnvelope, boost::json::parse(text), "");
    CXET_CHECK(evidence.logical_success && evidence.schema_success && evidence.symbol_success);
    CXET_CHECK(evidence.contract == "htx_public_envelope" && evidence.native_symbol.empty());
    CXET_CHECK(evidence.units == "envelope_only;numeric_units_not_validated");
  }
  const auto logical_error = validate_rest_contract(RestContract::HtxPublicEnvelope,
      boost::json::parse(R"({"status":"error","err-code":"synthetic-error","data":[]})"), "");
  CXET_CHECK(!logical_error.logical_success && !logical_error.schema_success);
  CXET_CHECK(logical_error.api_code == "synthetic-error");
  for (const auto* text : {R"([])", R"({"data":[]})", R"({"status":1,"data":[]})",
      R"({"status":"ok"})", R"({"status":"ok","data":null})",
      R"({"status":"ok","data":"invalid"})", R"({"status":"ok","tick":[]})"}) {
    const auto evidence = validate_rest_contract(
        RestContract::HtxPublicEnvelope, boost::json::parse(text), "");
    CXET_CHECK(!evidence.schema_success && !evidence.error.empty());
  }
  const auto symbol = validate_rest_contract(RestContract::HtxPublicEnvelope,
      boost::json::parse(R"({"status":"ok","data":[{"symbol":"AAA"}]})"), "AAA");
  CXET_CHECK(!symbol.symbol_success && !symbol.schema_success);
  CXET_CHECK(symbol.error == "htx_envelope_symbol_proof_unavailable");
  for (const auto& profile : make_profiles()) {
    if (profile.venue != "htx") continue;
    for (const auto& request : profile.public_rest) {
      CXET_CHECK(request.contract == RestContract::HtxPublicEnvelope && !request.native);
      CXET_CHECK(request.expected_symbol.empty());
    }
    for (const auto& capability : profile.capabilities) {
      if (capability.transport == Transport::Rest && capability.surface == Surface::Public)
        CXET_CHECK(!capability.native);
    }
  }
}
void ackIdentity() {
  WsCase c{};c.ack_kind=WsAckKind::BinanceSubscription;c.expected_request_id="7";
  CXET_CHECK(validate_ws_ack(c,boost::json::parse(R"({"result":null,"id":7})"),true,false).matched);
  CXET_CHECK(!validate_ws_ack(c,boost::json::parse(R"({"result":null,"id":8})"),true,false).matched);
  CXET_CHECK(!validate_ws_ack(c,boost::json::parse(R"({"result":null,"id":7})"),true,true).matched);
}
void sbeHeader() {
  WsCase c{};c.data_kind=WsDataKind::SbeHeader;c.expected_sbe_schema=3;c.expected_sbe_templates={2};
  std::string frame{char(1),char(0),char(2),char(0),char(3),char(0),char(1),char(0)};
  CXET_CHECK(validate_ws_data(c,{},false,true,frame).matched);
  frame[4]=4;CXET_CHECK(!validate_ws_data(c,{},false,true,frame).matched);
  frame.resize(7);CXET_CHECK(!validate_ws_data(c,{},false,true,frame).matched);
}
void protobufEnvelope() {
  WsCase c{};c.data_kind=WsDataKind::ProtobufEnvelope;
  const std::string valid{char(0x0a),char(1),'A'};
  const std::string truncated{char(0x0a),char(2),'A'};
  CXET_CHECK(validate_ws_data(c,{},false,true,valid).matched);
  CXET_CHECK(!validate_ws_data(c,{},false,true,truncated).matched);
  CXET_CHECK(!validate_ws_data(c,{},false,false,valid).matched);
}
void redaction() {
  const auto shaped=shape_of(boost::json::parse(R"({"nested":{"api_key":"synthetic-key","AccessToken":"synthetic-token","value":"ordinary"},"Authorization":"synthetic-auth"})"));
  const auto& object=shaped.as_object();const auto& nested=object.at("nested").as_object();
  CXET_CHECK(object.at("Authorization").as_string()=="<redacted>");
  CXET_CHECK(nested.at("api_key").as_string()=="<redacted>" && nested.at("AccessToken").as_string()=="<redacted>");
  CXET_CHECK(nested.at("value").as_string()=="string");
  CXET_CHECK(boost::json::serialize(shaped).find("synthetic-")==std::string::npos);
}
void frameBound() {
  const std::string exact(kMaxRawPublicBytes,'a');CXET_CHECK(bounded_public_frame(exact)==exact);
  const auto bounded=bounded_public_frame(exact+"tail");
  CXET_CHECK(bounded.size()==kMaxRawPublicBytes && bounded.ends_with("...<truncated>"));
  CXET_CHECK(bounded.find("tail")==std::string::npos);
}
CliParseResult cli(std::initializer_list<const char*> arguments) {
  std::vector<std::string> values{"exchange-api-probe"};
  for (const auto* argument : arguments) values.emplace_back(argument);
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  return parse_cli(static_cast<int>(argv.size()), argv.data());
}
void retiredPerformanceCli() {
  for (const auto* command : {"latency", "stability", "compare", "run"}) {
    const auto result = cli({command});
    CXET_CHECK(!result.ok && result.error.find("removed") != std::string::npos);
  }
  for (const auto* option : {"--samples", "--lanes", "--connection"}) {
    const auto result = cli({"placement", option, "1"});
    CXET_CHECK(!result.ok && !result.error.empty());
  }
  CXET_CHECK(!cli({"placement", "--duration-seconds", "1"}).ok);
}
void retainedCatalogCli() {
  CXET_CHECK(cli({"matrix", "--venue", "binance", "--jsonl"}).ok);
  CXET_CHECK(cli({"profile", "list"}).ok);
  CXET_CHECK(cli({"discover", "--venue", "binance", "--product", "spot"}).ok);
  CXET_CHECK(cli({"placement", "--route", "pinned"}).ok);
  CXET_CHECK(!cli({"placement", "--path", "proxy"}).ok);
  CXET_CHECK(!cli({"placement", "--path", "direct"}).ok);
  CXET_CHECK(cli({"research", "run", "--venue", "binance", "--product", "spot", "--duration-seconds", "1"}).ok);
  CXET_CHECK(!cli({"research", "run", "--venue", "binance", "--product", "spot", "--surface", "private"}).ok);
  std::ostringstream output;
  print_help(output);
  CXET_CHECK(output.str().find("exchange-api-probe latency") == std::string::npos);
  CXET_CHECK(output.str().find("exchange-api-probe stability") == std::string::npos);
  CXET_CHECK(output.str().find("exchange-api-probe compare") == std::string::npos);
  CliOptions options; options.command = Command::Matrix;
  options.venues = {"binance"}; options.products = {"spot"}; options.jsonl = true;
  std::ostringstream errors;
  CXET_CHECK(run_application(options, output, errors) == 0 && errors.str().empty());
  CXET_CHECK(output.str().find("\"kind\":\"capability\"") != std::string::npos);
}
void observationCapabilityEvidence() {
  Observation observation{};
  observation.kind = "observation"; observation.outcome = Outcome::Success;
  observation.expectation_met = true; observation.transport_ok = true;
  observation.transport_metadata.remote_ip = "127.0.0.1";
  observation.transport_metadata.tls_version = "TLSv1.3";
  observation.stage = "complete"; observation.evidence = boost::json::object{{"symbol", "AAA"}};
  const auto result = observation_json(observation);
  CXET_CHECK(result.at("schema_version") == 4);
  CXET_CHECK(result.at("expectation_met").as_bool() && result.at("transport_ok").as_bool());
  CXET_CHECK(result.at("evidence").as_object().at("symbol") == "AAA");
  CXET_CHECK(!result.contains("elapsed_ms") && !result.contains("stage_timings"));
  const auto& transport = result.at("transport_metadata").as_object();
  CXET_CHECK(transport.at("remote_ip") == "127.0.0.1" && transport.at("tls_version") == "TLSv1.3");
  CXET_CHECK(!transport.contains("tcp_rtt_us") && !transport.contains("tcp_info_available"));
  std::ostringstream output;
  emit_observations({observation}, false, output);
  CXET_CHECK(output.str().find("TIME_MS") == std::string::npos);
  CXET_CHECK(output.str().find("complete") != std::string::npos);
}
struct ResearchFixture {
  std::filesystem::path directory;
  ResearchFixture() {
    directory = std::filesystem::temp_directory_path() /
        ("exchange-probe-semantic-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    CXET_CHECK(std::filesystem::create_directory(directory));
  }
  ~ResearchFixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
  void write(const char* name, const std::string& value) {
    std::ofstream output{directory / name, std::ios::binary}; output << value;
    CXET_CHECK(output.good());
  }
  boost::json::value read(const char* name) {
    std::ifstream input{directory / name, std::ios::binary};
    CXET_CHECK(input.good());
    return boost::json::parse(std::string{std::istreambuf_iterator<char>{input}, {}});
  }
};
void researchSemanticOutput() {
  ResearchFixture fixture;
  fixture.write("manifest.json", R"({"schema":"exchange.api_probe.bundle.v3","schema_version":3,"channels":[{"id":"a","kind":"depth","mapping":{"event_id":"id","bids":"bids","asks":"asks","snapshot":"snapshot","snapshot_value":"true","event_time":"E","event_time_unit":"ns"}},{"id":"b","kind":"depth","mapping":{"event_id":"id","bids":"bids","asks":"asks","snapshot":"snapshot","snapshot_value":"true","event_time":"E","event_time_unit":"ns"}}]})");
  const std::string payload = R"({"id":"7","snapshot":true,"E":123,"bids":[["100","2"]],"asks":[["102","3"]]})";
  fixture.write("frames.bin", payload + payload);
  std::string index;
  for (unsigned n = 0; n != 2; ++n) {
    index += boost::json::serialize(boost::json::object{
        {"schema", "exchange.api_probe.frame_index.v1"}, {"frame_id", n}, {"round", 1},
        {"channel_id", n == 0 ? "a" : "b"}, {"opcode", "text"},
        {"monotonic_ns", 100 + n * 100}, {"utc_ns", 1000 + n * 100},
        {"offset", n * payload.size()}, {"length", payload.size()}}) + "\n";
  }
  fixture.write("frames.jsonl", index);
  std::ostringstream output, errors;
  CXET_CHECK(analyze_research_bundle(fixture.directory, output, errors) == 0 && errors.str().empty());
  const auto findings = fixture.read("findings.json").as_object();
  CXET_CHECK(findings.at("schema") == "exchange.api_probe.findings.v2");
  CXET_CHECK(findings.at("events") == 2 && findings.at("valid_bbo_transitions") == 2);
  CXET_CHECK(findings.at("full_bbo_reconstruction").as_object().at("status") == "observed");
  CXET_CHECK(findings.at("anomalies").as_object().at("exact_frame_duplicates") == 1);
  CXET_CHECK(!findings.contains("arrival_intervals_by_channel") && !findings.contains("absolute_receive_lag_by_relation"));
  CXET_CHECK(!findings.contains("first_usable_bbo_by_channel"));
  CXET_CHECK(!findings.at("full_bbo_reconstruction").as_object().contains("durations_by_channel"));
  std::ifstream relations{fixture.directory / "relations.jsonl"};
  std::string row; CXET_CHECK(static_cast<bool>(std::getline(relations, row)));
  const auto relation = boost::json::parse(row).as_object();
  CXET_CHECK(relation.at("schema") == "exchange.api_probe.relation.v2");
  CXET_CHECK(relation.at("source_event_id") == 0 && relation.at("target_event_id") == 1);
  CXET_CHECK(relation.at("causality") == "not_asserted" && !relation.contains("receive_lag_ns"));
  std::ifstream events{fixture.directory / "events.jsonl"};
  CXET_CHECK(static_cast<bool>(std::getline(events, row)));
  const auto event = boost::json::parse(row).as_object();
  CXET_CHECK(event.at("monotonic_ns") == 100 && event.at("utc_ns") == 1000 && event.at("exchange_event_ns") == 123);
}
void htxResearchEndpointPairs() {
  const auto loaded = load_research_catalog(
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "profiles");
  CXET_CHECK(loaded.ok);
  for (const auto* product : {"spot", "futures"}) {
    const auto* profile = find_research_profile(loaded.catalog, "htx", product);
    CXET_CHECK(profile != nullptr);
    const bool spot = profile->product == "spot";
    for (const auto& channel : profile->channels) {
      if (channel.id.ends_with("_aws")) continue;
      const auto* alternative = find_research_channel(*profile, channel.id + "_aws");
      CXET_CHECK(alternative != nullptr);
      CXET_CHECK(channel.host == (spot ? "api.huobi.pro" : "api.hbdm.com"));
      CXET_CHECK(alternative->host == (spot ? "api-aws.huobi.pro" : "api.hbdm.vn"));
      CXET_CHECK(channel.path == (spot ? "/ws" : "/linear-swap-ws"));
      CXET_CHECK(alternative->path == channel.path && alternative->subscribe == channel.subscribe);
      CXET_CHECK(channel.compression == "gzip" && alternative->compression == channel.compression);
      CXET_CHECK(alternative->wire == channel.wire && alternative->kind == channel.kind);
      CXET_CHECK(alternative->support == channel.support && alternative->capabilities == channel.capabilities);
      CXET_CHECK(!alternative->subscribe_binary && alternative->port == channel.port);
      CXET_CHECK(alternative->mapping.event_id_path == channel.mapping.event_id_path);
      CXET_CHECK(alternative->mapping.trades_path == channel.mapping.trades_path);
      if (channel.kind == ResearchChannelKind::Trade) {
        // Official Trade Detail examples (reviewed 2026-10-05): spot replaces
        // obsolete id with tradeId; linear swaps use symbol-local native id.
        // https://huobiapi.github.io/docs/spot/v1/en/#trade-detail
        // https://huobiapi.github.io/docs/usdt_swap/v1/en/#general-subscribe-trade-detail-data
        CXET_CHECK(channel.mapping.data_path == "tick.data");
        CXET_CHECK(channel.mapping.event_id_path == (spot ? "tradeId" : "id"));
        CXET_CHECK(channel.mapping.event_time_path == "ts" && channel.mapping.event_time_unit == "ms");
        CXET_CHECK(channel.mapping.price_path == "price" && channel.mapping.quantity_path == "amount");
        CXET_CHECK(channel.mapping.side_path == "direction" && channel.mapping.symbol_path.empty());
        CXET_CHECK(channel.support == ResearchSupport::ObservedOnly);
      }
    }
  }
}
void htxDiagnosticEndpointPairs() {
  const auto profiles = make_profiles();
  for (const auto& profile : profiles) {
    if (profile.venue != "htx") continue;
    const bool spot = profile.product == "spot";
    for (const auto& channel : profile.public_ws) {
      if (channel.name.ends_with("_aws") || (spot && channel.path != "/ws")) continue;
      const WsCase* alternative = nullptr;
      for (const auto& candidate : profile.public_ws)
        if (candidate.name == channel.name + "_aws") alternative = &candidate;
      CXET_CHECK(alternative != nullptr);
      CXET_CHECK(alternative->host == (spot ? "api-aws.huobi.pro" : "api.hbdm.vn"));
      CXET_CHECK(alternative->path == channel.path && alternative->subscribe == channel.subscribe);
      CXET_CHECK(alternative->expected_symbol == channel.expected_symbol && alternative->expected_topic == channel.expected_topic);
      CXET_CHECK(alternative->compression == Compression::Gzip && alternative->application_heartbeat == "htx");
      CXET_CHECK(alternative->wire == channel.wire && alternative->data_kind == channel.data_kind);
      CXET_CHECK(alternative->selection == Selection::DiagnosticVariant);
    }
    for (const auto& request : profile.public_rest) {
      if (request.name.ends_with("_aws")) continue;
      const RestCase* alternative = nullptr;
      for (const auto& candidate : profile.public_rest)
        if (candidate.name == request.name + "_aws") alternative = &candidate;
      CXET_CHECK(alternative != nullptr);
      CXET_CHECK(alternative->host == (spot ? "api-aws.huobi.pro" : "api.hbdm.vn"));
      CXET_CHECK(alternative->path == request.path && alternative->capabilities == request.capabilities);
      CXET_CHECK(!alternative->private_case && alternative->auth == AuthKind::None);
    }
  }
}
void gateSbeCatalogAndFuturesRest() {
  const auto loaded = load_research_catalog(
      std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path() / "profiles");
  CXET_CHECK(loaded.ok);
  for (const auto* product : {"spot", "futures"}) {
    const auto* profile = find_research_profile(loaded.catalog, "gate", product);
    CXET_CHECK(profile != nullptr);
    const bool spot = profile->product == "spot";
    for (const auto* id : {"trades_sbe", "book_ticker_sbe", "orderbook_sbe"}) {
      const auto* channel = find_research_channel(*profile, id);
      CXET_CHECK(channel != nullptr);
      CXET_CHECK(channel->support == ResearchSupport::AdapterRequired && channel->wire == "sbe");
      CXET_CHECK(channel->host == (spot ? "api.gateio.ws" : "fx-ws.gateio.ws"));
      CXET_CHECK(channel->path == (spot ? "/ws/v4/ws/spot/sbe?sbe_schema_id=2" : "/v4/ws/usdt/sbe?sbe_schema_id=1"));
      CXET_CHECK(!channel->subscribe_binary && !channel->subscribe.empty());
      const auto control = boost::json::parse(channel->subscribe).as_object();
      CXET_CHECK(control.at("event") == "subscribe");
      const std::string topic = std::string{spot ? "spot." : "futures."} +
          (std::string_view{id} == "trades_sbe" ? "trades" :
           std::string_view{id} == "book_ticker_sbe" ? "book_ticker" : "obu");
      CXET_CHECK(control.at("channel") == topic);
      CXET_CHECK(control.at("payload").as_array().size() == 1);
      CXET_CHECK(control.at("payload").as_array().front() ==
          (std::string_view{id} == "orderbook_sbe" ? "ob.{symbol}.50" : "{symbol}"));
    }
  }
  const auto profiles = make_profiles();
  for (const auto& profile : profiles) {
    if (profile.venue != "gate") continue;
    bool alternative_rest = false, native_sbe = false;
    for (const auto& request : profile.public_rest) {
      if (request.host != "fx-api.gateio.ws") continue;
      alternative_rest = true;
      CXET_CHECK(profile.product == "futures" && request.path.starts_with("/api/v4/futures/usdt/"));
      CXET_CHECK(request.selection == Selection::DiagnosticVariant && !request.private_case);
      const RestCase* ordinary = nullptr;
      for (const auto& candidate : profile.public_rest)
        if (request.name == candidate.name + "_fx") ordinary = &candidate;
      CXET_CHECK(ordinary != nullptr && ordinary->host == "api.gateio.ws");
      CXET_CHECK(ordinary->path == request.path && ordinary->capabilities == request.capabilities);
      CXET_CHECK(ordinary->contract == request.contract && ordinary->expectation == request.expectation);
    }
    for (const auto& channel : profile.public_ws) {
      if (channel.wire != Wire::Sbe) continue;
      native_sbe = true;
      CXET_CHECK(profile.product == "futures");
      CXET_CHECK(!channel.subscribe_binary && channel.inbound_binary);
      CXET_CHECK(channel.path == "/v4/ws/usdt/sbe?sbe_schema_id=1");
      CXET_CHECK(channel.expected_sbe_schema == 1);
    }
    CXET_CHECK(native_sbe == (profile.product == "futures"));
    CXET_CHECK(alternative_rest == (profile.product == "futures"));
  }
}
}
int main(int argc,char** argv) {
  const cxet::testing::Case cases[]{
    cxet::testing::Case{"helper.rest_contract_requires_exact_symbol",restSymbol},
    cxet::testing::Case{"helper.htx_public_rest_envelope_refuses_errors_and_symbol_claims",htxPublicEnvelope},
    cxet::testing::Case{"helper.ws_ack_requires_request_identity_and_text_wire",ackIdentity},
    cxet::testing::Case{"helper.sbe_header_refuses_wrong_schema_and_truncation",sbeHeader},
    cxet::testing::Case{"helper.protobuf_envelope_refuses_truncated_length",protobufEnvelope},
    cxet::testing::Case{"helper.shape_recursively_redacts_sensitive_fields",redaction},
    cxet::testing::Case{"helper.public_frame_bound_preserves_exact_limit",frameBound},
    cxet::testing::Case{"helper.retired_performance_cli_is_rejected",retiredPerformanceCli},
    cxet::testing::Case{"helper.retained_catalog_cli_needs_no_performance_modes",retainedCatalogCli},
    cxet::testing::Case{"helper.observation_retains_capability_evidence_without_timings",observationCapabilityEvidence},
    cxet::testing::Case{"helper.research_retains_semantics_without_timing_reports",researchSemanticOutput},
    cxet::testing::Case{"helper.htx_research_endpoint_pairs_preserve_product_grammar",htxResearchEndpointPairs},
    cxet::testing::Case{"helper.htx_diagnostic_endpoint_pairs_preserve_public_contract",htxDiagnosticEndpointPairs},
    cxet::testing::Case{"helper.gate_sbe_catalog_uses_text_controls_and_product_rest",gateSbeCatalogAndFuturesRest},
  };return cxet::testing::runCases(argc,argv,cases);
}
