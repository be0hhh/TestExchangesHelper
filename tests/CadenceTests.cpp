#include "Cadence.hpp"
#include <iostream>
#include <set>
#include <stdexcept>
using namespace exchange_probe::cadence;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    const auto catalog = feeds("ethusdt");
    check(catalog.size() == 19, "all 19 feeds must be present");
    std::set<std::string> topics;
    for (const auto& feed : catalog) topics.insert(feed.topic);
    check(topics.size() == 19, "no aliased or duplicate subscriptions");
    check(topics.contains("ethusdt@depth@0ms"), "explicit zero interval");
    check(topics.contains("ethusdt@depth20@500ms"), "partial depth matrix");
    check(catalog[0].path == "/ws", "raw trade matches actual CXET endpoint");
    check(catalog[1].path == "/market/ws", "aggregate trade uses market endpoint");
    Lane lane;
    Window window;
    window.start_ns = 100 * second;
    window.duration_seconds = 180;
    check(!append(lane, Record{.receive_ns=99*second}, window), "exclude warmup");
    check(append(lane, Record{.receive_ns=100*second}, window), "include start");
    check(!append(lane, Record{.receive_ns=280*second}, window), "exclude end");
    check(lane.records.size()==1 && lane.warmup_events==1, "window accounting");
    lane.records.resize(laneRecordLimit);
    check(!append(lane, Record{.receive_ns=101*second}, window), "bounded storage");
    check(lane.overflow==1 && lane.status=="incomplete", "overflow visible");
    Record row;
    bool control=false;
    check(decode(catalog[0], R"({"e":"trade","s":"ETHUSDT","E":123,"T":122,"t":9})", row, control), "trade decode");
    check(row.id==9 && row.event_ns==123000000 && row.transaction_ns==122000000, "timestamp units");
    check(!decode(catalog[0], R"({"e":"aggTrade","s":"ETHUSDT","a":9})", row, control), "reject substituted stream");
    check(!decode(catalog[0], R"({"e":"trade","s":"BTCUSDT","t":9})", row, control), "reject wrong symbol");
    check(!decode(catalog[0], R"({"result":null,"id":1})", row, control) && control, "ACK is not data");
    check(!decode(catalog[0], R"({"e":"trade","s":"ETHUSDT","t":-1})", row, control) && !control, "reject negative ID");
    check(decode(catalog[1], R"({"e":"aggTrade","s":"ETHUSDT","a":42,"f":9,"l":12})", row, control), "aggregate identity");
    check(row.id==42 && row.first_id==9 && row.last_id==12, "aggregate trade range");
    check(decode(catalog[3], R"({"e":"depthUpdate","s":"ETHUSDT","U":10,"u":15,"pu":9})", row, control), "depth identity");
    check(row.id==15 && row.first_id==10 && row.previous_id==9, "depth continuity fields");
    std::cout << "cadence tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
