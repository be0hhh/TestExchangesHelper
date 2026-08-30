#include "race_svg.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace exchange_probe::race::svg {
namespace {

[[nodiscard]] std::string escape(const std::string& value) {
  std::string output;
  output.reserve(value.size());
  for (const char byte : value) {
    if (byte == '&') output += "&amp;";
    else if (byte == '<') output += "&lt;";
    else if (byte == '>') output += "&gt;";
    else if (byte == '\"') output += "&quot;";
    else output += byte;
  }
  return output;
}

[[nodiscard]] bool finish(
    std::ofstream& output, const std::filesystem::path& path,
    std::string& error) {
  output << "</svg>\n";
  output.flush();
  if (output) return true;
  error = "svg_write_failed:" + path.string();
  return false;
}

void header(std::ofstream& output, const std::string& title) {
  output << R"(<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="680" viewBox="0 0 1200 680">)"
         << "\n<rect width=\"1200\" height=\"680\" fill=\"#0b1020\"/>\n"
         << "<text x=\"50\" y=\"46\" fill=\"#e7edf8\" font-family=\"sans-serif\" font-size=\"24\">"
         << escape(title) << "</text>\n";
}

}  // namespace

bool write_bars(
    const std::filesystem::path& path, const std::string& title,
    const std::string& yLabel,
    const std::vector<std::pair<std::string, long double>>& values,
    std::string& error) {
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    error = "svg_open_failed:" + path.string();
    return false;
  }
  header(output, title);
  output << "<text x=\"50\" y=\"75\" fill=\"#9fb0c9\" font-family=\"sans-serif\" font-size=\"14\">"
         << escape(yLabel) << "</text>\n";
  if (values.empty()) {
    output << "<text x=\"50\" y=\"130\" fill=\"#9fb0c9\" font-family=\"sans-serif\">no matched data</text>\n";
    return finish(output, path, error);
  }
  long double maximum = 0.0L;
  for (const auto& value : values)
    maximum = std::max(maximum, std::fabs(value.second));
  if (maximum == 0.0L) maximum = 1.0L;
  const long double width = 1080.0L /
                            static_cast<long double>(values.size());
  const long double baseline = 350.0L;
  for (std::size_t index = 0u; index < values.size(); ++index) {
    const auto height = 250.0L * std::fabs(values[index].second) / maximum;
    const auto x = 70.0L + width * static_cast<long double>(index);
    const auto y = values[index].second >= 0.0L ? baseline - height : baseline;
    output << std::fixed << std::setprecision(2)
           << "<rect x=\"" << x << "\" y=\"" << y
           << "\" width=\"" << std::max(2.0L, width - 8.0L)
           << "\" height=\"" << height << "\" fill=\""
           << (values[index].second >= 0.0L ? "#4fd1c5" : "#f687b3")
           << "\"/>\n"
           << "<text x=\"" << x << "\" y=\"620\" fill=\"#c8d3e3\" font-family=\"monospace\" font-size=\"11\" transform=\"rotate(-45 "
           << x << " 620)\">" << escape(values[index].first)
           << "</text>\n";
  }
  output << "<line x1=\"50\" y1=\"350\" x2=\"1170\" y2=\"350\" stroke=\"#718096\"/>\n";
  return finish(output, path, error);
}

bool write_ecdf(
    const std::filesystem::path& path, const std::string& title,
    const std::vector<std::int64_t>& signedNanoseconds,
    std::string& error) {
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    error = "svg_open_failed:" + path.string();
    return false;
  }
  header(output, title);
  if (signedNanoseconds.empty()) {
    output << "<text x=\"50\" y=\"130\" fill=\"#9fb0c9\" font-family=\"sans-serif\">no matched data</text>\n";
    return finish(output, path, error);
  }
  auto values = signedNanoseconds;
  std::sort(values.begin(), values.end());
  const auto minimum = values.front();
  const auto maximum = values.back();
  const auto span = std::max<std::int64_t>(1, maximum - minimum);
  output << "<polyline fill=\"none\" stroke=\"#63b3ed\" stroke-width=\"3\" points=\"";
  for (std::size_t index = 0u; index < values.size(); ++index) {
    const long double x = 70.0L + 1080.0L *
        static_cast<long double>(values[index] - minimum) /
        static_cast<long double>(span);
    const long double y = 610.0L - 520.0L *
        static_cast<long double>(index + 1u) /
        static_cast<long double>(values.size());
    output << std::fixed << std::setprecision(2) << x << ',' << y << ' ';
  }
  output << "\"/>\n<text x=\"50\" y=\"655\" fill=\"#9fb0c9\" font-family=\"sans-serif\">signed delta, microseconds (A - B)</text>\n";
  return finish(output, path, error);
}

bool write_timeline(
    const std::filesystem::path& path, const std::string& title,
    const std::vector<std::pair<std::string, std::int64_t>>& points,
    std::string& error) {
  std::ofstream output{path, std::ios::binary};
  if (!output) {
    error = "svg_open_failed:" + path.string();
    return false;
  }
  header(output, title);
  if (points.empty()) {
    output << "<text x=\"50\" y=\"130\" fill=\"#9fb0c9\" font-family=\"sans-serif\">no matched data</text>\n";
    return finish(output, path, error);
  }
  auto minimum = points.front().second;
  auto maximum = points.front().second;
  for (const auto& point : points) {
    minimum = std::min(minimum, point.second);
    maximum = std::max(maximum, point.second);
  }
  const auto span = std::max<std::int64_t>(1, maximum - minimum);
  for (std::size_t index = 0u; index < points.size(); ++index) {
    const long double x = 80.0L + 1040.0L *
        static_cast<long double>(points[index].second - minimum) /
        static_cast<long double>(span);
    const long double y = 100.0L + 45.0L * static_cast<long double>(index);
    output << "<circle cx=\"" << x << "\" cy=\"" << y
           << "\" r=\"6\" fill=\"#f6ad55\"/>\n"
           << "<text x=\"50\" y=\"" << y + 4.0L
           << "\" fill=\"#c8d3e3\" font-family=\"monospace\" font-size=\"12\">"
           << escape(points[index].first) << "</text>\n";
  }
  return finish(output, path, error);
}

}  // namespace exchange_probe::race::svg
