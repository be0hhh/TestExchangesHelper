#include "Cadence.hpp"
#include <boost/json.hpp>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace exchange_probe::cadence;
void save(const std::filesystem::path& dir, const std::string& symbol,
          const Window& window, const std::vector<Lane>& direct,
          const std::vector<Lane>& cxet) {
  boost::json::array rows;
  for(const auto* group:{&direct,&cxet}) for(const auto& lane:*group) {
    const auto name=lane.path_kind+"_"+lane.feed.name+".csv";
    std::ofstream csv(dir/name);
    csv.exceptions(std::ios::badbit|std::ios::failbit);
    csv << "receive_ns,publish_ns,event_ns,transaction_ns,id,first_id,last_id,previous_id\n";
    for(const auto& r:lane.records)
      csv<<r.receive_ns<<','<<r.publish_ns<<','<<r.event_ns<<','<<r.transaction_ns<<','
         <<r.id<<','<<r.first_id<<','<<r.last_id<<','<<r.previous_id<<'\n';
    csv.close();
    rows.push_back(boost::json::object{
      {"name",lane.feed.name},{"path_kind",lane.path_kind},{"status",lane.status},
      {"error",lane.error},{"parser",lane.parser},{"endpoint",lane.endpoint},
      {"subscription",lane.subscription},{"frames",lane.frames},
      {"frames_available",lane.path_kind=="direct"},
      {"frames_semantics","decoded market-data WebSocket messages; fragmentation/control excluded"},
      {"parse_errors",lane.parse_errors},{"disconnects",lane.disconnects},
      {"warmup_parse_errors",lane.warmup_parse_errors},
      {"control_pings",lane.control_pings},{"overflow",lane.overflow},
      {"warmup_events",lane.warmup_events},{"csv",name}});
  }
  boost::json::object manifest{
    {"schema_version",1},{"symbol",symbol},
    {"start_monotonic_ns",window.start_ns.load()},
    {"clock","CLOCK_MONOTONIC_RAW"},{"duration_seconds",window.duration_seconds},
    {"warmup_seconds",30},{"connection_deadline_seconds",30},
    {"storage_limit_per_path_bytes",512ULL*1024*1024},
    {"lanes",std::move(rows)}};
  std::ofstream file(dir/"manifest.json");
  file.exceptions(std::ios::badbit|std::ios::failbit);
  file<<boost::json::serialize(manifest)<<'\n';
  file.close();
}
}
int main(int argc,char** argv) {
  if(argc!=3) {
    std::cerr<<"Usage: binance-feed-cadence SYMBOL NEW_OUTPUT_DIR\n"
      <<"Public live capture: 30s connect, 30s warmup, 180s measured; 19 direct plus supported CXET feeds.\n";
    return 2;
  }
  try {
    std::string symbol=argv[1],lower=symbol;
    if(symbol.empty() || symbol.size()>32) throw std::runtime_error("invalid symbol");
    for(auto& c:symbol) {
      if(!std::isalnum(static_cast<unsigned char>(c))) throw std::runtime_error("invalid symbol");
      c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    lower=symbol;
    for(auto& c:lower) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::filesystem::path dir=argv[2];
    if(std::filesystem::exists(dir)) throw std::runtime_error("output directory already exists");
    std::filesystem::create_directories(dir);
    Window window;
    std::vector<Lane> direct,cxet;
    for(const auto& feed:feeds(lower)) {
      Lane a; a.feed=feed; a.path_kind="direct"; direct.push_back(std::move(a));
      Lane b; b.feed=feed; b.path_kind="cxet"; cxet.push_back(std::move(b));
    }
    std::vector<std::jthread> readers;
    // stop-on-scope-exit precedes reader joins, including exception unwinding.
    struct Stop { Window& w; ~Stop(){w.stop=true;} } stop{window};
    readers.emplace_back([&]{captureCxet(symbol,window,cxet);});
    for(auto& lane:direct) readers.emplace_back([&lane,&window]{captureDirect(lane,window);});
    const auto deadline=nowNs()+30*second;
    while(window.prepared.load()<2*laneCount && nowNs()<deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto start=nowNs()+30*second;
    window.start_ns.store(start,std::memory_order_release);
    std::cout<<"prepared="<<window.prepared.load()<<"/38; warmup=30s; measured=180s\n"<<std::flush;
    unsigned next=30;
    while(nowNs()<start+window.duration_seconds*second) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const auto now=nowNs();
      if(now>=start+next*second) {
        std::cout<<"measured_elapsed="<<next<<"s\n"<<std::flush;
        next+=30;
      }
    }
    window.stop=true;
    for(auto& t:readers) t.join();
    save(dir,symbol,window,direct,cxet);
    unsigned observed=0;
    for(const auto* group:{&direct,&cxet}) for(const auto& lane:*group) {
      std::cout<<lane.path_kind<<' '<<lane.feed.name<<' '<<lane.status<<" events="<<lane.records.size()<<'\n';
      observed+=!lane.records.empty();
    }
    std::cout<<"capture="<<dir<<"; observed_lanes="<<observed<<"\n";
    return observed ? 0 : 1;
  } catch(const std::exception& e) {
    std::cerr<<"capture_error="<<e.what()<<'\n'; return 1;
  }
}
