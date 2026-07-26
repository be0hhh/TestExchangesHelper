#include "exchange_probe/contracts.hpp"

#include "test_support.hpp"

#include <boost/json/parse.hpp>

#include <string>

int main() {
  using namespace exchange_probe;

  const WsCase topic_case{
      .data_kind = WsDataKind::TopicJson,
      .expected_symbol = "BTCUSDT",
      .expected_topic = "trade",
  };
  const auto data = boost::json::parse(
      R"({"topic":"trade","symbol":"BTCUSDT","price":"100"})");
  require_test(
      validate_ws_data(topic_case, data, true, false, {}).matched,
      "direct JSON event data");
  const auto wrong_symbol = boost::json::parse(
      R"({"topic":"trade","symbol":"ETHUSDT","price":"100"})");
  require_test(
      !validate_ws_data(topic_case, wrong_symbol, true, false, {}).matched,
      "wrong symbol must fail");

  const std::string protobuf{
      static_cast<char>(0x0a), static_cast<char>(0x07),
      'B', 'T', 'C', 'U', 'S', 'D', 'T',
  };
  const WsCase protobuf_case{
      .data_kind = WsDataKind::ProtobufEnvelope,
      .expected_symbol = "BTCUSDT",
  };
  require_test(
      validate_ws_data(
          protobuf_case, boost::json::value{}, false, true, protobuf)
          .matched,
      "bounded protobuf wire");
  return 0;
}
