#include "OfflineCase.hpp"
#include "exchange_probe/App.hpp"
#include "exchange_probe/TbankCredential.hpp"
#include "exchange_probe/TbankGrpcDiagnostic.hpp"
#include "../../../src/Network/TbankConnectionControl.hpp"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
exchange_probe::CliParseResult cli(std::initializer_list<const char*> arguments) {
  std::vector<std::string> storage{"exchange-api-probe"};
  for (const auto* argument : arguments) storage.emplace_back(argument);
  std::vector<char*> argv;
  for (auto& argument : storage) argv.push_back(argument.data());
  return exchange_probe::parse_cli(static_cast<int>(argv.size()), argv.data());
}

void cliSelection() {
  const auto valid = cli({"research", "tbank-goaway", "--engines", "cxet,nghttp2", "--env-file", "absent", "--ca-file", "ca.pem", "--api-slot", "1", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out"});
  CXET_CHECK(valid.ok && valid.options.api_slot == 1 && valid.options.limits.attempts == 1);
  CXET_CHECK(valid.options.ca_file && *valid.options.ca_file == "ca.pem");
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "12345678-1234-1234-1234-123456789abc", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--engines", "other", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--attempts", "2"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--ca-file", "", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--ca-file", "a", "--ca-file", "b", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--ca-file", "--env-file", "absent"}).ok);
  CXET_CHECK(!cli({"research", "run", "--ca-file", "ca.pem"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "--api-slot", "1"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--pcap"}).ok);
  CXET_CHECK(!cli({"research", "run", "--engines", "cxet,nghttp2"}).ok);
  using namespace exchange_probe::tbank_grpc;
  bool rejected = false;
  auto connection = make_nghttp2_connection();
  // A future deadline isolates the missing CA; invalid IP also prevents a
  // socket attempt if the CA guard is accidentally removed.
  const ConnectOptions no_trust{.host = "invest-public-api.tbank.ru", .pinned_ip = "not-an-ip",
                                .deadline_ns = std::numeric_limits<std::uint64_t>::max()};
  CXET_CHECK(!connection->connect(no_trust, [&](const Event& event) {
    rejected = event.kind == EventKind::Failure && event.stage == "configuration";
  }, {}));
  CXET_CHECK(rejected);
}

void credentialSelection() {
  const auto path = std::filesystem::temp_directory_path() /
      ("cxet-tbank-credential-synthetic-" + std::to_string(getpid()));
  auto write = [&](const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
  };
  std::string error;
  write("# synthetic\nOTHER_KEY=ignored\nexport TBANK_API_1_API='synthetic-token'\n");
  auto token = exchange_probe::load_tbank_credential(path, 1, error);
  CXET_CHECK(token && token->token() == "synthetic-token" && error.empty());
  write("TBANK_API_1_API=first\nTBANK_API_1_API=second\n");
  CXET_CHECK(!exchange_probe::load_tbank_credential(path, 1, error));
  write("TBANK_API_1_API='unterminated\n");
  CXET_CHECK(!exchange_probe::load_tbank_credential(path, 1, error));
  write("TBANK_API_1_API=has space\n");
  CXET_CHECK(!exchange_probe::load_tbank_credential(path, 1, error));
  write("TBANK_API_2_API=other\n");
  CXET_CHECK(!exchange_probe::load_tbank_credential(path, 1, error));
  std::filesystem::remove(path);
}
void lifetimeSelection() {
  {
    using exchange_probe::tbank_grpc::detail::service_other_unless_stopped;
    bool stopped = false; unsigned services = 0, network_steps = 0;
    auto service = [&] { ++services; stopped = true; };
    auto stop = [&] { return stopped; };
    if (service_other_unless_stopped(service, stop)) ++network_steps;
    CXET_CHECK(stopped && services == 1 && network_steps == 0);
    if (service_other_unless_stopped(service, stop)) ++network_steps;
    CXET_CHECK(services == 1 && network_steps == 0);
    stopped = false;
    if (service_other_unless_stopped({}, {})) ++network_steps;
    CXET_CHECK(network_steps == 1); // Existing mode has no stop callback.
  }
  const auto valid = cli({"research", "tbank-lifetime", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--duration-seconds", "600"});
  CXET_CHECK(valid.ok && *valid.options.duration_seconds == 600 && !valid.options.open_viewer);
  CXET_CHECK(cli({"research", "tbank-lifetime", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--duration-seconds", "28800"}).ok);
  CXET_CHECK(!cli({"research", "tbank-lifetime", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--duration-seconds", "601"}).ok);
  CXET_CHECK(!cli({"research", "tbank-lifetime", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--duration-seconds", "600", "--attempts", "2"}).ok);
  CXET_CHECK(!cli({"research", "tbank-lifetime", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--duration-seconds", "600", "--timeout-ms", "5001"}).ok);
  CXET_CHECK(cli({"research", "tbank-goaway", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--timeout-ms", "10000"}).ok);
  CXET_CHECK(!cli({"research", "tbank-goaway", "--env-file", "absent", "--ca-file", "ca.pem", "--symbol", "e6123145-9665-43e0-8413-cd61b8aa9b13", "--output-dir", "out", "--duration-seconds", "600"}).ok);
}
void pingHandoffSelection() {
  {
    using exchange_probe::tbank_grpc::detail::record_http2_frame;
    using exchange_probe::tbank_grpc::detail::record_tls_read;
    std::uint64_t legacy_last = 100;
    record_tls_read(false, 105, legacy_last); // Positive SSL_read with an incomplete H2 frame.
    CXET_CHECK(legacy_last == 105 && legacy_last + 10 == 115);
    record_tls_read(false, 110, legacy_last); // Incoming unmatched ACK is still received bytes.
    record_http2_frame(false, true, false, 111, legacy_last);
    CXET_CHECK(legacy_last == 110 && legacy_last + 10 == 120);
    record_tls_read(false, 112, legacy_last);
    record_http2_frame(false, true, true, 113, legacy_last);
    CXET_CHECK(legacy_last == 113 && legacy_last + 10 == 123);
    std::uint64_t handoff_last = 100;
    record_tls_read(true, 105, handoff_last);
    record_http2_frame(true, true, false, 110, handoff_last);
    CXET_CHECK(handoff_last == 100);
    record_http2_frame(true, true, true, 111, handoff_last);
    CXET_CHECK(handoff_last == 111);
  }
  CXET_CHECK(cli({"research", "tbank-ping-handoff", "--ca-file", "ca.pem", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-ping-handoff", "--ca-file", "ca.pem", "--output-dir", "out", "--env-file", "token"}).ok);
  CXET_CHECK(!cli({"research", "tbank-ping-handoff", "--ca-file", "ca.pem", "--output-dir", "out", "--duration-seconds", "600"}).ok);
  CXET_CHECK(!cli({"research", "tbank-ping-handoff", "--ca-file", "ca.pem", "--output-dir", "out", "--timeout-ms", "5001"}).ok);
  CXET_CHECK(!cli({"research", "tbank-ping-handoff", "--ca-file", "ca.pem", "--ca-file", "second", "--output-dir", "out"}).ok);
  CXET_CHECK(!cli({"research", "tbank-ping-handoff", "--output-dir", "out"}).ok);
}
}  // namespace

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      cxet::testing::Case{"helper.tbank_goaway_cli_selection", cliSelection},
      cxet::testing::Case{"helper.tbank_credential_selection", credentialSelection},
      cxet::testing::Case{"helper.tbank_lifetime_cli_selection", lifetimeSelection},
      cxet::testing::Case{"helper.tbank_ping_handoff_cli_selection", pingHandoffSelection},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
