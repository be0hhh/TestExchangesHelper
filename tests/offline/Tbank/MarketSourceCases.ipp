// Included by LifecycleCases.cpp; uses its synthetic TempDir/options fixture.
#include "../../../src/Research/TbankMarketSources.hpp"
#include <algorithm>
namespace {
struct MarketSimulation {
  unsigned created{},active{},max_active{},connects{},rpcs{},resolves{},mode{};
  std::uint64_t now{1'000'000'000ULL};
  bool unexpected{};
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
std::vector<std::byte> market_message(unsigned phase,bool ack) {
  const bool book=phase<2;const auto source=phase%2?3u:1u;
  std::vector<std::byte> row,body,envelope;
  const std::vector<std::byte> uid_bytes(reinterpret_cast<const std::byte*>(uid.data()),reinterpret_cast<const std::byte*>(uid.data()+uid.size()));
  if(ack) {
    market_scalar(row,book?3:2,market_sim.mode==2?2:1);
    market_nested(row,book?4:3,uid_bytes);
    if(book) {market_scalar(row,2,50);market_scalar(row,7,source);}
    market_nested(body,2,row);if(!book)market_scalar(body,3,source);
    market_nested(envelope,book?2:3,body);
  } else {
    if(book) {market_scalar(row,2,50);market_nested(row,6,{});}
    else {market_scalar(row,2,1);market_nested(row,3,{});market_scalar(row,4,1);market_nested(row,5,{});}
    market_nested(row,book?9:6,uid_bytes);market_scalar(row,book?10:7,source);
    if(market_sim.mode==3)market_scalar(row,book?10:7,source); // duplicate selected singular
    if(market_sim.mode==7)market_nested(row,100,std::vector<std::byte>(16*1024));
    market_nested(envelope,book?7:6,row);
    if(market_sim.mode==4)envelope.pop_back();
  }
  return envelope;
}
class MarketFakeConnection final:public Connection {
 public:
  MarketFakeConnection():phase_(market_sim.created++) {++market_sim.active;market_sim.max_active=std::max(market_sim.active,market_sim.max_active);}
  ~MarketFakeConnection() override {close();}
  bool connect(const ConnectOptions& o,EventSink sink,const std::function<void()>&) override {
    ++market_sim.connects;sink_=std::move(sink);
    if(o.host!="invest-public-api.tbank.ru" || o.pinned_ip!="192.0.2.1" || o.ca_file!="/synthetic/tbank-ca.pem")market_sim.unexpected=true;
    if(market_sim.mode==5)return false;
    opened_=true;return true;
  }
  std::string peer_ip()const override{return market_sim.mode==6?"192.0.2.2":"192.0.2.1";}
  std::uint32_t begin_rpc(std::string_view path,std::string_view token,std::span<const std::byte> request,bool streaming,std::uint64_t) override {
    ++market_sim.rpcs;
    const auto expected=market_source_request(phase_<2?MarketKind::Book:MarketKind::Trades,phase_%2?3:1);
    if(path!=kMarketStreamPath || token!=secret || !streaming || !std::equal(expected.begin(),expected.end(),request.begin(),request.end()))market_sim.unexpected=true;
    return 1;
  }
  void poll(std::uint64_t) override {
    if(sent_)return;sent_=true;
    auto send=[&](bool ack) {
      auto body=market_message(phase_,ack);
      // Saved complete bytes must survive reuse of the callback's backing buffer.
      if(market_sim.mode!=7)market_sim.expected_raw.insert(market_sim.expected_raw.end(),body.begin(),body.end());
      Event event;event.kind=EventKind::Message;event.payload=body;sink_(event);
      std::fill(body.begin(),body.end(),std::byte{0xff});
    };
    send(true);
    if(market_sim.mode!=1 && market_sim.mode!=2) {
      const auto count=market_sim.mode==7?40:1;for(int i=0;i<count;++i)send(false);
    }
  }
  bool open()const override{return opened_;}
  void close()noexcept override{opened_=false;if(!closed_){closed_=true;--market_sim.active;}}
 private:unsigned phase_;EventSink sink_;bool opened_{},sent_{},closed_{};
};
std::unique_ptr<Connection> market_factory(){return std::make_unique<MarketFakeConnection>();}
void market_sources_scenarios() {
  for(unsigned mode=0;mode<10;++mode) {
    TempDir temp;auto o=options(temp);o.action="tbank-market-sources";o.engines="nghttp2";
    if(mode==8)o.max_artifact_bytes=65536; // summary reserve leaves no sample space
    std::ofstream(*o.env_file)<<"TBANK_API_1_API="<<secret<<'\n';
    market_sim={};market_sim.mode=mode;
    const detail::MarketSourcesRuntime runtime{[]{return market_sim.now;},[]{market_sim.now+=100'000'000ULL;},
      [](std::uint64_t){++market_sim.resolves;if(market_sim.mode==9)market_sim.now+=20'000'000'000ULL;return std::string{"192.0.2.1"};}};
    std::ostringstream out,err;
    const int result=detail::run_market_sources(o,{.nghttp2=market_factory},out,err,runtime);
    CXET_CHECK(!market_sim.unexpected && market_sim.resolves==1 && market_sim.max_active<=1 && market_sim.active==0);
    CXET_CHECK(market_sim.connects<=4 && market_sim.rpcs<=4);
    if(mode==5 || mode==6)CXET_CHECK(result==1 && market_sim.connects==1);
    if(mode==9)CXET_CHECK(result==1 && market_sim.connects==0);
    if(mode==0 || mode==1 || mode==2 || mode==7)CXET_CHECK(result==0 && market_sim.rpcs==4);
    if(mode==3 || mode==4 || mode==8)CXET_CHECK(result==1);
    std::string events;std::vector<std::byte> raw;std::uintmax_t total=0;
    for(const auto& directory:std::filesystem::directory_iterator(*o.output_dir)) {
      for(const auto& file:std::filesystem::directory_iterator(directory.path())) {
        total+=file.file_size();std::ifstream stream(file.path(),std::ios::binary);
        std::string contents{std::istreambuf_iterator<char>(stream),{}};
        CXET_CHECK(contents.find(secret)==std::string::npos);
        if(file.path().filename()=="events.jsonl")events=contents;
        if(file.path().filename()=="public-frames.bin")for(unsigned char b:contents)raw.push_back(static_cast<std::byte>(b));
      }
    }
    CXET_CHECK(total<=o.max_artifact_bytes);
    if(mode<5)CXET_CHECK(raw==market_sim.expected_raw);
    if(mode==1)CXET_CHECK(events.find("accepted_no_data")!=std::string::npos);
    if(mode==2)CXET_CHECK(events.find("rejected")!=std::string::npos);
    if(mode==3)CXET_CHECK(events.find("schema_anomaly")!=std::string::npos);
    if(mode==4)CXET_CHECK(events.find("malformed_syntax")!=std::string::npos);
    if(mode==7)CXET_CHECK(raw.size()<=4*384*1024);
    CXET_CHECK(out.str().find(secret)==std::string::npos && err.str().find(secret)==std::string::npos);
  }
}
}
