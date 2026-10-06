#include "OfflineCase.hpp"
#include "exchange_probe/App.hpp"
#include "exchange_probe/Net.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>
#include <string>

namespace {
using namespace exchange_probe;

ProductSpec publicPair() {
  ProductSpec product{};
  product.venue = "htx";
  product.product = "spot";
  RestCase standard{};
  standard.name = "public_trades";
  standard.host = "api.huobi.pro";
  standard.path = "/market/history/trade?symbol=btcusdt&size=10";
  standard.capabilities = {"historical_trades"};
  auto aws = standard;
  aws.name += "_aws";
  aws.host = "api-aws.huobi.pro";
  product.public_rest = {standard, aws};
  return product;
}

void refused(ProductSpec product, CliOptions options,
             const char* expected_error) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("rest-admission-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::ostringstream output, errors;
  CXET_CHECK(capture_paired_rest_research(
      options, product, directory, output, errors) == 2);
  CXET_CHECK(errors.str().find(expected_error) != std::string::npos);
  CXET_CHECK(output.str().empty() && !std::filesystem::exists(directory));
}

CliOptions pairOptions() {
  CliOptions options{};
  options.cases = {"public_trades", "public_trades_aws"};
  return options;
}

void privateAdmission() {
  auto product = publicPair();
  product.public_rest[1].private_case = true;
  refused(product, pairOptions(), "public_only");
  product.public_rest[1].private_case = false;
  product.public_rest[1].auth = AuthKind::GateHmac;
  refused(product, pairOptions(), "public_only");
  auto options = pairOptions();
  options.surface = Surface::Private;
  refused(publicPair(), options, "public_only");
}

void mismatchedAdmission() {
  auto product = publicPair();
  product.public_rest[1].path += "&size=20";
  refused(product, pairOptions(), "case_semantics_mismatch");
  product = publicPair();
  product.public_rest[1].capabilities = {"rest_orderbook"};
  refused(product, pairOptions(), "case_semantics_mismatch");
  auto options = pairOptions();
  options.cases.pop_back();
  refused(publicPair(), options, "exactly_two_cases");
  options.cases = {"public_trades", "public_trades"};
  refused(publicPair(), options, "distinct_cases");
}

void hostAdmission() {
  auto product = publicPair();
  product.public_rest[1].host = "unverified.example";
  refused(product, pairOptions(), "documented_htx_host_pair_required");
  product = publicPair();
  product.venue = "gate";
  refused(product, pairOptions(), "htx_only");
}

void loadAdmission() {
  auto options = pairOptions();
  options.rounds = 4;
  refused(publicPair(), options, "invalid_limits");
  options = pairOptions();
  options.duration_seconds = 301;
  refused(publicPair(), options, "invalid_limits");
  options.confirm_load = true;
  refused(publicPair(), options, "invalid_limits");
  options = pairOptions();
  options.rounds = 0;
  refused(publicPair(), options, "invalid_limits");
}

void publicConnectionAdmission() {
  auto request = publicPair().public_rest.front();
  request.private_case = true;
  PublicHttpConnection private_connection{request};
  const auto private_result = private_connection.get(
      std::chrono::steady_clock::now());
  CXET_CHECK(private_result.stage == "configuration");
  CXET_CHECK(private_result.error == "public_connection_requires_public_unsigned_case");
  CXET_CHECK(private_result.dns_answers.empty() && !private_result.dns_ns);
  CXET_CHECK(!private_result.connection_available && !private_result.connection_reused);
  request.private_case = false;
  request.auth = AuthKind::GateHmac;
  PublicHttpConnection signed_connection{request};
  CXET_CHECK(signed_connection.get(std::chrono::steady_clock::now()).error ==
      "public_connection_requires_public_unsigned_case");
}
void expiredDeadline() {
  PublicHttpConnection connection{publicPair().public_rest.front()};
  const auto result = connection.get(std::chrono::steady_clock::now());
  CXET_CHECK(result.error == "http_deadline_expired" && result.stage == "configuration");
  CXET_CHECK(result.dns_answers.empty() && !result.dns_ns && !result.request_response_ns);
  CXET_CHECK(!result.connection_reused && !result.connection_available && result.raw_body.empty());
}
void invalidAuthority() {
  for (const auto* host : {"", "api.huobi.pro\r\nInjected: value"}) {
    auto request = publicPair().public_rest.front();
    request.host = host;
    PublicHttpConnection connection{request};
    const auto result = connection.get(std::chrono::steady_clock::now() + std::chrono::seconds{1});
    CXET_CHECK(result.error == "public_connection_authority_or_path_invalid");
    CXET_CHECK(result.dns_answers.empty() && !result.dns_ns);
  }
  auto request = publicPair().public_rest.front();
  request.path = "/path\r\nInjected: value";
  PublicHttpConnection connection{request};
  CXET_CHECK(connection.get(std::chrono::steady_clock::now() + std::chrono::seconds{1}).error ==
             "public_connection_authority_or_path_invalid");
}
}  // namespace

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      cxet::testing::Case{"rest.paired_refuses_private_before_artifacts_or_dns", privateAdmission},
      cxet::testing::Case{"rest.paired_refuses_mismatched_cases_before_dns", mismatchedAdmission},
      cxet::testing::Case{"rest.paired_requires_documented_htx_host_pair", hostAdmission},
      cxet::testing::Case{"rest.paired_refuses_excess_load_even_with_confirmation", loadAdmission},
      cxet::testing::Case{"rest.public_connection_refuses_private_or_signed_cases", publicConnectionAdmission},
      cxet::testing::Case{"rest.public_connection_expired_deadline_does_not_start_dns", expiredDeadline},
      cxet::testing::Case{"rest.public_connection_refuses_invalid_authority_before_dns", invalidAuthority},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
