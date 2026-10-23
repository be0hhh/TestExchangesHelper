#include "OfflineCase.hpp"
#include "exchange_probe/App.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace {
exchange_probe::CliParseResult cli(std::initializer_list<const char*> arguments) {
  std::vector<std::string> storage{"exchange-api-probe"};
  for (const auto* argument : arguments) storage.emplace_back(argument);
  std::vector<char*> argv;
  for (auto& argument : storage) argv.push_back(argument.data());
  return exchange_probe::parse_cli(static_cast<int>(argv.size()), argv.data());
}

void pairedAnalysisIsExplicit() {
  const auto ordinary = cli({"research", "analyze", "--input", "bundle"});
  CXET_CHECK(ordinary.ok && !ordinary.options.standard_channel && !ordinary.options.aws_channel);
  const auto paired = cli({"research", "analyze", "--input", "bundle", "--standard-channel", "trades", "--aws-channel", "trades_aws"});
  CXET_CHECK(paired.ok && *paired.options.standard_channel == "trades" && *paired.options.aws_channel == "trades_aws");
}

void pairedAnalysisRefusesAmbiguousSelection() {
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--standard-channel", "trades"}).ok);
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--aws-channel", "trades_aws"}).ok);
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--standard-channel", "trades", "--aws-channel", "trades"}).ok);
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--standard-channel", "", "--aws-channel", "trades_aws"}).ok);
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--standard-channel", "trades", "--standard-channel", "other", "--aws-channel", "trades_aws"}).ok);
  CXET_CHECK(!cli({"research", "run", "--venue", "htx", "--product", "spot", "--standard-channel", "trades", "--aws-channel", "trades_aws"}).ok);
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--input", "other", "--standard-channel", "trades", "--aws-channel", "trades_aws"}).ok);
  CXET_CHECK(!cli({"research", "analyze", "--input", "bundle", "--surface", "private", "--standard-channel", "trades", "--aws-channel", "trades_aws"}).ok);
}
}  // namespace

int main(int argc, char** argv) {
  const cxet::testing::Case cases[]{
      cxet::testing::Case{"helper.paired_receipt_cli_is_explicit", pairedAnalysisIsExplicit},
      cxet::testing::Case{"helper.paired_receipt_cli_refuses_ambiguous_selection", pairedAnalysisRefusesAmbiguousSelection},
  };
  return cxet::testing::runCases(argc, argv, cases);
}
