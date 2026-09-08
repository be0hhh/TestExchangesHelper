#include "exchange_probe/Model.hpp"

#include <algorithm>
#include <array>
#include <tuple>

namespace exchange_probe {
namespace {

template <typename Enum, std::size_t Size>
[[nodiscard]] std::string_view enum_name(
    Enum value,
    const std::array<std::pair<Enum, std::string_view>, Size>& names,
    std::string_view fallback) noexcept {
  for (const auto& [candidate, name] : names) {
    if (candidate == value) {
      return name;
    }
  }
  return fallback;
}

[[nodiscard]] bool same_capability(
    const CapabilityRow& lhs,
    const CapabilityRow& rhs) noexcept {
  return std::tie(
             lhs.name,
             lhs.surface,
             lhs.transport,
             lhs.wire,
             lhs.selection,
             lhs.profile_status) ==
         std::tie(
             rhs.name,
             rhs.surface,
             rhs.transport,
             rhs.wire,
             rhs.selection,
             rhs.profile_status);
}

void add_capability(std::vector<CapabilityRow>& rows, CapabilityRow row) {
  if (std::none_of(rows.begin(), rows.end(), [&](const CapabilityRow& existing) {
        return same_capability(existing, row);
      })) {
    rows.push_back(std::move(row));
  }
}

[[nodiscard]] bool has_named_capability(
    const std::vector<CapabilityRow>& rows,
    std::string_view name) {
  return std::any_of(rows.begin(), rows.end(), [&](const CapabilityRow& row) {
    return row.name == name && row.profile_status != ProfileStatus::NotApplicable;
  });
}

}  // namespace

std::string_view to_string(Surface value) noexcept {
  static constexpr std::array names{
      std::pair{Surface::Public, std::string_view{"public"}},
      std::pair{Surface::Private, std::string_view{"private"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(Transport value) noexcept {
  static constexpr std::array names{
      std::pair{Transport::None, std::string_view{"none"}},
      std::pair{Transport::Rest, std::string_view{"rest"}},
      std::pair{Transport::WebSocket, std::string_view{"ws"}},
      std::pair{Transport::Fix, std::string_view{"fix"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(Wire value) noexcept {
  static constexpr std::array names{
      std::pair{Wire::None, std::string_view{"none"}},
      std::pair{Wire::Json, std::string_view{"json"}},
      std::pair{Wire::BinaryJson, std::string_view{"binary_json"}},
      std::pair{Wire::Sbe, std::string_view{"sbe_binary"}},
      std::pair{Wire::Protobuf, std::string_view{"protobuf_binary"}},
      std::pair{Wire::FixSbe, std::string_view{"fix_sbe_binary"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(Selection value) noexcept {
  static constexpr std::array names{
      std::pair{Selection::CoreSelected, std::string_view{"profile_selected"}},
      std::pair{
          Selection::DiagnosticVariant,
          std::string_view{"diagnostic_variant"}},
      std::pair{
          Selection::ExternalAdapterRequired,
          std::string_view{"external_adapter_required"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(ProfileStatus value) noexcept {
  static constexpr std::array names{
      std::pair{ProfileStatus::Profiled, std::string_view{"profiled"}},
      std::pair{ProfileStatus::NotProfiled, std::string_view{"not_profiled"}},
      std::pair{
          ProfileStatus::NotApplicable,
          std::string_view{"not_applicable"}},
      std::pair{
          ProfileStatus::ExternalAdapterRequired,
          std::string_view{"external_adapter_required"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(Outcome value) noexcept {
  static constexpr std::array names{
      std::pair{Outcome::NotRun, std::string_view{"not_run"}},
      std::pair{Outcome::Success, std::string_view{"success"}},
      std::pair{
          Outcome::ExpectedRejection,
          std::string_view{"expected_rejection"}},
      std::pair{Outcome::TransportError, std::string_view{"transport_error"}},
      std::pair{Outcome::TlsError, std::string_view{"tls_error"}},
      std::pair{Outcome::ProxyError, std::string_view{"proxy_error"}},
      std::pair{Outcome::HttpError, std::string_view{"http_error"}},
      std::pair{Outcome::LogicalError, std::string_view{"logical_error"}},
      std::pair{Outcome::SchemaError, std::string_view{"schema_error"}},
      std::pair{Outcome::Timeout, std::string_view{"timeout"}},
      std::pair{
          Outcome::CapacityExceeded,
          std::string_view{"capacity_exceeded"}},
      std::pair{Outcome::Unsupported, std::string_view{"unsupported"}},
      std::pair{
          Outcome::ConfigurationError,
          std::string_view{"configuration_error"}},
      std::pair{Outcome::InternalError, std::string_view{"internal_error"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(Expectation value) noexcept {
  static constexpr std::array names{
      std::pair{
          Expectation::LogicalSuccess,
          std::string_view{"logical_success"}},
      std::pair{
          Expectation::SymbolRejected,
          std::string_view{"symbol_rejected"}},
  };
  return enum_name(value, names, "unknown");
}

std::string_view to_string(RestContract value) noexcept {
  static constexpr std::array names{
      std::pair{RestContract::None, std::string_view{"none"}},
      std::pair{
          RestContract::BinanceTicker24h,
          std::string_view{"binance_ticker24h"}},
      std::pair{
          RestContract::BinanceFunding,
          std::string_view{"binance_funding"}},
      std::pair{
          RestContract::BybitTicker24h,
          std::string_view{"bybit_ticker24h"}},
      std::pair{
          RestContract::BybitFunding,
          std::string_view{"bybit_funding"}},
      std::pair{
          RestContract::OkxTicker24h,
          std::string_view{"okx_ticker24h"}},
      std::pair{RestContract::OkxFunding, std::string_view{"okx_funding"}},
      std::pair{
          RestContract::GateTicker24h,
          std::string_view{"gate_ticker24h"}},
      std::pair{RestContract::GateFunding, std::string_view{"gate_funding"}},
      std::pair{
          RestContract::KucoinTicker24h,
          std::string_view{"kucoin_ticker24h"}},
      std::pair{
          RestContract::KucoinFunding,
          std::string_view{"kucoin_funding"}},
      std::pair{
          RestContract::BitgetTicker24h,
          std::string_view{"bitget_ticker24h"}},
      std::pair{
          RestContract::BitgetFunding,
          std::string_view{"bitget_funding"}},
      std::pair{
          RestContract::BinancePrivate,
          std::string_view{"binance_private"}},
      std::pair{
          RestContract::BybitPrivate,
          std::string_view{"bybit_private"}},
      std::pair{RestContract::OkxPrivate, std::string_view{"okx_private"}},
      std::pair{RestContract::GatePrivate, std::string_view{"gate_private"}},
      std::pair{
          RestContract::KucoinPrivate,
          std::string_view{"kucoin_private"}},
      std::pair{
          RestContract::BitgetPrivate,
          std::string_view{"bitget_private"}},
  };
  return enum_name(value, names, "unknown");
}

void finalize_capabilities(ProductSpec& product) {
  std::vector<CapabilityRow> rows;

  const auto add_rest = [&](const RestCase& probe_case, Surface surface) {
    for (const auto& capability : probe_case.capabilities) {
      add_capability(
          rows,
          CapabilityRow{
              .name = capability,
              .surface = surface,
              .transport = Transport::Rest,
              .wire = Wire::Json,
              .selection = probe_case.selection,
              .profile_status = ProfileStatus::Profiled,
              .requires_confirmation = surface == Surface::Private,
              .native = probe_case.native,
          });
    }
  };
  for (const auto& probe_case : product.public_rest) {
    add_rest(probe_case, Surface::Public);
  }
  for (const auto& probe_case : product.private_rest) {
    add_rest(probe_case, Surface::Private);
  }
  for (const auto& probe_case : product.public_ws) {
    for (const auto& capability : probe_case.capabilities) {
      add_capability(
          rows,
          CapabilityRow{
              .name = capability,
              .surface = Surface::Public,
              .transport = Transport::WebSocket,
              .wire = probe_case.wire,
              .selection = probe_case.selection,
              .profile_status = ProfileStatus::Profiled,
              .requires_confirmation = false,
              .native = true,
          });
    }
  }
  for (const auto& probe_case : product.fix_sessions) {
    add_capability(
        rows,
        CapabilityRow{
            .name = probe_case.capability,
            .surface = Surface::Public,
            .transport = Transport::Fix,
            .wire = Wire::FixSbe,
            .selection = Selection::ExternalAdapterRequired,
            .profile_status = ProfileStatus::ExternalAdapterRequired,
            .requires_confirmation = false,
            .native = false,
        });
  }

  static constexpr std::array public_common{
      "exchange_info",
      "instrument_catalog",
      "instrument_detail",
      "ticker_24h",
      "historical_trades",
      "live_trades",
      "live_bbo",
      "live_l2",
  };
  static constexpr std::array public_futures{
      "funding_current_all",
      "funding_current_symbol",
      "funding_history",
  };
  static constexpr std::array private_common{
      "balances",
      "fills_history",
      "open_orders",
      "order_history",
      "user_account_stream",
      "user_orders_stream",
      "user_trades_stream",
  };

  const auto add_missing = [&](std::string_view name, Surface surface) {
    if (!has_named_capability(rows, name)) {
      add_capability(
          rows,
          CapabilityRow{
              .name = std::string{name},
              .surface = surface,
              .transport = Transport::None,
              .wire = Wire::None,
              .selection = Selection::DiagnosticVariant,
              .profile_status = ProfileStatus::NotProfiled,
              .requires_confirmation = surface == Surface::Private,
              .native = false,
          });
    }
  };

  for (const auto* name : public_common) {
    add_missing(name, Surface::Public);
  }
  if (product.product == "futures") {
    for (const auto* name : public_futures) {
      add_missing(name, Surface::Public);
    }
  } else {
    for (const auto* name : public_futures) {
      add_capability(
          rows,
          CapabilityRow{
              .name = name,
              .surface = Surface::Public,
              .transport = Transport::None,
              .wire = Wire::None,
              .selection = Selection::DiagnosticVariant,
              .profile_status = ProfileStatus::NotApplicable,
              .requires_confirmation = false,
              .native = false,
          });
    }
  }
  for (const auto* name : private_common) {
    add_missing(name, Surface::Private);
  }
  if (product.product == "futures") {
    add_missing("positions", Surface::Private);
  } else {
    add_capability(
        rows,
        CapabilityRow{
            .name = "positions",
            .surface = Surface::Private,
            .transport = Transport::None,
            .wire = Wire::None,
            .selection = Selection::DiagnosticVariant,
            .profile_status = ProfileStatus::NotApplicable,
            .requires_confirmation = true,
            .native = false,
        });
  }

  std::stable_sort(
      rows.begin(),
      rows.end(),
      [](const CapabilityRow& lhs, const CapabilityRow& rhs) {
        return std::tie(lhs.surface, lhs.name, lhs.profile_status, lhs.transport) <
               std::tie(rhs.surface, rhs.name, rhs.profile_status, rhs.transport);
      });
  product.capabilities = std::move(rows);
}

}  // namespace exchange_probe
