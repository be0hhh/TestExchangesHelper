#include "Cadence.hpp"
#include "NetCommon.hpp"
#include "exchange_probe/Net.hpp"
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/json.hpp>
#include <functional>

namespace exchange_probe::cadence {
namespace {
namespace asio=boost::asio;
namespace beast=boost::beast;
namespace ssl=asio::ssl;
namespace ws=beast::websocket;
using Socket=ws::stream<net_detail::TlsStream>;
using Clock=std::chrono::steady_clock;
constexpr const char* host="fstream.binance.com";
void close(Socket& socket) {
  boost::system::error_code ignored;
  beast::get_lowest_layer(socket).socket().cancel(ignored);
  beast::get_lowest_layer(socket).socket().close(ignored);
}
template<class Start>
bool operation(asio::io_context& io, Socket& socket, Clock::time_point deadline,
               Start start, std::string& error) {
  asio::steady_timer timer(io,deadline);
  boost::system::error_code result;
  bool complete=false, expired=false;
  timer.async_wait([&](auto ec) { if(!ec) { expired=true; close(socket); } });
  start([&](boost::system::error_code ec) {
    result=ec; complete=true; timer.cancel();
  });
  io.run(); io.restart();
  if(expired || !complete || result) {
    error=expired ? "connect_deadline" : result.message();
    return false;
  }
  return true;
}
}

void captureDirect(Lane& lane, Window& window) {
  bool signaled=false;
  auto ready=[&] { if(!signaled) { window.prepared.fetch_add(1); signaled=true; } };
  try {
    lane.records.reserve(laneRecordLimit);
    lane.parser="diagnostic Boost.JSON; timestamp at complete WS message callback";
    lane.endpoint=std::string("wss://")+host+lane.feed.path;
    lane.subscription="{\"method\":\"SUBSCRIBE\",\"params\":[\""+lane.feed.topic+"\"],\"id\":1}";
    asio::io_context io;
    ssl::context tls{ssl::context::tls_client};
    tls.set_default_verify_paths();
    Socket socket{io,tls};
    const auto deadline=Clock::now()+std::chrono::seconds(30);
    const auto proxy=proxy_for_host(host);
    if(!proxy.error.empty()) throw std::runtime_error(proxy.error);
    auto resolved=net_detail::resolve(io,proxy.enabled ? proxy.host : host,
                                     proxy.enabled ? proxy.port : "443",deadline);
    if(!resolved.ok) throw std::runtime_error(resolved.error);
    if(!net_detail::connect(io,beast::get_lowest_layer(socket),resolved.endpoints,deadline,lane.error) ||
       (proxy.enabled && !net_detail::establish_proxy_tunnel(io,beast::get_lowest_layer(socket),proxy,host,"443",deadline,lane.error)) ||
       !net_detail::configure_tls(socket.next_layer(),host,lane.error) ||
       !net_detail::tls_handshake(io,socket.next_layer(),deadline,lane.error))
      throw std::runtime_error(lane.error);
    beast::get_lowest_layer(socket).expires_never();
    socket.read_message_max(1024*1024);
    socket.set_option(ws::stream_base::timeout::suggested(beast::role_type::client));
    if(!operation(io,socket,deadline,[&](auto done){
      socket.async_handshake(host,lane.feed.path,std::move(done));
    },lane.error)) throw std::runtime_error(lane.error);
    if(!operation(io,socket,deadline,[&](auto done){
      socket.async_write(asio::buffer(lane.subscription),
          [done](auto ec,std::size_t){done(ec);});
    },lane.error)) throw std::runtime_error(lane.error);
    lane.status="connected";
    ready();
    socket.control_callback([&](ws::frame_type type,beast::string_view){
      if(type==ws::frame_type::ping) ++lane.control_pings;
    });
    asio::steady_timer watchdog(io);
    beast::flat_buffer buffer{1024*1024};
    bool ended=false;
    std::function<void()> tick, read;
    tick=[&] {
      watchdog.expires_after(std::chrono::milliseconds(50));
      watchdog.async_wait([&](auto ec){
        if(ec || ended) return;
        const auto start=window.start_ns.load(std::memory_order_acquire);
        if(window.stop.load() || (start && nowNs()>=start+window.duration_seconds*second)) {
          ended=true; close(socket);
        } else tick();
      });
    };
    read=[&] {
      socket.async_read(buffer,[&](auto ec,std::size_t) {
        const auto receive=nowNs();
        if(ec) {
          if(!ended) { ++lane.disconnects; lane.error="read:"+ec.message(); }
          ended=true; watchdog.cancel(); close(socket); return;
        }
        const auto start=window.start_ns.load(std::memory_order_acquire);
        const bool measured=start && receive>=start && receive<start+window.duration_seconds*second;
        auto payload=beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        Record row{};
        row.receive_ns=receive;
        bool control=false;
        if(!socket.got_binary() && decode(lane.feed,payload,row,control)) {
          row.publish_ns=nowNs();
          if(measured) ++lane.frames;
          append(lane,row,window);
        } else if(!control) {
          if(measured) ++lane.parse_errors;
          else ++lane.warmup_parse_errors;
          if(lane.error.empty()) lane.error="invalid_or_rejected_payload:"+payload.substr(0,256);
        }
        if(!ended) read();
      });
    };
    tick(); read(); io.run();
    lane.status=lane.overflow ? "incomplete" :
      lane.disconnects || lane.parse_errors || lane.warmup_parse_errors ? "degraded" :
      lane.records.empty() ? "no_data" : "observed";
  } catch(const std::exception& e) {
    lane.status="failed"; lane.error=e.what();
  }
  ready();
}
}
