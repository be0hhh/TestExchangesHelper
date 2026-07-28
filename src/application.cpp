#include "exchange_probe/app.hpp"

#include <algorithm>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace exchange_probe {
namespace {

[[nodiscard]] bool case_selected(
    const CliOptions& options,
    std::string_view name) {
  return options.cases.empty() ||
         std::find(options.cases.begin(), options.cases.end(), name) !=
             options.cases.end();
}

[[nodiscard]] std::string first_capability(
    const std::vector<std::string>& capabilities) {
  return capabilities.empty() ? std::string{} : capabilities.front();
}

[[nodiscard]] Observation configuration_observation(
    const ProductSpec& product,
    const RestCase& probe_case,
    std::string error) {
  return {
      .kind = "observation",
      .venue = product.venue,
      .product = product.product,
      .case_name = probe_case.name,
      .capability = first_capability(probe_case.capabilities),
      .surface = Surface::Private,
      .transport = Transport::Rest,
      .wire = Wire::Json,
      .selection = probe_case.selection,
      .expectation = probe_case.expectation,
      .outcome = Outcome::ConfigurationError,
      .stage = "configuration",
      .error = std::move(error),
      .evidence = nullptr,
      .raw_public = std::nullopt,
  };
}

}  // namespace

int run_application(
    const CliOptions& options,
    std::ostream& output,
    std::ostream& error_output) {
  if (options.command == Command::Help) {
    print_help(output);
    return 0;
  }

  auto profiles = make_profiles();
  if (options.command == Command::Latency) {
    return run_latency(options, output, error_output);
  }
  if (options.command == Command::Stability) {
    return run_stability(options, output, error_output);
  }
  if (options.command == Command::Compare) {
    return run_compare(options, output, error_output);
  }
  if (options.command == Command::Placement) {
    return run_placement(options, output, error_output);
  }
  if (options.command == Command::Audit) {
    const auto audit = audit_profiles(profiles, *options.source_root);
    const bool ok = audit.at("ok").as_bool();
    emit_audit(audit, options.jsonl, output);
    return ok ? 0 : 1;
  }

  std::optional<ProductSpec> sandbox;
  std::vector<const ProductSpec*> selected;
  if (options.command == Command::Sandbox) {
    std::string sandbox_error;
    sandbox = load_sandbox_profile(*options.sandbox_file, sandbox_error);
    if (!sandbox.has_value()) {
      error_output << "configuration_error: " << sandbox_error << '\n';
      return 2;
    }
    selected.push_back(&*sandbox);
  } else {
    selected = select_products(profiles, options);
  }
  if (selected.empty()) {
    error_output << "configuration_error: filters selected no products\n";
    return 2;
  }
  if (options.command == Command::Matrix) {
    emit_matrix(selected, options.jsonl, output);
    return 0;
  }

  const auto surface = options.surface.value_or(Surface::Public);
  const auto transport = options.transport.value_or(Transport::Rest);
  if (options.command == Command::Sandbox && surface == Surface::Private) {
    error_output << "configuration_error: sandbox is public-only\n";
    return 2;
  }
  if (surface == Surface::Private && transport != Transport::Rest) {
    error_output << "configuration_error: private probes are REST-only\n";
    return 2;
  }
  if (surface == Surface::Private && options.limits.raw_public) {
    error_output
        << "configuration_error: --raw-public is forbidden for private probes\n";
    return 2;
  }
  if (surface == Surface::Private && !options.confirm_private) {
    error_output
        << "configuration_error: private REST requires --confirm-private\n";
    return 2;
  }
  if (surface == Surface::Public &&
      (options.confirm_private || options.env_file.has_value())) {
    error_output
        << "configuration_error: credential options require --surface private\n";
    return 2;
  }

  Environment environment;
  if (surface == Surface::Private) {
    std::string environment_error;
    environment = load_environment(options.env_file, environment_error);
    if (!environment_error.empty()) {
      error_output << "configuration_error: " << environment_error << '\n';
      return 2;
    }
  }

  std::vector<Observation> observations;
  for (const auto* product : selected) {
    if (surface == Surface::Public && transport == Transport::Rest) {
      for (const auto& probe_case : product->public_rest) {
        if (case_selected(options, probe_case.name)) {
          observations.push_back(
              run_public_rest(*product, probe_case, options.limits));
        }
      }
    } else if (surface == Surface::Public &&
               transport == Transport::WebSocket) {
      for (const auto& probe_case : product->public_ws) {
        if (case_selected(options, probe_case.name)) {
          observations.push_back(
              run_public_ws(*product, probe_case, options.limits));
        }
      }
    } else {
      const auto slots =
          configured_slots(environment, product->credential_prefix);
      for (const auto& probe_case : product->private_rest) {
        if (!case_selected(options, probe_case.name)) {
          continue;
        }
        if (slots.empty()) {
          observations.push_back(configuration_observation(
              *product, probe_case, "credentials_not_configured"));
          continue;
        }
        for (const auto slot : slots) {
          auto credentials = resolve_credentials(
              environment, product->credential_prefix, slot);
          if (!credentials.has_value()) {
            auto missing = configuration_observation(
                *product, probe_case, "credential_slot_incomplete");
            missing.case_name += "#slot" + std::to_string(slot);
            observations.push_back(std::move(missing));
            continue;
          }
          auto observation = run_private_rest(
              *product, probe_case, *credentials, options.limits);
          observation.case_name += "#slot" + std::to_string(slot);
          observations.push_back(std::move(observation));
        }
      }
    }
  }

  if (observations.empty()) {
    error_output << "configuration_error: filters selected no probe cases\n";
    return 2;
  }
  emit_observations(observations, options.jsonl, output);
  return std::all_of(
             observations.begin(),
             observations.end(),
             [](const Observation& observation) {
               return observation.expectation_met;
             })
             ? 0
             : 1;
}

}  // namespace exchange_probe
