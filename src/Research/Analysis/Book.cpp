#include "ResearchAnalysisInternal.hpp"

namespace exchange_probe::research_analysis {

[[nodiscard]] bool native_id_less(
    std::string_view lhs,
    std::string_view rhs) noexcept {
  const bool lhs_digits =
      !lhs.empty() &&
      std::all_of(lhs.begin(), lhs.end(), [](char value) {
        return value >= '0' && value <= '9';
      });
  const bool rhs_digits =
      !rhs.empty() &&
      std::all_of(rhs.begin(), rhs.end(), [](char value) {
        return value >= '0' && value <= '9';
      });
  if (lhs_digits && rhs_digits) {
    const auto lhs_first = lhs.find_first_not_of('0');
    const auto rhs_first = rhs.find_first_not_of('0');
    lhs = lhs_first == std::string_view::npos ? std::string_view{"0"}
                                              : lhs.substr(lhs_first);
    rhs = rhs_first == std::string_view::npos ? std::string_view{"0"}
                                              : rhs.substr(rhs_first);
    if (lhs.size() != rhs.size()) return lhs.size() < rhs.size();
  }
  return lhs < rhs;
}

[[nodiscard]] bool is_zero_decimal(std::string_view value) {
  if (value.empty()) return false;
  for (const char character : value) {
    if (character != '0' && character != '.') return false;
  }
  return true;
}

void apply_levels(
    BookState& state,
    const boost::json::value* value,
    bool bids) {
  if (value == nullptr || !value->is_array()) return;
  auto& side = bids ? state.bids : state.asks;
  for (const auto& level : value->as_array()) {
    if (!level.is_array() || level.as_array().size() < 2U) continue;
    const auto price = scalar_text(&level.as_array()[0]);
    const auto quantity = scalar_text(&level.as_array()[1]);
    if (price.empty() || quantity.empty()) continue;
    if (price.size() > kMaximumBookScalarBytes ||
        quantity.size() > kMaximumBookScalarBytes) {
      state.capacity_exceeded = true;
      continue;
    }
    if (is_zero_decimal(quantity)) {
      side.erase(price);
    } else {
      if (!side.contains(price) &&
          side.size() >= kMaximumBookLevelsPerSide) {
        state.capacity_exceeded = true;
        continue;
      }
      side[price] = quantity;
    }
  }
}

[[nodiscard]] std::pair<std::string, std::string> best_levels(
    const BookState& state) {
  const auto bid =
      state.bids.empty() ? std::string{} : state.bids.rbegin()->first;
  const auto ask =
      state.asks.empty() ? std::string{} : state.asks.begin()->first;
  return {bid, ask};
}

}
