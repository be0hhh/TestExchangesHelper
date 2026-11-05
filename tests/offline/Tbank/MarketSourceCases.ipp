// Included by LifecycleCases.cpp; uses its synthetic TempDir/options fixture.
#include "../../../src/Research/TbankMarketSources.hpp"
#include <algorithm>
#include <boost/json.hpp>
namespace {
constexpr std::string_view market_uid="962e2a95-02a9-4171-abd7-aa198dbe643a";
constexpr std::string_view market_section="moex_mrng_evng_e_wknd_dlr";
constexpr std::string_view market_calendar_section="MOEX_MRNG_EVNG_E_WKND_DLR";
struct MarketSimulation {
  unsigned created{},active{},max_active{},connects{},rpcs{},resolves{},mode{},market_polls[2]{};
  std::uint64_t now{1'000'000'000ULL},realtime{1'784'073'600'000'000'000ULL};
  bool unexpected{},inside_poll{},reference_closed{};
  std::vector<std::byte> expected_raw;
} market_sim;
void market_varint(std::vector<std::byte>& out,std::uint64_t value) {
  do { unsigned b=value&127u;value>>=7u;out.push_back(static_cast<std::byte>(b|(value?128u:0u))); }while(value);
}
void market_scalar(std::vector<std::byte>& out,unsigned tag,std::uint64_t value) {
  market_varint(out,tag*8);market_varint(out,value);
}
void market_nested(std::vector<std::byte>& out,unsigned tag,const std::vector<std::byte>& value) {
  market_varint(out,tag*8+2);market_varint(out,value.size());out.insert(out.end(),value.begin(),value.end());
}
void market_text(std::vector<std::byte>& out,unsigned tag,std::string_view value) {
  const auto* first=reinterpret_cast<const std::byte*>(value.data());
  market_nested(out,tag,{first,first+value.size()});
}
std::vector<std::byte> market_reference(unsigned phase) {
  std::vector<std::byte> row,body;
  if(phase==0) {
    market_text(row,2,"GAZP");market_text(row,3,"TQBR");market_text(row,15,market_section);
    market_text(row,25,market_sim.mode==10?uid:market_uid);
  } else if(phase==1) {
    market_text(row,1,market_sim.mode==11?"OTHER":market_calendar_section);
    std::vector<std::byte> day,date;
    market_scalar(date,1,1'784'073'600ULL);market_nested(day,1,date);market_scalar(day,2,0);
    market_nested(row,2,day);
    if(market_sim.mode==23)row.pop_back();
  } else {market_text(row,6,market_uid);market_scalar(row,2,5);}
  market_nested(body,1,row);return body;
}
std::vector<std::byte> market_message(unsigned connection,MarketKind kind,bool ack) {
  const bool book=kind==MarketKind::Book;const auto requested=connection==1?1u:3u;
  const bool reject=market_sim.mode==2 && connection==1 && !book;
  unsigned source=requested;
  bool source_present=true;
  if(market_sim.mode==17 && !ack) {
    if(connection==2)source=book?2:99;
    else if(!book)source_present=false;
  }
  std::vector<std::byte> row,body,envelope;
  if(ack) {
    const unsigned status=market_sim.mode==24 && connection==1 && book?0:
        market_sim.mode==26 && connection==2 && !book?99:reject?2:1;
    market_scalar(row,book?3:2,status);market_text(row,book?4:3,market_uid);
    if(book) {market_scalar(row,2,50);market_scalar(row,7,source);}
    market_nested(body,2,row);if(!book)market_scalar(body,3,source);
    market_nested(envelope,book?2:3,body);
  } else {
    if(book) {market_scalar(row,2,50);market_nested(row,6,{});}
    else {market_scalar(row,2,1);market_nested(row,3,{});market_scalar(row,4,1);market_nested(row,5,{});}
    market_text(row,book?9:6,market_uid);
    if(source_present)market_scalar(row,book?10:7,source);
    if(market_sim.mode==3)market_scalar(row,book?10:7,source);
    if(market_sim.mode==7)market_nested(row,100,std::vector<std::byte>(16*1024));
    market_nested(envelope,book?7:6,row);
    if(market_sim.mode==4)envelope.pop_back();
  }
  return envelope;
}
class MarketFakeConnection final:public Connection {
 public:
  MarketFakeConnection():connection_(market_sim.created++) {
    if(connection_ && !market_sim.reference_closed)market_sim.unexpected=true;
  }
  ~MarketFakeConnection() override {close();}
  bool connect(const ConnectOptions& o,EventSink sink,const std::function<void()>& service_other) override {
    ++market_sim.connects;sink_=std::move(sink);stopped_=o.stop_requested;
    if(o.host!="invest-public-api.tbank.ru" || o.pinned_ip!="192.0.2.1" || o.ca_file!="/synthetic/tbank-ca.pem" ||
        o.deadline_ns>market_sim.now+5'000'000'000ULL)market_sim.unexpected=true;
    if(market_sim.mode==5)return false;
    opened_=true;++market_sim.active;market_sim.max_active=std::max(market_sim.active,market_sim.max_active);
    // Both handshakes finish before a market RPC is queued.
    if(connection_==2 && service_other)service_other();
    if(connection_==2 && market_sim.mode==29) {
      Event failure;failure.kind=EventKind::Failure;failure.monotonic_ns=market_sim.now;sink_(failure);
    }
    return !(stopped_ && stopped_());
  }
  std::string peer_ip()const override{return market_sim.mode==6?"192.0.2.2":"192.0.2.1";}
  std::uint32_t begin_rpc(std::string_view path,std::string_view token,std::span<const std::byte> request,bool streaming,std::uint64_t deadline) override {
    ++market_sim.rpcs;
    if(pending_ || market_sim.inside_poll || token!=secret || (stopped_ && stopped_()))market_sim.unexpected=true;
    std::vector<std::byte> expected;
    if(connection_==0) {
      if(streaming || unary_>=3 || deadline>market_sim.now+5'000'000'000ULL)market_sim.unexpected=true;
      const auto expected_path=unary_==0?kInstrumentPath:unary_==1?kTradingSchedulesPath:kTradingStatusPath;
      if(path!=expected_path)market_sim.unexpected=true;
      expected=unary_==0?instrument_request(market_uid):unary_==1?
          schedule_request(market_section,market_sim.realtime,market_sim.realtime+7ULL*86400*1'000'000'000ULL):
          trading_status_request(market_uid);
      stream_=1+2*unary_;
    } else {
      if(!streaming || path!=kMarketStreamPath || market_sim.connects!=3 || market_sim.active!=2)market_sim.unexpected=true;
      expected=combined_market_request(market_uid,connection_==1?1:3);stream_=1;
    }
    if(!std::equal(expected.begin(),expected.end(),request.begin(),request.end()))market_sim.unexpected=true;
    market_sim.expected_raw.insert(market_sim.expected_raw.end(),request.begin(),request.end());
    pending_=true;return stream_;
  }
  void poll(std::uint64_t) override {
    if(!opened_)return;
    market_sim.inside_poll=true;
    if(connection_)++market_sim.market_polls[connection_-1];
    auto emit=[&](EventKind kind) {
      Event event;event.kind=kind;event.stream_id=stream_;event.monotonic_ns=market_sim.now;
      if(kind==EventKind::Failure) {event.stage="synthetic_failure";event.local_status=-17;}
      sink_(event);
    };
    auto message=[&](std::vector<std::byte> body) {
      market_sim.expected_raw.insert(market_sim.expected_raw.end(),body.begin(),body.end());
      Event event;event.kind=EventKind::Message;event.stream_id=stream_;
      event.monotonic_ns=market_sim.now;
      event.payload=body;sink_(event);
      // Borrowed payload lifetime ends at callback return; persistent evidence must be independent.
      std::fill(body.begin(),body.end(),std::byte{0xff});
    };
    if(pending_ && connection_==0) {
      Event header;header.kind=EventKind::Headers;header.stream_id=stream_;
      header.monotonic_ns=market_sim.now;
      header.http_status_present=market_sim.mode!=15;header.http_status=market_sim.mode==21?503:200;sink_(header);
      if(market_sim.mode==16)emit(EventKind::Failure);
      else {
        if(market_sim.mode!=13) {
          auto body=market_reference(unary_);
          if(market_sim.mode==28 && unary_==2) {
            const auto duplicate=body;body.insert(body.end(),duplicate.begin(),duplicate.end());
          }
          message(std::move(body));
        }
        if(market_sim.mode==14)message(market_reference(unary_));
        Event complete;complete.kind=EventKind::RpcComplete;complete.stream_id=stream_;
        complete.monotonic_ns=market_sim.now;
        complete.grpc_status_present=market_sim.mode!=12;complete.grpc_status=market_sim.mode==22?7:0;
        // The transport clears its current RPC only after this callback returns.
        sink_(complete);
      }
      pending_=false;++unary_;
    } else if(pending_ && connection_) {
      pending_=false;
      if(market_sim.mode==18 && connection_==1)emit(EventKind::Failure);
      else {
        if(market_sim.mode!=27) {
          Event header;header.kind=EventKind::Headers;header.stream_id=stream_;
          header.http_status_present=true;header.http_status=200;sink_(header);
        }
        for(auto kind:{MarketKind::Book,MarketKind::Trades}) {
          if(!(market_sim.mode==19 && connection_==2 && kind==MarketKind::Trades))message(market_message(connection_,kind,true));
          if(market_sim.mode!=1 && !(market_sim.mode==2 && connection_==1 && kind==MarketKind::Trades)) {
            const unsigned count=market_sim.mode==7?40:1;
            for(unsigned n=0;n<count;++n) {
              if(stopped_ && stopped_())break;
              message(market_message(connection_,kind,false));
            }
          }
          if(stopped_ && stopped_())break;
        }
      }
    }
    market_sim.inside_poll=false;
  }
  bool open()const override{return opened_;}
  void close()noexcept override {
    if(opened_) {opened_=false;--market_sim.active;}
    if(connection_==0)market_sim.reference_closed=true;
  }
 private:
  unsigned connection_,unary_{};std::uint32_t stream_{};
  EventSink sink_;std::function<bool()> stopped_;bool opened_{},pending_{};
};
std::unique_ptr<Connection> market_factory(){return std::make_unique<MarketFakeConnection>();}
void market_sources_scenarios() {
  for(unsigned mode=0;mode<30;++mode) {
    TempDir temp;auto o=options(temp);o.action="tbank-market-sources";o.engines="nghttp2";o.symbol=market_uid;
    if(mode==8)o.max_artifact_bytes=65536;
    if(mode==25)o.max_artifact_bytes=67000;
    if(mode==20) {o.duration_seconds=2;o.overall_timeout_seconds=1;}
    std::ofstream(*o.env_file)<<"TBANK_API_1_API="<<secret<<'\n';
    market_sim={};market_sim.mode=mode;
    const detail::MarketSourcesRuntime runtime{[]{return market_sim.now;},[]{market_sim.now+=100'000'000ULL;},
      [](std::uint64_t){++market_sim.resolves;if(market_sim.mode==9)market_sim.now+=20'000'000'000ULL;return std::string{"192.0.2.1"};},
      []{return market_sim.realtime;}};
    std::ostringstream out,err;
    const int result=detail::run_market_sources(o,{.nghttp2=market_factory},out,err,runtime);
    CXET_CHECK(!market_sim.unexpected && market_sim.resolves==1 && market_sim.max_active<=2 && market_sim.active==0);
    CXET_CHECK(market_sim.created<=3 && market_sim.connects<=3 && market_sim.rpcs<=5);
    const bool success=mode==0 || mode==1 || mode==2 || mode==7 || mode==17;
    CXET_CHECK(result==(success?0:1));
    if(success)CXET_CHECK(market_sim.rpcs==5 && market_sim.connects==3 && market_sim.max_active==2);
    if(mode==5 || mode==6)CXET_CHECK(market_sim.connects==1 && market_sim.rpcs==0);
    if(mode==8 || mode==9)CXET_CHECK(market_sim.connects==0 && market_sim.rpcs==0);
    if((mode>=10 && mode<=16) || mode==21 || mode==22 || mode==23 || mode==28)CXET_CHECK(market_sim.connects==1);
    if(mode==18)CXET_CHECK(market_sim.rpcs==5 && market_sim.market_polls[1]==0);
    if(mode==29)CXET_CHECK(market_sim.rpcs==3 && market_sim.market_polls[0]==0 && market_sim.market_polls[1]==0);
    std::string events,index;std::vector<std::byte> raw;std::uintmax_t total=0;
    for(const auto& directory:std::filesystem::directory_iterator(*o.output_dir)) {
      CXET_CHECK((std::filesystem::status(directory.path()).permissions()&std::filesystem::perms::all)==std::filesystem::perms::owner_all);
      for(const auto& file:std::filesystem::directory_iterator(directory.path())) {
        total+=file.file_size();std::ifstream stream(file.path(),std::ios::binary);
        std::string contents{std::istreambuf_iterator<char>(stream),{}};
        CXET_CHECK(contents.find(secret)==std::string::npos);
        CXET_CHECK((std::filesystem::status(file.path()).permissions()&std::filesystem::perms::all)==
                   (std::filesystem::perms::owner_read|std::filesystem::perms::owner_write));
        if(file.path().filename()=="events.jsonl")events=contents;
        if(file.path().filename()=="public-frames.index.jsonl")index=contents;
        if(file.path().filename()=="public-frames.bin")for(unsigned char b:contents)raw.push_back(static_cast<std::byte>(b));
      }
    }
    CXET_CHECK(total<=o.max_artifact_bytes);
    if(mode==0 || mode==1 || mode==2 || mode==17)CXET_CHECK(raw==market_sim.expected_raw);
    if(success) {
      CXET_CHECK(events.find("\"reference_rpcs\":3")!=std::string::npos);
      CXET_CHECK(events.find("\"market_rpcs\":2")!=std::string::npos);
      CXET_CHECK(events.find("\"setup_data\":true")==std::string::npos);
      CXET_CHECK(events.find("\"setup_data\":false")!=std::string::npos);
      CXET_CHECK(events.find("common_observation_start")!=std::string::npos);
      CXET_CHECK(index.find("\"direction\":\"request\"")!=std::string::npos);
      CXET_CHECK(index.find("\"direction\":\"response\"")!=std::string::npos);
    }
    if(mode==1)CXET_CHECK(events.find("accepted_no_data")!=std::string::npos);
    if(mode==2)CXET_CHECK(events.find("rejected")!=std::string::npos && events.find("accepted_data")!=std::string::npos);
    if(mode==3)CXET_CHECK(events.find("schema_anomaly")!=std::string::npos);
    if(mode==4)CXET_CHECK(events.find("malformed_syntax")!=std::string::npos);
    if(mode==8)CXET_CHECK(events.empty() && raw.empty() && index.empty()); // failed run event stays latched through summary
    if(mode==17)CXET_CHECK(events.find("\"source\":2")!=std::string::npos && events.find("\"source\":99")!=std::string::npos);
    if(mode==16 || mode==18)CXET_CHECK(events.find("\"stage\":\"synthetic_failure\"")!=std::string::npos &&
                                     events.find("\"local_status\":-17")!=std::string::npos);
    if(mode==21)CXET_CHECK(events.find("reference_rejected_metadata")!=std::string::npos &&
                          events.find("\"http_status\":503")!=std::string::npos);
    if(mode==22)CXET_CHECK(events.find("reference_rejected_metadata")!=std::string::npos &&
                          events.find("\"grpc_status\":7")!=std::string::npos);
    if(mode==20)CXET_CHECK(events.find("\"deadline_clipped\":true")!=std::string::npos);
    std::istringstream rows(events);std::string line;unsigned summaries=0;
    while(std::getline(rows,line)) {
      const auto value=boost::json::parse(line).as_object();
      if(value.at("type").as_string()=="group_summary") {
        ++summaries;
        CXET_CHECK(value.at("sampled_bodies").to_number<std::uint64_t>()<=32);
        CXET_CHECK(value.at("sampled_bytes").to_number<std::uint64_t>()<=384*1024);
        if(mode==7) {
          CXET_CHECK(value.at("setup_data").to_number<std::uint64_t>()==0);
          CXET_CHECK(value.at("common_data").to_number<std::uint64_t>()==40);
          CXET_CHECK(value.at("sampled_bodies").to_number<std::uint64_t>()>0);
        }
      }
    }
    if(mode==8 || mode==25)CXET_CHECK(summaries==0);else CXET_CHECK(summaries==4);
    if(mode==24 || mode==26) {
      CXET_CHECK(market_sim.rpcs==5 && market_sim.now>=2'000'000'000ULL);
      CXET_CHECK(events.find("accepted_data")!=std::string::npos && events.find("inconclusive")!=std::string::npos);
      CXET_CHECK(events.find("rejected")==std::string::npos);
    }
    CXET_CHECK(out.str().find(secret)==std::string::npos && err.str().find(secret)==std::string::npos);
  }
}
}
