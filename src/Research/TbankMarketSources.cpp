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
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

namespace exchange_probe {
namespace {
using namespace tbank_grpc;
using Bytes = std::span<const std::byte>;
constexpr std::string_view host = "invest-public-api.tbank.ru";
constexpr std::uint64_t second = 1'000'000'000ULL, reserve = 65536;
std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::uint64_t realtime_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
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
    if (ec) return fail();
    auto pattern = (root / "tbank-market-sources-XXXXXX").string();
    std::vector<char> name(pattern.begin(),pattern.end()); name.push_back('\0');
    auto* dir = ::mkdtemp(name.data()); if (!dir) return fail();
    directory = dir;
    const std::array<const char*,3> names{"public-frames.bin","public-frames.index.jsonl","events.jsonl"};
    for (std::size_t i=0;i<3;++i) {
      fd_[i] = ::open((directory/names[i]).c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
      if (fd_[i]<0) return fail();
    }
    return true;
  }
  bool event(boost::json::object value, bool summary=false) {
    if (!ok_) return false;
    const auto line=boost::json::serialize(value)+'\n';
    if (line.size()>remaining(summary) || !write(fd_[2],line.data(),line.size())) return fail();
    used_+=line.size(); return true;
  }
  bool body(Bytes body, boost::json::object entry) {
    if (!ok_) return false;
    entry["offset"]=offset_;entry["bytes"]=body.size();
    const auto line=boost::json::serialize(entry)+'\n';
    if (body.size()+line.size()>remaining(false)) return fail();
    // Index entries refer only to complete borrowed bodies, even after an I/O failure.
    const auto raw_start=::lseek(fd_[0],0,SEEK_CUR), index_start=::lseek(fd_[1],0,SEEK_CUR);
    if (raw_start<0 || index_start<0 || !write(fd_[0],body.data(),body.size()) || !write(fd_[1],line.data(),line.size())) {
      if (raw_start>=0) { (void)::ftruncate(fd_[0],raw_start); (void)::lseek(fd_[0],raw_start,SEEK_SET); }
      if (index_start>=0) { (void)::ftruncate(fd_[1],index_start); (void)::lseek(fd_[1],index_start,SEEK_SET); }
      return fail();
    }
    offset_+=body.size(); used_+=body.size()+line.size(); return true;
  }
  bool ok() const { return ok_; }
  std::filesystem::path directory;
 private:
  bool fail() {ok_=false;return false;}
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
  bool ok_{true};
};
struct Samples {
  std::uint64_t count{}, bytes{};
  bool closed{};
  bool take(std::size_t size) {
    if (closed || count>=32 || size>384*1024-bytes) {closed=true;return false;}
    ++count;bytes+=size;return true;
  }
};
struct Group {
  bool ack{}, rejected{};
  std::uint64_t messages{},data{},setup_data{},queued{},ack_at{},first_data{},unknown_ack{};
  std::uint64_t source_absent{},source_present{},source_unknown{},source_mismatch{},uid_mismatch{};
  std::array<std::uint64_t,5> ack_sources{},data_sources{};
  std::array<std::uint64_t,12> ack_statuses{};
  Samples samples;
};
const char* channel(MarketKind kind) {return kind==MarketKind::Book?"book":kind==MarketKind::Trades?"trades":"other";}
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
boost::json::value time_value(std::uint64_t value) {return value?boost::json::value(value):boost::json::value(nullptr);}
} // namespace
int tbank_grpc::detail::run_market_sources(const CliOptions& o,const Factories& factories,
    std::ostream& out,std::ostream& err,const MarketSourcesRuntime& runtime) {
  if (o.command!=Command::Research || o.action!="tbank-market-sources" || o.engines!="nghttp2" || !factories.nghttp2 ||
      !o.env_file || o.env_file->empty() || !o.ca_file || o.ca_file->empty() ||
      o.ca_file->native().size()>4096 || o.ca_file->native().find('\0')!=std::string::npos ||
      !o.output_dir || o.output_dir->empty() || !valid_uid(o.symbol) || !o.duration_seconds ||
      !*o.duration_seconds || *o.duration_seconds>60 || !o.schedule_days || o.schedule_days>7 ||
      !o.overall_timeout_seconds || o.overall_timeout_seconds>120 ||
      o.limits.timeout<std::chrono::milliseconds{100} || o.limits.timeout>std::chrono::milliseconds{5000} ||
      o.limits.attempts!=1 || o.max_artifact_bytes<reserve || o.max_artifact_bytes>4194304 ||
      o.open_viewer || o.capture_pcap || o.capture_tls_keys || o.confirm_private || !runtime.now ||
      !runtime.resolve || !runtime.idle || !runtime.realtime_now) {
    err<<"configuration_error: invalid_tbank_market_sources_configuration\n";return 2;
  }
  try {
    Artifacts artifacts;
    if(!artifacts.create(*o.output_dir,o.max_artifact_bytes)) {err<<"artifact_error\n";return 2;}
    bool stop=false, inconclusive=false;
    const auto started=runtime.now(), realtime_started=runtime.realtime_now();
    const auto overall_end=add(started,o.overall_timeout_seconds*second);
    const auto schedule_end=add(realtime_started,o.schedule_days*86400ULL*second);
    const auto timeout=static_cast<std::uint64_t>(o.limits.timeout.count())*1'000'000ULL;
    auto event=[&](boost::json::object row,bool summary=false) {
      if (!artifacts.event(std::move(row),summary)) {stop=true;inconclusive=true;return false;}
      return true;
    };
    auto metadata=[&](std::string_view direction,std::string_view rpc,unsigned connection,unsigned source,
                      std::string_view kind,std::uint32_t stream,std::uint64_t recorded_at) {
      return boost::json::object{{"direction",direction},{"rpc",rpc},{"connection",connection},
          {"requested_source",source?boost::json::value(source):boost::json::value(nullptr)},
          {"channel",kind},{"stream_id",stream?boost::json::value(stream):boost::json::value(nullptr)},
          {"monotonic_ns",recorded_at}};
    };
    auto record=[&](Bytes body,boost::json::object row) {
      if (!artifacts.body(body,std::move(row))) {stop=true;inconclusive=true;return false;}
      return true;
    };
    std::string credential_error;
    auto credential=load_tbank_credential(*o.env_file,o.api_slot,credential_error);
    if(!credential) {inconclusive=true;stop=true;}
    std::string pinned;
    if(!stop) pinned=runtime.resolve(std::min(overall_end,add(runtime.now(),timeout)));
    if(!stop && (!numeric(pinned) || runtime.now()>=overall_end)) {inconclusive=true;stop=true;}
    event({{"type","run"},{"host",host},{"pinned_ip",pinned},{"uid",o.symbol},
        {"started_monotonic_ns",started},{"started_realtime_ns",realtime_started},
        {"schedule_to_realtime_ns",schedule_end},{"schedule_days",o.schedule_days},
        {"observation_seconds",*o.duration_seconds},{"production_source_policy","reject_source_other_than_1_static_only"}});
    Samples control_samples;
    InstrumentObservation instrument;
    unsigned reference_completed=0,market_queued=0;
    std::array<Group,4> groups;
    std::array<std::unique_ptr<Connection>,2> market;
    std::array<std::uint32_t,2> streams{};
    std::array<bool,2> market_http{};
    std::uint64_t common_start=0,common_end=0,observation_end=0;
    bool horizon_clipped=false;
    // One reference connection is retired before either market connection exists.
    if (!stop) {
      auto reference=factories.nghttp2();
      unsigned phase=0,body_count=0;
      std::uint32_t stream=0;
      bool complete=false,http_ok=false,grpc_ok=false,body_ok=false;
      boost::json::object details;
      std::string_view path=kInstrumentPath;
      auto fail=[&]{stop=true;inconclusive=true;};
      const EventSink sink=[&](const Event& e) {
        if (stop) return;
        // nghttp2 delivers only the current RPC and bounds Message bodies to 64 KiB.
        if (e.kind==EventKind::Headers || e.kind==EventKind::RpcComplete) {
          if (e.http_status_present)http_ok=e.http_status==200;
          if (e.grpc_status_present)grpc_ok=e.grpc_status==0;
          if ((e.http_status_present && !http_ok) || (e.grpc_status_present && !grpc_ok)) {
            event({{"type","reference_rejected_metadata"},{"rpc",path},{"stream_id",e.stream_id},
                {"kind",static_cast<unsigned>(e.kind)},
                {"http_status",e.http_status_present?boost::json::value(e.http_status):boost::json::value(nullptr)},
                {"grpc_status",e.grpc_status_present?boost::json::value(e.grpc_status):boost::json::value(nullptr)},
                {"monotonic_ns",e.monotonic_ns}});
            fail();return;
          }
        }
        if (e.kind==EventKind::Message) {
          const auto received_at=e.monotonic_ns;
          ++body_count;
          const bool sampled=control_samples.take(e.payload.size());
          if (sampled && !record(e.payload,metadata("response",path,0,0,"reference",e.stream_id,received_at)))return;
          if (body_count!=1) {fail();return;}
          if (phase==0)body_ok=inspect_instrument(e.payload,o.symbol,instrument,details);
          else if (phase==1)body_ok=inspect_trading_schedule(e.payload,instrument.exchange,details);
          else body_ok=inspect_trading_status(e.payload,o.symbol,&details);
          event({{"type","reference_body"},{"rpc",path},{"valid",body_ok},{"details",std::move(details)},
              {"sampled",sampled},{"monotonic_ns",received_at}});
          if (!body_ok)fail();
        } else if (e.kind==EventKind::RpcComplete) {
          complete=true;
          event({{"type","reference_complete"},{"rpc",path},{"stream_id",e.stream_id},
              {"http_status",e.http_status_present?boost::json::value(e.http_status):boost::json::value(nullptr)},
              {"grpc_status",e.grpc_status_present?boost::json::value(e.grpc_status):boost::json::value(nullptr)},
              {"body_count",body_count},{"body_valid",body_ok},{"monotonic_ns",e.monotonic_ns}});
          if (!http_ok || !grpc_ok || body_count!=1 || !body_ok)fail();
        } else if (e.kind==EventKind::Failure || e.kind==EventKind::Eof || e.kind==EventKind::Goaway ||
                   e.kind==EventKind::StreamReset) {
          event({{"type","reference_transport_stop"},{"rpc",path},{"stream_id",e.stream_id},
              {"kind",static_cast<unsigned>(e.kind)},{"stage",e.stage},{"local_status",e.local_status},
              {"http_status",e.http_status_present?boost::json::value(e.http_status):boost::json::value(nullptr)},
              {"grpc_status",e.grpc_status_present?boost::json::value(e.grpc_status):boost::json::value(nullptr)},
              {"monotonic_ns",e.monotonic_ns}});
          fail();
        }
      };
      const auto ca=o.ca_file->string();
      const auto deadline=std::min(overall_end,add(runtime.now(),timeout));
      const ConnectOptions config{.host=host,.pinned_ip=pinned,.ca_file=ca,.deadline_ns=deadline,
          .stop_requested=[&]{return stop || runtime.now()>=overall_end;}};
      if (!reference || !reference->connect(config,sink,{}) || stop || runtime.now()>=deadline || !same_ip(reference->peer_ip(),pinned))fail();
      for (phase=0;phase<3 && !stop;++phase) {
        complete=false;http_ok=false;grpc_ok=false;body_ok=false;body_count=0;details.clear();
        path=phase==0?kInstrumentPath:phase==1?kTradingSchedulesPath:kTradingStatusPath;
        const auto request=phase==0?instrument_request(o.symbol):phase==1?
            schedule_request(instrument.exchange,realtime_started,schedule_end):trading_status_request(o.symbol);
        const auto rpc_deadline=std::min(overall_end,add(runtime.now(),timeout));
        if (runtime.now()>=rpc_deadline || request.empty() ||
            !record(request,metadata("request",path,0,0,"reference",0,runtime.now()))) {fail();break;}
        stream=reference->begin_rpc(path,credential->token(),request,false,rpc_deadline);
        event({{"type","queued_request"},{"rpc",path},{"connection",0},{"stream_id",stream},
            {"queued",stream!=0},{"monotonic_ns",runtime.now()}});
        if (!stream) {fail();break;}
        while (!complete && !stop && runtime.now()<rpc_deadline) {
          reference->poll(runtime.now());
          if (!reference->open())fail();
          if (!complete && !stop)runtime.idle();
        }
        if (!complete)fail();
        if (!stop)++reference_completed;
        // Only the owner loop queues the next RPC, after poll() has returned.
      }
      if(reference)reference->close();
    }
    auto stop_transport=[&](unsigned connection,const Event& e) {
      event({{"type","transport_stop"},{"connection",connection},{"kind",static_cast<unsigned>(e.kind)},
          {"rpc",kMarketStreamPath},{"stream_id",e.stream_id},{"stage",e.stage},{"local_status",e.local_status},
          {"http_status",e.http_status_present?boost::json::value(e.http_status):boost::json::value(nullptr)},
          {"grpc_status",e.grpc_status_present?boost::json::value(e.grpc_status):boost::json::value(nullptr)},
          {"monotonic_ns",e.monotonic_ns}});
      stop=true;inconclusive=true;
    };
    auto market_sink=[&](unsigned index,const Event& e) {
      if (stop) return;
      const unsigned source=index?3:1, connection=index+1;
      if (e.kind==EventKind::Message) {
        const auto received_at=e.monotonic_ns;
        // Structural classification precedes capture. Financial payloads are decoded once, after capture.
        const auto envelope=market_envelope(e.payload);
        const bool selected=envelope.kind!=MarketKind::Other &&
            (envelope.result==MarketDecode::Ack || envelope.result==MarketDecode::Data);
        Group* group=selected?&groups[index*2+(envelope.kind==MarketKind::Trades?1:0)]:nullptr;
        auto& samples=group?group->samples:control_samples;
        const bool sampled=samples.take(e.payload.size());
        if (sampled && !record(e.payload,metadata("response",kMarketStreamPath,connection,source,
                                                channel(envelope.kind),e.stream_id,received_at)))return;
        auto observation=inspect_market_source(envelope,sampled);
        if (group) {
          ++group->messages;
          auto& histogram=observation.result==MarketDecode::Ack?group->ack_sources:group->data_sources;
          const auto bucket=!observation.source_present?0u:observation.source>=1 && observation.source<=3?
              static_cast<unsigned>(observation.source):4u;
          ++histogram[bucket];
          if (observation.source_present) {
            ++group->source_present;
            if (bucket==4)++group->source_unknown;
            if (observation.source!=source)++group->source_mismatch;
          } else ++group->source_absent;
          if (!observation.uid.empty() && observation.uid!=o.symbol) {++group->uid_mismatch;stop=true;inconclusive=true;}
          if (observation.result==MarketDecode::Ack) {
            if (!group->ack_at)group->ack_at=received_at;
            ++group->ack_statuses[observation.status<=10?static_cast<unsigned>(observation.status):11];
            if (observation.status==1)group->ack=true;
            else if (observation.status>=2 && observation.status<=10)group->rejected=true;
            else ++group->unknown_ack;
          } else if (observation.result==MarketDecode::Data) {
            ++group->data;if(!common_start)++group->setup_data;if(!group->first_data)group->first_data=received_at;
          }
        }
        if (observation.result==MarketDecode::MalformedSyntax || observation.result==MarketDecode::SchemaAnomaly) {
          stop=true;inconclusive=true;
        }
        if (sampled)event({{"type","message"},{"connection",connection},{"requested_source",source},
            {"channel",channel(envelope.kind)},{"decode",decode_name(observation.result)},
            {"uid",observation.uid},{"status",observation.status_present?boost::json::value(observation.status):boost::json::value(nullptr)},
            {"source",observation.source_present?boost::json::value(observation.source):boost::json::value(nullptr)},
            {"source_matches_request",observation.source_present && observation.source==source},
            {"depth",observation.depth_present?boost::json::value(observation.depth):boost::json::value(nullptr)},
            {"setup_data",common_start==0},{"monotonic_ns",received_at},{"details",std::move(observation.details)}});
      } else if (e.kind==EventKind::Headers) {
        if (e.http_status_present)market_http[index]=e.http_status==200;
        if ((e.http_status_present && e.http_status!=200) || (e.grpc_status_present && e.grpc_status!=0))
          stop_transport(connection,e);
      } else if (e.kind==EventKind::RpcComplete || e.kind==EventKind::Failure || e.kind==EventKind::Eof ||
                 e.kind==EventKind::Goaway || e.kind==EventKind::StreamReset)stop_transport(connection,e);
    };
    auto poll_market=[&] {
      for (unsigned i=0;i<2 && !stop;++i)if (market[i] && streams[i]) {
        market[i]->poll(runtime.now());
        if (!market[i]->open()) {stop=true;inconclusive=true;}
      }
    };
    const auto ca=o.ca_file->string();
    // Prepare both physical connections before either subscription can produce samples.
    for (unsigned i=0;i<2 && !stop;++i) {
      market[i]=factories.nghttp2();
      const auto deadline=std::min(overall_end,add(runtime.now(),timeout));
      const ConnectOptions config{.host=host,.pinned_ip=pinned,.ca_file=ca,.deadline_ns=deadline,
          .stop_requested=[&]{return stop || runtime.now()>=overall_end;}};
      if (!market[i] || !market[i]->connect(config,[&,i](const Event& e){market_sink(i,e);},poll_market) ||
          stop || runtime.now()>=deadline || !same_ip(market[i]->peer_ip(),pinned)) {
        stop=true;inconclusive=true;break;
      }
    }
    for (unsigned i=0;i<2 && !stop;++i) {
      const unsigned source=i?3:1;
      if (runtime.now()>=overall_end || !market[i]->open()) {stop=true;inconclusive=true;break;}
      const auto request=combined_market_request(o.symbol,source);
      if (!record(request,metadata("request",kMarketStreamPath,i+1,source,"book_and_trades",0,runtime.now())))break;
      streams[i]=market[i]->begin_rpc(kMarketStreamPath,credential->token(),request,true,overall_end);
      const auto queued=runtime.now();
      event({{"type","queued_request"},{"rpc",kMarketStreamPath},{"connection",i+1},{"requested_source",source},
          {"stream_id",streams[i]},{"queued",streams[i]!=0},{"monotonic_ns",queued}});
      if (!streams[i]) {stop=true;inconclusive=true;break;}
      ++market_queued;groups[i*2].queued=queued;groups[i*2+1].queued=queued;
    }
    if (!stop && market_queued==2) {
      common_start=runtime.now();
      const auto requested_end=add(common_start,*o.duration_seconds*second);
      observation_end=std::min(overall_end,requested_end);horizon_clipped=observation_end<requested_end;
      event({{"type","common_observation_start"},{"monotonic_ns",common_start},
          {"planned_end_monotonic_ns",observation_end},{"deadline_clipped",horizon_clipped}});
      while (!stop && runtime.now()<observation_end) {
        poll_market();
        if (!stop && runtime.now()<observation_end)runtime.idle();
      }
      common_end=runtime.now();
    }
    for(auto& connection:market)if(connection)connection->close();
    const bool complete=!stop && !horizon_clipped && common_start && common_end>=observation_end;
    if (!complete || reference_completed!=3 || market_queued!=2)inconclusive=true;
    for (unsigned i=0;i<4;++i) {
      const auto& group=groups[i];
      const char* result=!complete || !market_http[i/2] || group.unknown_ack?"inconclusive":group.rejected?"rejected":
          !group.ack?"inconclusive":group.data>group.setup_data?"accepted_data":"accepted_no_data";
      if (std::string_view(result)=="inconclusive")inconclusive=true;
      boost::json::array ah,dh,statuses;for(auto v:group.ack_sources)ah.push_back(v);for(auto v:group.data_sources)dh.push_back(v);
      for(auto v:group.ack_statuses)statuses.push_back(v);
      event({{"type","group_summary"},{"connection",i/2+1},{"requested_source",i/2?3:1},
          {"channel",i%2?"trades":"book"},{"result",result},{"messages",group.messages},{"data",group.data},
          {"setup_data",group.setup_data},{"common_data",group.data-group.setup_data},{"unknown_ack",group.unknown_ack},
          {"ack_statuses_0_through_10_unknown",std::move(statuses)},
          {"queued_monotonic_ns",time_value(group.queued)},{"ack_monotonic_ns",time_value(group.ack_at)},
          {"first_data_monotonic_ns",time_value(group.first_data)},{"source_present",group.source_present},
          {"source_absent",group.source_absent},{"source_unknown",group.source_unknown},
          {"source_mismatch",group.source_mismatch},{"uid_mismatch",group.uid_mismatch},
          {"sampled_bodies",group.samples.count},{"sampled_bytes",group.samples.bytes},
          {"ack_sources_absent_1_2_3_unknown",std::move(ah)},{"data_sources_absent_1_2_3_unknown",std::move(dh)}},true);
    }
    event({{"type","summary"},{"result",inconclusive || !artifacts.ok()?"inconclusive":"observed"},
        {"reference_rpcs",reference_completed},{"market_rpcs",market_queued},
        {"common_start_monotonic_ns",time_value(common_start)},{"common_end_monotonic_ns",time_value(common_end)},
        {"common_overlap_ns",common_start && common_end>=common_start?common_end-common_start:0},
        {"deadline_clipped",horizon_clipped},{"control_sampled_bodies",control_samples.count},
        {"control_sampled_bytes",control_samples.bytes},{"fill_readiness_claim",false}},true);
    out<<"artifact_directory="<<artifacts.directory.string()<<'\n';
    return inconclusive || !artifacts.ok()?1:0;
  } catch(...) {err<<"diagnostic_error: inconclusive\n";return 1;}
}
int run_tbank_market_sources(const CliOptions& o,const Factories& f,std::ostream& out,std::ostream& err) {
  const tbank_grpc::detail::MarketSourcesRuntime runtime{now_ns,
      []{std::this_thread::sleep_for(std::chrono::milliseconds{1});},
      [](std::uint64_t deadline) {
        net_detail::asio::io_context context;
        auto resolved=net_detail::resolve(context,host,"443",std::chrono::steady_clock::time_point{std::chrono::nanoseconds{deadline}});
        if(!resolved.ok || resolved.endpoints.empty())return std::string{};
        return resolved.endpoints.begin()->endpoint().address().to_string();
      },realtime_ns};
  return tbank_grpc::detail::run_market_sources(o,f,out,err,runtime);
}
} // namespace exchange_probe
