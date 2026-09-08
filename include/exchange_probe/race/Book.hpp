#pragma once

#include "exchange_probe/race/Match.hpp"

#include "cxet/Primitives/Composite/BboReconstruction.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace exchange_probe::race {

inline constexpr std::size_t kMaximumBookLevels = 1024u;

enum class BookSide : std::uint8_t {
  Bid = 0u,
  Ask = 1u,
};

struct BookLevel {
  std::int64_t price{0};
  std::int64_t quantity{0};
};

struct BookMutation {
  BookSide side{BookSide::Bid};
  BookLevel level{};
};

struct SequenceUpdate {
  std::uint64_t first{0u};
  std::uint64_t last{0u};
  std::uint64_t previous{0u};
  bool hasPrevious{false};
};

struct BookApplyResult {
  bool accepted{false};
  bool bboChanged{false};
  bool top5Changed{false};
  bool top50Changed{false};
  bool gap{false};
  BboState before{};
  BboState after{};
  std::uint64_t top5Fingerprint{0u};
  std::uint64_t top50Fingerprint{0u};
};

class BoundedLocalBook {
 public:
  void invalidate(BookValidity reason) noexcept;
  [[nodiscard]] bool apply_snapshot(
      const BookLevel* bids, std::size_t bidCount,
      const BookLevel* asks, std::size_t askCount,
      std::uint64_t sequence) noexcept;
  [[nodiscard]] BookApplyResult apply_delta(
      BookSide side, BookLevel level, const SequenceUpdate& sequence) noexcept;
  [[nodiscard]] BookApplyResult apply_delta_batch(
      const BookMutation* mutations, std::size_t mutationCount,
      const SequenceUpdate& sequence) noexcept;

  [[nodiscard]] BookValidity validity() const noexcept { return validity_; }
  [[nodiscard]] std::uint64_t last_sequence() const noexcept {
    return lastSequence_;
  }
  [[nodiscard]] std::uint64_t gap_count() const noexcept { return gaps_; }
  [[nodiscard]] std::uint32_t resync_generation() const noexcept {
    return resyncGeneration_;
  }
  [[nodiscard]] BboState bbo() const noexcept;
  [[nodiscard]] std::uint64_t top_fingerprint(
      std::size_t depth) const noexcept;

 private:
  [[nodiscard]] static bool update_side(
      std::array<BookLevel, kMaximumBookLevels>& levels,
      std::size_t& count, BookSide side, BookLevel level) noexcept;
  static void sort_side(
      std::array<BookLevel, kMaximumBookLevels>& levels,
      std::size_t count, BookSide side) noexcept;
  [[nodiscard]] bool sequence_continuous(
      const SequenceUpdate& update) const noexcept;

  std::array<BookLevel, kMaximumBookLevels> bids_{};
  std::array<BookLevel, kMaximumBookLevels> asks_{};
  std::size_t bidCount_{0u};
  std::size_t askCount_{0u};
  std::uint64_t lastSequence_{0u};
  std::uint64_t gaps_{0u};
  std::uint32_t resyncGeneration_{0u};
  BookValidity validity_{BookValidity::Uninitialized};
};

class ExistingTradeBbo {
 public:
  [[nodiscard]] bool configure(std::int64_t tickSize) noexcept;
  void reset() noexcept;
  [[nodiscard]] bool seed(
      const BboState& bbo, std::uint64_t parentRecvMonoNs) noexcept;
  [[nodiscard]] bool apply_trade(
      std::int64_t price, std::int64_t quantity, std::uint8_t aggressorSide,
      std::uint64_t parentRecvMonoNs, BboState& output) noexcept;

 private:
  cxet::composite::BboReconstructionState state_{};
};

}  // namespace exchange_probe::race
