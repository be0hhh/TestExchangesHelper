#include "RaceDashboard.hpp"

#include "RaceSvg.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace exchange_probe::race {
namespace {

[[nodiscard]] std::string feed_name(const std::string& feedId) {
  const auto slash = feedId.find('/');
  const auto suffix = slash == std::string::npos
                          ? feedId
                          : feedId.substr(slash + 1u);
  if (suffix == "orderbook_1") return "orderbook.1 - direct BBO";
  if (suffix == "orderbook_50") return "orderbook.50 - derived BBO";
  if (suffix == "orderbook_200") return "orderbook.200 - derived BBO";
  if (suffix == "orderbook_1000") return "orderbook.1000 - derived BBO";
  if (suffix == "public_trade") return "publicTrade - separate activity";
  return suffix;
}

[[nodiscard]] std::string source_label(
    const AnalysisResult& analysis, std::uint32_t sourceId,
    std::uint16_t raceGroupId) {
  for (const auto& descriptor : analysis.sourceDescriptors) {
    if (descriptor.source.sourceId == sourceId &&
        descriptor.source.raceGroupId == raceGroupId) {
      return feed_name(descriptor.feedId);
    }
  }
  return "source " + std::to_string(sourceId);
}

[[nodiscard]] long double microseconds(std::int64_t nanoseconds) noexcept {
  return static_cast<long double>(nanoseconds) / 1'000.0L;
}

[[nodiscard]] std::string script_safe_json(const std::string& json) {
  std::string output;
  output.reserve(json.size());
  for (const char byte : json) {
    if (byte == '<')
      output += "\\u003c";
    else if (byte == '>')
      output += "\\u003e";
    else if (byte == '&')
      output += "\\u0026";
    else
      output += byte;
  }
  return output;
}

[[nodiscard]] boost::json::object distribution_json(
    const DistributionSummary& distribution) {
  return {
      {"samples", distribution.samples},
      {"p50_us", static_cast<double>(microseconds(distribution.medianNs))},
      {"p95_us", static_cast<double>(microseconds(distribution.p95Ns))},
      {"p99_us", static_cast<double>(microseconds(distribution.p99Ns))},
      {"max_us", static_cast<double>(microseconds(distribution.maximumNs))},
  };
}

[[nodiscard]] boost::json::object race_json(
    const AnalysisResult& analysis, const MultiwayRaceStatistics& race) {
  boost::json::array lanes;
  for (const auto& lane : race.lanes) {
    lanes.emplace_back(boost::json::object{
        {"source_id", lane.sourceId},
        {"name", feed_name(lane.feedId)},
        {"wins", lane.wins},
        {"ties", lane.tiedFirst},
        {"eligible", lane.eligibleEvents},
        {"arrival", distribution_json(lane.arrivalAfterWinner)},
        {"winning_lead", distribution_json(lane.winningLead)},
    });
  }
  boost::json::array events;
  for (const auto& event : race.eventSamples) {
    boost::json::array deltas;
    for (const auto value : event.arrivalAfterWinnerNs)
      deltas.emplace_back(static_cast<double>(microseconds(value)));
    events.emplace_back(boost::json::object{
        {"session", event.sessionId},
        {"sequence", event.sequence},
        {"offset_ms", static_cast<double>(event.offsetFromMeasuredStartNs) /
                          1'000'000.0},
        {"bid", event.bbo.bidPrice},
        {"ask", event.bbo.askPrice},
        {"first_mask", event.firstLaneMask},
        {"deltas_us", std::move(deltas)},
    });
  }
  boost::json::array rolling;
  for (const auto& window : analysis.rollingWindows) {
    if (window.raceGroupId != race.raceGroupId) continue;
    boost::json::array wins;
    for (const auto& lane : window.lanes) wins.emplace_back(lane.wins);
    rolling.emplace_back(boost::json::object{
        {"session", window.sessionId},
        {"start_s", static_cast<double>(window.startOffsetNs) /
                        1'000'000'000.0},
        {"end_s", static_cast<double>(window.endOffsetNs) /
                      1'000'000'000.0},
        {"matched", window.matchedAll},
        {"leader", window.leaderLane == kNoBboRaceLane
                       ? -1
                       : static_cast<int>(window.leaderLane)},
        {"wins", std::move(wins)},
    });
  }
  boost::json::array pairwise;
  for (const auto& pair : analysis.pooledPairs) {
    if (pair.venue != Venue::Bybit ||
        pair.eventClass != EventClass::Bbo ||
        pair.raceGroupId != race.raceGroupId) {
      continue;
    }
    pairwise.emplace_back(boost::json::object{
        {"a", source_label(analysis, pair.sourceA, pair.raceGroupId)},
        {"b", source_label(analysis, pair.sourceB, pair.raceGroupId)},
        {"matched", pair.matched},
        {"a_first", pair.aFirst},
        {"b_first", pair.bFirst},
        {"ties", pair.ties},
        {"median_us", static_cast<double>(
                          microseconds(pair.deltaRecvANsMinusB.medianNs))},
        {"p99_us", static_cast<double>(
                       microseconds(pair.deltaRecvANsMinusB.p99Ns))},
    });
  }
  std::uint64_t tradeRecords = 0u;
  std::uint64_t tradeInvalid = 0u;
  for (const auto& source : analysis.sources) {
    if (source.source.venue == Venue::Bybit &&
        source.source.raceGroupId == race.raceGroupId &&
        source.eventClass == EventClass::Trade) {
      tradeRecords += source.records;
      tradeInvalid += source.staleOrInvalid;
    }
  }
  boost::json::object recommendation{
      {"status", "insufficient_evidence"},
      {"reason", "missing_strict_four_feed_evidence"},
      {"primary", ""},
      {"complement", ""},
  };
  for (const auto& candidate : analysis.recommendations) {
    if (candidate.raceGroupId != race.raceGroupId ||
        candidate.symbol != race.symbol) {
      continue;
    }
    recommendation["status"] =
        candidate.status == RecommendationStatus::ObservedCandidate
            ? "observed_candidate"
            : "insufficient_evidence";
    recommendation["reason"] = candidate.reason;
    recommendation["primary"] = source_label(
        analysis, candidate.primarySourceId, candidate.raceGroupId);
    recommendation["complement"] = source_label(
        analysis, candidate.complementSourceId, candidate.raceGroupId);
  }
  return {
      {"symbol", race.symbol},
      {"group", race.raceGroupId},
      {"union_events", race.unionEvents},
      {"matched_all", race.matchedAll},
      {"excluded_partial", race.excludedPartial},
      {"tie_events", race.tieEvents},
      {"health_clean", race.healthClean},
      {"lanes", std::move(lanes)},
      {"events", std::move(events)},
      {"rolling", std::move(rolling)},
      {"pairwise", std::move(pairwise)},
      {"trades", boost::json::object{
          {"records", tradeRecords}, {"invalid", tradeInvalid}}},
      {"recommendation", std::move(recommendation)},
  };
}

[[nodiscard]] bool write_main_svgs(
    const AnalysisResult& analysis, const std::filesystem::path& plots,
    std::string& error) {
  std::vector<std::pair<std::string, long double>> podium;
  std::vector<std::pair<std::string, long double>> coverage;
  std::vector<std::pair<std::string, long double>> rolling;
  std::vector<std::pair<std::string, long double>> pairwise;
  if (!analysis.pooledMultiway.empty()) {
    const auto& race = analysis.pooledMultiway.front();
    for (const auto& lane : race.lanes)
      podium.emplace_back(feed_name(lane.feedId), lane.wins);
    const auto coveragePercent = race.unionEvents == 0u
        ? 0.0L
        : 100.0L * static_cast<long double>(race.matchedAll) /
              static_cast<long double>(race.unionEvents);
    coverage.emplace_back("strict 4/4 coverage", coveragePercent);
    coverage.emplace_back("health clean", race.healthClean ? 100.0L : 0.0L);
    std::array<std::uint64_t, kBybitBboRaceLaneCount> ledWindows{};
    for (const auto& window : analysis.rollingWindows) {
      if (window.raceGroupId == race.raceGroupId &&
          window.leaderLane < kBybitBboRaceLaneCount) {
        ++ledWindows[window.leaderLane];
      }
    }
    for (std::size_t lane = 0u; lane < race.lanes.size(); ++lane)
      rolling.emplace_back(feed_name(race.lanes[lane].feedId),
                           ledWindows[lane]);
    for (const auto& pair : analysis.pooledPairs) {
      if (pair.venue == Venue::Bybit && pair.eventClass == EventClass::Bbo &&
          pair.raceGroupId == race.raceGroupId) {
        pairwise.emplace_back(
            source_label(analysis, pair.sourceA, pair.raceGroupId) + " vs " +
                source_label(analysis, pair.sourceB, pair.raceGroupId),
            microseconds(pair.deltaRecvANsMinusB.medianNs));
      }
    }
  }
  return svg::write_bars(
             plots / "race_overview.svg", "Strict 4/4 BBO podium",
             "wins; partial events excluded", podium, error) &&
         svg::write_bars(
             plots / "rolling_leader.svg", "Rolling leader stability",
             "number of uniquely led windows", rolling, error) &&
         svg::write_bars(
             plots / "coverage_health.svg", "Coverage and capture health",
             "percent", coverage, error) &&
         svg::write_bars(
             plots / "pairwise_matrix.svg", "Bybit BBO pairwise matrix",
             "median recv_A - recv_B, microseconds", pairwise, error);
}

}  // namespace

bool write_race_dashboard(
    const AnalysisResult& analysis, const std::filesystem::path& directory,
    std::string& error) {
  if (!write_main_svgs(analysis, directory / "plots", error)) return false;
  boost::json::array races;
  for (const auto& race : analysis.pooledMultiway)
    races.emplace_back(race_json(analysis, race));
  const auto data = script_safe_json(boost::json::serialize(
      boost::json::object{
          {"schema", "exchange.feed_race.dashboard"},
          {"schema_version", 1},
          {"races", std::move(races)},
      }));
  std::ofstream output{directory / "dashboard.html", std::ios::binary};
  if (!output) {
    error = "dashboard_open_failed";
    return false;
  }
  output << R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bybit BBO feed race</title><style>
:root{color-scheme:dark;--bg:#07101d;--panel:#101c2d;--line:#263a55;--text:#edf4ff;--muted:#94a8c2;--cyan:#48d9cf;--amber:#ffbe55;--red:#ff7185;--blue:#6aa8ff}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,#132642 0,var(--bg) 42%);color:var(--text);font:14px system-ui,sans-serif}.page{max-width:1440px;margin:auto;padding:28px}.top{display:flex;gap:18px;align-items:end;justify-content:space-between;flex-wrap:wrap}h1{font-size:30px;margin:0 0 7px}.sub,.muted{color:var(--muted)}select,button{background:#15263b;color:var(--text);border:1px solid var(--line);border-radius:8px;padding:9px 12px}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px;margin-top:16px}.card{background:linear-gradient(145deg,#122137,#0d1828);border:1px solid var(--line);border-radius:14px;padding:16px;min-width:0}.stat{grid-column:span 3}.stat b{display:block;font-size:25px;margin-top:7px}.wide{grid-column:span 8}.side{grid-column:span 4}.full{grid-column:1/-1}h2{font-size:17px;margin:0 0 12px}.lane{display:grid;grid-template-columns:minmax(210px,1.5fr) 3fr 95px 120px;gap:12px;align-items:center;padding:11px 0;border-top:1px solid #20334d}.lane:first-child{border-top:0}.track{height:10px;background:#07111e;border-radius:8px;overflow:visible;position:relative}.dot{position:absolute;top:-5px;width:20px;height:20px;border:3px solid #dff;border-radius:50%;background:var(--cyan);transform:translateX(-50%)}.winner .dot{box-shadow:0 0 16px var(--amber);background:var(--amber)}.podium{display:grid;gap:8px}.podium div{display:flex;justify-content:space-between;background:#0a1524;padding:10px;border-radius:8px}.health-good{color:#5ee6a8}.health-bad{color:var(--red)}input[type=range]{width:100%}.event-head{display:flex;justify-content:space-between;gap:12px;margin-bottom:12px}.roll{display:flex;gap:4px;overflow-x:auto;padding-bottom:5px}.window{min-width:74px;padding:8px 6px;background:#0a1524;border:1px solid var(--line);border-radius:7px;text-align:center}.window.tie{color:var(--muted)}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{text-align:left;padding:9px;border-bottom:1px solid #20334d}th{color:var(--muted);font-weight:600}.note{border-left:3px solid var(--blue);padding-left:12px;line-height:1.5}@media(max-width:900px){.stat{grid-column:span 6}.wide,.side{grid-column:1/-1}.lane{grid-template-columns:1fr 2fr 70px}.lane .tail{display:none}}@media(max-width:560px){.page{padding:16px}.stat{grid-column:1/-1}.lane{grid-template-columns:1fr}.track{margin:5px 10px}}
</style></head><body><main class="page"><div class="top"><div><h1>Bybit BBO feed race</h1><div class="sub">One market transition, four independent orderbook feeds, local receive timestamp.</div></div><div><label for="raceSelect" class="muted">Market </label><select id="raceSelect"></select> <button id="png">Export race PNG</button></div></div><section id="root"></section></main>
<script id="race-data" type="application/json">)HTML"
         << data << R"HTML(</script><script>
const DATA=JSON.parse(document.getElementById('race-data').textContent);const COLORS=['#48d9cf','#ffbe55','#6aa8ff','#c487ff'];const $=s=>document.querySelector(s);const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));const pct=(a,b)=>b?`${(100*a/b).toFixed(1)}%`:'0.0%';const f=n=>Number(n||0).toFixed(3);
function draw(i){const r=DATA.races[i];if(!r){$('#root').innerHTML='<div class="card full" style="margin-top:16px">No strict four-feed Bybit race metadata. Analyze a new Bybit capture.</div>';return}const rec=r.recommendation,good=r.health_clean;$('#root').innerHTML=`<div class="grid"><div class="card stat"><span class="muted">Strict 4/4 events</span><b>${r.matched_all}</b></div><div class="card stat"><span class="muted">Coverage</span><b>${pct(r.matched_all,r.union_events)}</b></div><div class="card stat"><span class="muted">Excluded partial</span><b>${r.excluded_partial}</b></div><div class="card stat"><span class="muted">Capture health</span><b class="${good?'health-good':'health-bad'}">${good?'CLEAN':'DEGRADED'}</b></div><div class="card wide"><div class="event-head"><div><h2>Where is the race?</h2><div class="muted">Each dot is the same BBO change. Left = received first; distance = local delay after winner.</div></div><b id="eventLabel"></b></div><input id="event" type="range" min="0" max="${Math.max(0,r.events.length-1)}" value="0"><div id="lanes"></div><svg id="raceSvg" width="1200" height="520" viewBox="0 0 1200 520" style="display:none"></svg></div><div class="card side"><h2>Strict podium</h2><div class="podium">${[...r.lanes].sort((a,b)=>b.wins-a.wins).map((x,j)=>`<div><span>${j+1}. ${esc(x.name)}</span><b>${x.wins} wins</b></div>`).join('')}</div><p class="note"><b>${rec.status==='observed_candidate'?'Observed candidate':'Insufficient evidence'}</b><br>${esc(rec.reason)}${rec.primary?`<br>Primary: ${esc(rec.primary)}<br>Complement: ${esc(rec.complement)}`:''}</p></div><div class="card full"><h2>Rolling leader</h2><div class="muted">Leader is recomputed independently in each time window. A dash means tie/no unique winner.</div><div class="roll">${r.rolling.map(w=>`<div class="window ${w.leader<0?'tie':''}"><b>${w.leader<0?'—':esc(r.lanes[w.leader].name.split(' ')[0])}</b><br><small>s${w.session} ${f(w.start_s)}s</small><br><small>${w.matched} events</small></div>`).join('')||'<span class="muted">No rolling windows</span>'}</div></div><div class="card wide"><h2>Pairwise technical matrix</h2><div class="muted">Signed delta is recv_A - recv_B; negative means A arrived first.</div><div style="overflow:auto"><table><thead><tr><th>A</th><th>B</th><th>matched</th><th>A first</th><th>B first</th><th>ties</th><th>median us</th><th>p99 us</th></tr></thead><tbody>${r.pairwise.map(p=>`<tr><td>${esc(p.a)}</td><td>${esc(p.b)}</td><td>${p.matched}</td><td>${p.a_first}</td><td>${p.b_first}</td><td>${p.ties}</td><td>${f(p.median_us)}</td><td>${f(p.p99_us)}</td></tr>`).join('')}</tbody></table></div></div><div class="card side"><h2>publicTrade is separate</h2><b style="font-size:28px">${r.trades.records}</b> normalized trades<p class="muted">Not mixed into the BBO podium or coverage denominator. Invalid: ${r.trades.invalid}.</p><p class="note">The dashboard describes observed arrival order only. It does not prove exchange matcher latency or production profitability.</p></div></div>`;const range=$('#event');range.addEventListener('input',()=>eventView(r,+range.value));eventView(r,0)}
function eventView(r,n){const e=r.events[n];const lanes=$('#lanes');if(!e){$('#eventLabel').textContent='No sampled strict events';lanes.innerHTML='<p class="muted">No event can enter the podium until all four feeds report the same transition.</p>';syncSvg(r,null);return}$('#eventLabel').textContent=`event ${n+1}/${r.events.length} · s${e.session} · seq ${e.sequence}`;const max=Math.max(1,...e.deltas_us);lanes.innerHTML=r.lanes.map((x,i)=>`<div class="lane ${e.deltas_us[i]===0?'winner':''}"><b>${esc(x.name)}</b><div class="track"><span class="dot" style="left:${4+92*e.deltas_us[i]/max}%;background:${COLORS[i]}"></span></div><b>${f(e.deltas_us[i])} us</b><span class="tail muted">wins ${x.wins} · p99 ${f(x.arrival.p99_us)} us</span></div>`).join('');syncSvg(r,e)}
function syncSvg(r,e){const svg=$('#raceSvg');const deltas=e?e.deltas_us:[0,0,0,0],max=Math.max(1,...deltas);svg.innerHTML=`<rect width="1200" height="520" fill="#07101d"/><text x="50" y="55" fill="#edf4ff" font-size="30" font-family="sans-serif">${esc(r.symbol)} strict 4/4 BBO race</text><text x="50" y="86" fill="#94a8c2" font-size="16" font-family="sans-serif">${e?`session ${e.session}, sequence ${e.sequence}`:'no strict event sample'}</text>`+r.lanes.map((x,i)=>`<text x="50" y="${150+i*82}" fill="#edf4ff" font-size="18" font-family="sans-serif">${esc(x.name)}</text><line x1="420" x2="1110" y1="${143+i*82}" y2="${143+i*82}" stroke="#263a55" stroke-width="10" stroke-linecap="round"/><circle cx="${430+670*deltas[i]/max}" cy="${143+i*82}" r="13" fill="${COLORS[i]}"/><text x="1010" y="${175+i*82}" fill="#94a8c2" font-size="15" font-family="monospace">+${f(deltas[i])} us</text>`).join('')}
function exportPng(){const svg=$('#raceSvg'),blob=new Blob([new XMLSerializer().serializeToString(svg)],{type:'image/svg+xml'}),url=URL.createObjectURL(blob),img=new Image;img.onload=()=>{const c=document.createElement('canvas');c.width=1200;c.height=520;c.getContext('2d').drawImage(img,0,0);URL.revokeObjectURL(url);const a=document.createElement('a');a.download='bybit-bbo-race.png';a.href=c.toDataURL('image/png');a.click()};img.src=url}
const select=$('#raceSelect');DATA.races.forEach((r,i)=>select.add(new Option(`${r.symbol} · group ${r.group}`,i)));select.addEventListener('change',()=>draw(+select.value));$('#png').addEventListener('click',exportPng);draw(0);
</script></body></html>)HTML";
  output.flush();
  if (output) return true;
  error = "dashboard_write_failed";
  return false;
}

}  // namespace exchange_probe::race
