#pragma once

#include "exchange_probe/credentials.hpp"
#include "exchange_probe/model.hpp"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace exchange_probe {

enum class Command {
  Help,
  Matrix,
  Audit,
  Run,
  Sandbox,
};

struct CliOptions {
  Command command{Command::Help};
  std::vector<std::string> venues;
  std::vector<std::string> products;
  std::vector<std::string> cases;
  std::optional<Surface> surface;
  std::optional<Transport> transport;
  std::optional<std::filesystem::path> source_root;
  std::optional<std::filesystem::path> sandbox_file;
  std::optional<std::filesystem::path> env_file;
  RunLimits limits;
  bool jsonl{false};
  bool confirm_private{false};
};

struct CliParseResult {
  CliOptions options;
  bool ok{false};
  std::string error;
};

[[nodiscard]] CliParseResult parse_cli(int argc, char** argv);
void print_help(std::ostream& output);

[[nodiscard]] std::vector<const ProductSpec*> select_products(
    const std::vector<ProductSpec>& products,
    const CliOptions& options);

[[nodiscard]] Observation run_public_rest(
    const ProductSpec& product,
    const RestCase& probe_case,
    const RunLimits& limits);

[[nodiscard]] Observation run_private_rest(
    const ProductSpec& product,
    const RestCase& probe_case,
    const Credentials& credentials,
    const RunLimits& limits);

[[nodiscard]] Observation run_public_ws(
    const ProductSpec& product,
    const WsCase& probe_case,
    const RunLimits& limits);

[[nodiscard]] boost::json::object capability_json(
    const ProductSpec& product,
    const CapabilityRow& capability);

[[nodiscard]] boost::json::object audit_profiles(
    const std::vector<ProductSpec>& products,
    const std::filesystem::path& source_root);

[[nodiscard]] std::optional<ProductSpec> load_sandbox_profile(
    const std::filesystem::path& path,
    std::string& error);

[[nodiscard]] boost::json::object observation_json(const Observation& observation);
void emit_observations(
    const std::vector<Observation>& observations,
    bool jsonl,
    std::ostream& output);
void emit_matrix(
    const std::vector<const ProductSpec*>& products,
    bool jsonl,
    std::ostream& output);
void emit_audit(
    const boost::json::object& audit,
    bool jsonl,
    std::ostream& output);

[[nodiscard]] int run_application(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace exchange_probe
