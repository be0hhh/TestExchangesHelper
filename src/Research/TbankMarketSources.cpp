#include "TbankMarketSources.hpp"
#include "TbankGrpcProtocol.hpp"
#include "exchange_probe/TbankCredential.hpp"
#include "../Network/NetCommon.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <ostream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

namespace exchange_probe {
namespace {
using namespace tbank_grpc;
constexpr std::string_view host = "invest-public-api.tbank.ru";
constexpr std::uint64_t second = 1'000'000'000ULL, reserve = 65536;
std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t add(std::uint64_t a, std::uint64_t b) {
  return b > std::numeric_limits<std::uint64_t>::max() - a ? std::numeric_limits<std::uint64_t>::max() : a + b;
}
bool numeric(const std::string& ip) {
  std::array<unsigned char,16> buf{};
  return ip.find('\0') == std::string::npos &&
      (::inet_pton(AF_INET,ip.c_str(),buf.data()) == 1 || ::inet_pton(AF_INET6,ip.c_str(),buf.data()) == 1);
}
bool same_ip(const std::string& a, const std::string& b) {
  std::array<unsigned char,16> x{}, y{};
  for (int family : {AF_INET, AF_INET6})
    if (::inet_pton(family,a.c_str(),x.data()) == 1 && ::inet_pton(family,b.c_str(),y.data()) == 1) return x == y;
  return false;
}
class Artifacts {
 public:
  ~Artifacts() { for (int fd : fd_) if (fd >= 0) ::close(fd); }
  bool create(const std::filesystem::path& root, std::uint64_t limit) {
    limit_ = limit; std::error_code ec; std::filesystem::create_directories(root,ec);
    if (ec) return false;
    auto pattern = (root / "tbank-market-sources-XXXXXX").string();
    std::vector<char> name(pattern.begin(),pattern.end()); name.push_back('\0');
    auto* dir = ::mkdtemp(name.data()); if (!dir) return false;
    directory = dir;
    const std::array<const char*,3> names{"public-frames.bin","public-frames.index.jsonl","events.jsonl"};
    for (std::size_t i=0;i<3;++i) {
      fd_[i] = ::open((directory/names[i]).c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
      if (fd_[i]<0) return false;
    }
    return true;
  }
  bool event(boost::json::object value, bool summary=false) {
    auto line=boost::json::serialize(value)+'\n';
    if (line.size()>remaining(summary) || !write(fd_[2],line.data(),line.size())) return false;
    used_+=line.size(); return true;
  }
  bool body(std::span<const std::byte> body, unsigned phase, std::uint64_t timestamp) {
    boost::json::object entry{{"phase",phase},{"offset",offset_},{"bytes",body.size()},{"monotonic_ns",timestamp}};
    auto line=boost::json::serialize(entry)+'\n';
    if (body.size()+line.size()>remaining(false)) return false;
    // Roll back incomplete writes: an index always refers to an entire body.
    auto raw_start=::lseek(fd_[0],0,SEEK_CUR), index_start=::lseek(fd_[1],0,SEEK_CUR);
    if (raw_start<0 || index_start<0 || !write(fd_[0],body.data(),body.size()) || !write(fd_[1],line.data(),line.size())) {
      if (raw_start>=0) { (void)::ftruncate(fd_[0],raw_start); (void)::lseek(fd_[0],raw_start,SEEK_SET); }
      if (index_start>=0) { (void)::ftruncate(fd_[1],index_start); (void)::lseek(fd_[1],index_start,SEEK_SET); }
      return false;
    }
    offset_+=body.size(); used_+=body.size()+line.size(); return true;
  }
  std::filesystem::path directory;
 private:
  std::uint64_t remaining(bool summary) const {
    const auto ceiling=summary ? limit_ : limit_-reserve;
    return used_>=ceiling ? 0 : ceiling-used_;
  }
  static bool write(int fd,const void* data,std::size_t size) {
    auto* bytes=static_cast<const char*>(data); std::size_t done=0;
    while (done<size) { auto n=::write(fd,bytes+done,size-done); if(n<0&&errno==EINTR)continue; if(n<=0)return false; done+=static_cast<std::size_t>(n); }
    return true;
  }
  std::array<int,3> fd_{-1,-1,-1};
  std::uint64_t limit_{},used_{},offset_{};
};
const char* decode_name(MarketDecode result) {
  switch(result) {
    case MarketDecode::Ack:return "ack";
    case MarketDecode::Data:return "data";
    case MarketDecode::MalformedSyntax:return "malformed_syntax";
    case MarketDecode::SchemaAnomaly:return "schema_anomaly";
    case MarketDecode::Other:return "other";
  }
  return "other";
}
} // namespace
int tbank_grpc::detail::run_market_sources(const CliOptions& o,const Factories& factories,
    std::ostream& out,std::ostream& err,const MarketSourcesRuntime& runtime) {
  if (o.command!=Command::Research || o.action!="tbank-market-sources" || o.engines!="nghttp2" || !factories.nghttp2 ||
      !o.env_file || o.env_file->empty() || !o.ca_file || o.ca_file->empty() ||
      o.ca_file->native().size()>4096 || o.ca_file->native().find('\0')!=std::string::npos ||
      !o.output_dir || o.output_dir->empty() || o.symbol!=kSberUid || !o.duration_seconds ||
      !*o.duration_seconds || *o.duration_seconds>20 || !o.overall_timeout_seconds || o.overall_timeout_seconds>120 ||
      o.limits.timeout<std::chrono::milliseconds{100} || o.limits.timeout>std::chrono::milliseconds{5000} ||
      o.limits.attempts!=1 || o.max_artifact_bytes<reserve || o.max_artifact_bytes>4194304 ||
      o.open_viewer || o.capture_pcap || o.capture_tls_keys || o.confirm_private || !runtime.now || !runtime.resolve || !runtime.idle) {
    err<<"configuration_error: invalid_tbank_market_sources_configuration\n";return 2;
  }
  try {
    Artifacts artifacts;
    if(!artifacts.create(*o.output_dir,o.max_artifact_bytes)) {err<<"artifact_error\n";return 2;}
    bool inconclusive=false, stop=false, io_ok=true;
    const auto started=runtime.now(), overall_end=add(started,o.overall_timeout_seconds*second);
    const auto timeout=static_cast<std::uint64_t>(o.limits.timeout.count())*1'000'000ULL;
    std::string credential_error;
    auto credential=load_tbank_credential(*o.env_file,o.api_slot,credential_error);
    // Neither credential errors nor resolver errors are reflected into artifacts.
    if(!credential) {inconclusive=true;stop=true;}
    std::string pinned;
    if(!stop) pinned=runtime.resolve(std::min(overall_end,add(runtime.now(),timeout)));
    if(!stop && (!numeric(pinned) || runtime.now()>=overall_end)) {inconclusive=true;stop=true;}
    io_ok=artifacts.event({{"type","run"},{"host",host},{"pinned_ip",pinned},
        {"uid",kSberUid},{"production_source_policy","reject_source_other_than_1_static_only"}});
    unsigned completed=0;
    for(unsigned phase=0;phase<4 && !stop && io_ok;++phase) {
      const auto kind=phase<2 ? MarketKind::Book : MarketKind::Trades;
      const unsigned source=phase%2 ? 3u : 1u;
      const auto requested_end=add(runtime.now(),*o.duration_seconds*second);
      const auto phase_end=std::min(overall_end,requested_end);
      if(runtime.now()>=phase_end) {inconclusive=true;break;}
      bool transport_failed=false, ack=false, rejected=false, bad=phase_end<requested_end, ended=false, samples_closed=false;
      std::uint32_t event_mask=0;
      std::uint64_t messages=0,data=0,samples=0,sample_bytes=0;
      std::array<std::uint64_t,5> ack_sources{},data_sources{};
      auto connection=factories.nghttp2();
      if(!connection) {inconclusive=true;break;}
      auto sink=[&](const Event& e) {
        const auto bit=1u<<static_cast<unsigned>(e.kind);
        if(e.kind!=EventKind::Message && !(event_mask & bit)) {
          event_mask|=bit;
          io_ok=artifacts.event({{"type","transport_event"},{"phase",phase},{"kind",static_cast<unsigned>(e.kind)},
              {"http_status",e.http_status_present?boost::json::value(e.http_status):boost::json::value(nullptr)},
              {"grpc_status",e.grpc_status_present?boost::json::value(e.grpc_status):boost::json::value(nullptr)}}) && io_ok;
        }
        if(e.kind==EventKind::Message) {
          if(messages!=std::numeric_limits<std::uint64_t>::max())++messages;
          if(e.payload.size()>65536) {
            bad=true;ended=true;
            io_ok=artifacts.event({{"type","oversized_body"},{"phase",phase},{"bytes",e.payload.size()}},true) && io_ok;
            return;
          }
          if(samples>=32 || e.payload.size()>384*1024-sample_bytes)samples_closed=true;
          const bool sampled=!samples_closed;
          // Copy/write complete borrowed PUBLIC body before any semantic decode.
          if(sampled) {
            if(!artifacts.body(e.payload,phase,runtime.now())) {io_ok=false;ended=true;return;}
            ++samples;sample_bytes+=e.payload.size();
          }
          const auto observation=inspect_market_source(e.payload,kind);
          auto& histogram=observation.result==MarketDecode::Ack ? ack_sources : data_sources;
          if(observation.result==MarketDecode::Ack || observation.result==MarketDecode::Data) {
            const auto bucket=!observation.source_present ? 0u : observation.source>=1 && observation.source<=3 ? static_cast<unsigned>(observation.source) : 4u;
            if(histogram[bucket]!=std::numeric_limits<std::uint64_t>::max())++histogram[bucket];
            if(observation.uid!=kSberUid)bad=true;
            if(observation.result==MarketDecode::Ack) {
              if(observation.status==1 && observation.uid==kSberUid)ack=true;
              else {rejected=true;ended=true;}
            } else if(data!=std::numeric_limits<std::uint64_t>::max())++data;
          } else if(observation.result!=MarketDecode::Other) bad=true;
          if(sampled) {
            boost::json::object row{{"type","message"},{"phase",phase},{"decode",decode_name(observation.result)},
              {"uid",observation.uid},{"status",observation.status_present ? boost::json::value(observation.status):boost::json::value(nullptr)},
              {"source",observation.source_present ? boost::json::value(observation.source):boost::json::value(nullptr)},
              {"source_matches_request",observation.source_present && observation.source==source},
              {"depth",observation.depth_present ? boost::json::value(observation.depth):boost::json::value(nullptr)}};
            io_ok=artifacts.event(std::move(row));if(!io_ok)ended=true;
          }
        } else if(e.kind==EventKind::Headers) {
          if((e.http_status_present && e.http_status!=200) || (e.grpc_status_present && e.grpc_status!=0)) {transport_failed=true;ended=true;}
        } else if(e.kind==EventKind::RpcComplete || e.kind==EventKind::Failure || e.kind==EventKind::Eof ||
                  e.kind==EventKind::Goaway || e.kind==EventKind::StreamReset) {transport_failed=true;ended=true;}
      };
      io_ok=artifacts.event({{"type","configured_request"},{"phase",phase},{"kind",kind==MarketKind::Book?"book":"trades"},
          {"source",source},{"uid",kSberUid},{"depth",kind==MarketKind::Book?boost::json::value(50):boost::json::value(nullptr)}});
      const auto ca=o.ca_file->string();
      const ConnectOptions config{.host=host,.pinned_ip=pinned,.ca_file=ca,.deadline_ns=std::min(phase_end,add(runtime.now(),timeout)),
          .stop_requested=[&]{return transport_failed || !io_ok || runtime.now()>=phase_end;}};
      if(!io_ok || !connection->connect(config,sink,{}) || transport_failed || !same_ip(connection->peer_ip(),pinned)) {
        connection->close();inconclusive=true;stop=true;
      } else {
        io_ok=artifacts.event({{"type","peer_verified"},{"phase",phase},{"matched",true}}) && io_ok;
        auto request=market_source_request(kind,source);
        const auto stream=io_ok ? connection->begin_rpc(kMarketStreamPath,credential->token(),request,true,phase_end) : 0u;
        io_ok=artifacts.event({{"type","queued_request"},{"phase",phase},{"stream_id",stream},{"queued",stream!=0}}) && io_ok;
        if(!stream) {transport_failed=true;ended=true;}
        while(!ended && io_ok && runtime.now()<phase_end) {
          connection->poll(runtime.now());
          if(!connection->open()) {transport_failed=true;ended=true;}
          if(!ended && io_ok)runtime.idle();
        }
        connection->close();
        if(transport_failed) {inconclusive=true;stop=true;}
      }
      // Connection is closed before constructing the next phase.
      connection.reset();
      const char* result=transport_failed || stop || bad || !io_ok ? "inconclusive" : rejected ? "rejected" :
          !ack ? "inconclusive" : data ? "accepted_data" : "accepted_no_data";
      if(std::string_view(result)=="inconclusive")inconclusive=true;
      boost::json::array ah,dh;for(auto v:ack_sources)ah.push_back(v);for(auto v:data_sources)dh.push_back(v);
      io_ok=artifacts.event({{"type","phase_summary"},{"phase",phase},{"result",result},{"messages",messages},{"data",data},
          {"sampled_bodies",samples},{"sampled_bytes",sample_bytes},{"ack_sources_absent_1_2_3_unknown",std::move(ah)},
          {"data_sources_absent_1_2_3_unknown",std::move(dh)}},true) && io_ok;
      ++completed;
    }
    if(completed!=4)inconclusive=true;
    io_ok=artifacts.event({{"type","summary"},{"result",inconclusive || !io_ok?"inconclusive":"observed"},
        {"phases",completed},{"fill_readiness_claim",false}},true) && io_ok;
    out<<"artifact_directory="<<artifacts.directory.string()<<'\n';
    return inconclusive || !io_ok ? 1 : 0;
  } catch(...) {err<<"diagnostic_error: inconclusive\n";return 2;}
}
int run_tbank_market_sources(const CliOptions& o,const Factories& f,std::ostream& out,std::ostream& err) {
  const tbank_grpc::detail::MarketSourcesRuntime runtime{now_ns,
      []{std::this_thread::sleep_for(std::chrono::milliseconds{1});},
      [](std::uint64_t deadline) {
        net_detail::asio::io_context context;
        auto resolved=net_detail::resolve(context,host,"443",std::chrono::steady_clock::time_point{std::chrono::nanoseconds{deadline}});
        if(!resolved.ok || resolved.endpoints.empty())return std::string{};
        return resolved.endpoints.begin()->endpoint().address().to_string();
      }};
  return tbank_grpc::detail::run_market_sources(o,f,out,err,runtime);
}
} // namespace exchange_probe
