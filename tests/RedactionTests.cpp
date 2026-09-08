#include "exchange_probe/Contracts.hpp"

#include "TestSupport.hpp"

#include <boost/json/parse.hpp>

#include <string>

int main() {
  using namespace exchange_probe;

  const auto shape = shape_of(boost::json::parse(
      R"({"apiKey":"secret-value","data":[{"price":"1"}]})"));
  require_test(
      shape.as_object().at("apiKey").is_string(),
      "sensitive shape field type");
  require_test(
      shape.as_object().at("apiKey").as_string() == "<redacted>",
      "sensitive shape field");
  require_test(
      bounded_public_frame(std::string(kMaxRawPublicBytes + 100, 'x')).size() ==
          kMaxRawPublicBytes,
      "raw public bound");
  return 0;
}
