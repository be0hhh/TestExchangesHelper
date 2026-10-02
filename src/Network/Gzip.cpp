#include "exchange_probe/Contracts.hpp"

#include <boost/beast/zlib.hpp>
#include <boost/crc.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace exchange_probe {
namespace {

[[nodiscard]] std::uint16_t little_u16(
    std::string_view bytes,
    std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<unsigned char>(bytes[offset]) |
      static_cast<unsigned>(
          static_cast<unsigned char>(bytes[offset + 1]))
          << 8U);
}

[[nodiscard]] std::uint32_t little_u32(
    std::string_view bytes,
    std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(
      static_cast<unsigned char>(bytes[offset]) |
      static_cast<unsigned>(
          static_cast<unsigned char>(bytes[offset + 1]))
          << 8U |
      static_cast<unsigned>(
          static_cast<unsigned char>(bytes[offset + 2]))
          << 16U |
      static_cast<unsigned>(
          static_cast<unsigned char>(bytes[offset + 3]))
          << 24U);
}

[[nodiscard]] bool skip_zero_terminated(
    std::string_view input,
    std::size_t& cursor,
    std::size_t limit) {
  while (cursor < limit) {
    if (input[cursor++] == '\0') {
      return true;
    }
  }
  return false;
}

}  // namespace

bool decode_gzip_bounded(
    std::string_view input,
    std::size_t output_limit,
    std::string& output,
    std::string& error) {
  output.clear();
  if (input.size() < 18 ||
      static_cast<unsigned char>(input[0]) != 0x1fU ||
      static_cast<unsigned char>(input[1]) != 0x8bU ||
      static_cast<unsigned char>(input[2]) != 8U) {
    error = "gzip_header_invalid";
    return false;
  }
  const auto flags = static_cast<unsigned char>(input[3]);
  if ((flags & 0xe0U) != 0) {
    error = "gzip_reserved_flags";
    return false;
  }
  std::size_t cursor = 10;
  const std::size_t trailer = input.size() - 8;
  if ((flags & 0x04U) != 0) {
    if (trailer - std::min(trailer, cursor) < 2) {
      error = "gzip_extra_length_missing";
      return false;
    }
    const auto extra_size = little_u16(input, cursor);
    cursor += 2;
    if (extra_size > trailer - std::min(trailer, cursor)) {
      error = "gzip_extra_overflow";
      return false;
    }
    cursor += extra_size;
  }
  if ((flags & 0x08U) != 0 &&
      !skip_zero_terminated(input, cursor, trailer)) {
    error = "gzip_name_unterminated";
    return false;
  }
  if ((flags & 0x10U) != 0 &&
      !skip_zero_terminated(input, cursor, trailer)) {
    error = "gzip_comment_unterminated";
    return false;
  }
  if ((flags & 0x02U) != 0) {
    if (trailer - std::min(trailer, cursor) < 2) {
      error = "gzip_header_crc_missing";
      return false;
    }
    cursor += 2;
  }
  if (cursor >= trailer) {
    error = "gzip_deflate_missing";
    return false;
  }

  const auto expected_size = little_u32(input, input.size() - 4);
  if (expected_size > output_limit) {
    error = "gzip_output_capacity_exceeded";
    return false;
  }
  output.resize(std::min<std::size_t>(
      output_limit + 1,
      static_cast<std::size_t>(expected_size) + 1));

  boost::beast::zlib::inflate_stream inflater;
  boost::beast::zlib::z_params parameters{};
  parameters.next_in = input.data() + cursor;
  parameters.avail_in = trailer - cursor;
  parameters.next_out = output.data();
  parameters.avail_out = output.size();
  boost::system::error_code inflate_error;
  inflater.write(
      parameters,
      boost::beast::zlib::Flush::finish,
      inflate_error);
  if (inflate_error != boost::beast::zlib::error::end_of_stream ||
      parameters.avail_in != 0) {
    error = inflate_error ? inflate_error.message() : "gzip_trailing_deflate";
    output.clear();
    return false;
  }
  if (parameters.total_out > output_limit ||
      parameters.total_out != expected_size) {
    error = "gzip_output_size_mismatch";
    output.clear();
    return false;
  }
  output.resize(parameters.total_out);

  boost::crc_32_type crc;
  crc.process_bytes(output.data(), output.size());
  const auto expected_crc = little_u32(input, input.size() - 8);
  if (crc.checksum() != expected_crc) {
    error = "gzip_crc_mismatch";
    output.clear();
    return false;
  }
  return true;
}

}  // namespace exchange_probe
