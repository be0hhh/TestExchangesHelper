#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe::race::svg {

[[nodiscard]] bool write_bars(
    const std::filesystem::path& path, const std::string& title,
    const std::string& yLabel,
    const std::vector<std::pair<std::string, long double>>& values,
    std::string& error);
[[nodiscard]] bool write_ecdf(
    const std::filesystem::path& path, const std::string& title,
    const std::vector<std::int64_t>& signedNanoseconds,
    std::string& error);
[[nodiscard]] bool write_timeline(
    const std::filesystem::path& path, const std::string& title,
    const std::vector<std::pair<std::string, std::int64_t>>& points,
    std::string& error);

}  // namespace exchange_probe::race::svg
