#include "exchange_probe/Contracts.hpp"

#include "TestSupport.hpp"

#include <array>
#include <string>
#include <string_view>

int main() {
  using namespace exchange_probe;

  constexpr std::array<unsigned char, 22> compressed{
      0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x03, 0xab, 0xae, 0x05, 0x00, 0x43, 0xbf,
      0xa6, 0xa3, 0x02, 0x00, 0x00, 0x00,
  };
  const std::string_view fixture{
      reinterpret_cast<const char*>(compressed.data()),
      compressed.size()};
  std::string output;
  std::string error;
  require_test(
      decode_gzip_bounded(fixture, 16, output, error),
      "valid gzip fixture");
  require_test(output == "{}", "gzip payload");

  error.clear();
  require_test(
      !decode_gzip_bounded(fixture, 1, output, error),
      "gzip output bound");
  require_test(
      error == "gzip_output_capacity_exceeded",
      "gzip bound error");
  return 0;
}
