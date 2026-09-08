#pragma once

#include <string_view>

namespace exchange_probe::viewer_assets {

inline constexpr std::string_view kHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Exchange API Probe</title>
  <link rel="stylesheet" href="/app.css">
</head>
<body>
  <header>
    <h1>Exchange API Probe</h1>
    <input id="search" type="search" placeholder="venue, product, symbol, channel">
  </header>
  <main>
    <aside><div id="catalog"></div></aside>
    <section>
      <div id="summary" class="cards"></div>
      <div class="toolbar">
        <label>Relation
          <select id="relation-mode">
            <option value="all">all</option>
            <option value="identity">identity</option>
            <option value="exchange_time_cohort">exchange time</option>
            <option value="market_effect">market effect</option>
            <option value="state_convergence">state convergence</option>
          </select>
        </label>
        <label><input id="receive-axis" type="checkbox" checked> monotonic</label>
        <label><input id="utc-axis" type="checkbox"> local UTC</label>
        <label><input id="exchange-axis" type="checkbox" checked> exchange E</label>
        <label><input id="transaction-axis" type="checkbox"> exchange T</label>
      </div>
      <div id="timeline"></div>
      <h2>Findings</h2>
      <pre id="findings"></pre>
      <h2>Evidence preview</h2>
      <div id="evidence"></div>
    </section>
  </main>
  <script type="module" src="/app.js"></script>
</body>
</html>)HTML";

inline constexpr std::string_view kCss = R"CSS(
:root{color-scheme:dark;font-family:Inter,system-ui,sans-serif;background:#0b1020;color:#e7ecf5}
*{box-sizing:border-box}body{margin:0}header{height:64px;display:flex;align-items:center;gap:24px;padding:0 20px;border-bottom:1px solid #26314c;background:#11182a}
h1{font-size:19px;margin:0;white-space:nowrap}input,select{background:#0b1020;color:#e7ecf5;border:1px solid #34415f;border-radius:6px;padding:8px}
#search{width:min(560px,60vw)}main{display:grid;grid-template-columns:280px 1fr;min-height:calc(100vh - 64px)}
aside{border-right:1px solid #26314c;padding:12px;overflow:auto}.bundle{display:block;width:100%;text-align:left;background:#11182a;color:#e7ecf5;border:1px solid #26314c;border-radius:8px;padding:10px;margin-bottom:8px;cursor:pointer}
.bundle:hover,.bundle.active{border-color:#59a7ff}.muted{color:#91a0bd;font-size:12px}section{padding:18px;overflow:auto}
.cards{display:flex;gap:10px;flex-wrap:wrap}.card{min-width:130px;background:#11182a;border:1px solid #26314c;border-radius:8px;padding:12px}.value{font-size:22px;color:#7dc4ff}
.toolbar{display:flex;gap:18px;align-items:center;margin:18px 0}.toolbar label{display:flex;gap:7px;align-items:center}
#timeline{min-height:330px;background:#080d19;border:1px solid #26314c;border-radius:8px;overflow:auto}
.lane{position:relative;height:58px;border-bottom:1px solid #1d263b}.lane-name{position:sticky;left:0;display:inline-block;width:140px;padding:8px;color:#9fb2d2;background:#080d19;z-index:2}
.event{position:absolute;top:27px;width:8px;height:8px;border-radius:50%;background:#48b4ff;transform:translateX(-4px)}.event.trade{background:#ffb454}.event.depth{background:#68d391}.event.book_ticker{background:#d6a7ff}
.axis{height:30px;padding:6px 145px;color:#91a0bd;border-bottom:1px solid #26314c;font-size:12px}
pre{white-space:pre-wrap;background:#080d19;border:1px solid #26314c;border-radius:8px;padding:12px}.evidence-row{font-family:ui-monospace,monospace;font-size:12px;border-bottom:1px solid #1d263b;padding:6px}
@media(max-width:850px){main{grid-template-columns:1fr}aside{border-right:0;border-bottom:1px solid #26314c;max-height:220px}}
)CSS";

inline constexpr std::string_view kJs = R"JS(
const state={catalog:[],selected:null,events:[],relations:[]};
const byId=id=>document.getElementById(id);
const esc=value=>String(value??'').replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
async function json(url){const response=await fetch(url);if(!response.ok)throw new Error(`${response.status} ${url}`);return response.json()}
async function lines(url){const response=await fetch(url);if(!response.ok)return[];const text=await response.text();return text.split('\n').filter(Boolean).map(line=>JSON.parse(line))}
function renderCatalog(){
 const query=byId('search').value.toLowerCase();
 byId('catalog').innerHTML=state.catalog.filter(item=>JSON.stringify(item).toLowerCase().includes(query)).map((item,index)=>
 `<button class="bundle ${state.selected===item.index?'active':''}" data-index="${item.index}">
 <strong>${esc(item.venue)} / ${esc(item.product)}</strong><br><span>${esc(item.symbol)}</span><br>
 <span class="muted">${esc(item.status)} · ${esc(item.path)}</span></button>`).join('');
 document.querySelectorAll('.bundle').forEach(button=>button.onclick=()=>selectBundle(Number(button.dataset.index)));
}
function card(label,value){return `<div class="card"><div class="muted">${esc(label)}</div><div class="value">${esc(value)}</div></div>`}
function renderTimeline(){
 const mode=byId('relation-mode').value;
 const relationIds=new Set(state.relations.filter(item=>mode==='all'||item.mode===mode).flatMap(item=>[item.source_event_id,item.target_event_id]));
 const events=mode==='all'?state.events:state.events.filter(item=>relationIds.has(item.event_id));
 if(!events.length){byId('timeline').innerHTML='<div class="axis">No normalized events in preview</div>';return}
 const useReceive=byId('receive-axis').checked;
 const useUtc=byId('utc-axis').checked;
 const useExchange=byId('exchange-axis').checked;
 const useTransaction=byId('transaction-axis').checked;
 const lanes=[...new Set(events.map(item=>item.channel_id))];
 const renderAxis=(name,valueOf)=>{
   const available=events.filter(item=>valueOf(item)!=null);
   if(!available.length)return `<div class="axis">${name}: no declared timestamps</div>`;
   const values=available.map(valueOf),min=Math.min(...values),max=Math.max(...values),span=Math.max(1,max-min);
   const axis=`<div class="axis">${name}: ${min} .. ${max} ns</div>`;
   const body=lanes.map(lane=>`<div class="lane"><span class="lane-name">${esc(lane)}</span>${available.filter(item=>item.channel_id===lane).map(item=>{
     const left=145+Math.round(800*(valueOf(item)-min)/span);
     return `<span class="event ${esc(item.event_kind)}" style="left:${left}px" title="event=${item.event_id} receive=${item.monotonic_ns} UTC=${item.utc_ns} E=${item.exchange_event_ns} T=${item.exchange_transaction_ns} price=${esc(item.price)} bid=${esc(item.bid_price)} ask=${esc(item.ask_price)}"></span>`
   }).join('')}</div>`).join('');
   return axis+body;
 };
 const panels=[];
 if(useReceive)panels.push(renderAxis('local monotonic receive',item=>item.monotonic_ns));
 if(useUtc)panels.push(renderAxis('local UTC',item=>item.utc_ns));
 if(useExchange)panels.push(renderAxis('exchange event E',item=>item.exchange_event_ns));
 if(useTransaction)panels.push(renderAxis('exchange transaction T',item=>item.exchange_transaction_ns));
 if(!panels.length)panels.push('<div class="axis">Select at least one time axis</div>');
 byId('timeline').innerHTML=`<div style="min-width:980px">${panels.join('')}</div>`;
}
async function selectBundle(index){
 state.selected=index;renderCatalog();
 const [manifest,findings,events,relations]=await Promise.all([
   json(`/api/file?bundle=${index}&name=manifest.json`),
   json(`/api/file?bundle=${index}&name=findings.json`).catch(()=>({})),
   lines(`/api/preview?bundle=${index}&name=events.jsonl&limit=5000`),
   lines(`/api/preview?bundle=${index}&name=relations.jsonl&limit=5000`)
 ]);
 state.events=events;state.relations=relations;
 byId('summary').innerHTML=card('venue',manifest.venue)+card('product',manifest.product)+card('symbol',manifest.symbol)+card('frames',manifest.frames)+card('events',findings.events??0)+card('relations',findings.relations??0);
 byId('findings').textContent=JSON.stringify(findings,null,2);
 byId('evidence').innerHTML=events.slice(0,200).map(item=>`<div class="evidence-row">${esc(JSON.stringify(item))}</div>`).join('');
 renderTimeline();
}
async function boot(){state.catalog=await json('/api/catalog');renderCatalog();if(state.catalog.length)selectBundle(state.catalog[0].index)}
byId('search').addEventListener('input',renderCatalog);byId('relation-mode').addEventListener('change',renderTimeline);byId('exchange-axis').addEventListener('change',renderTimeline);byId('receive-axis').addEventListener('change',renderTimeline);byId('utc-axis').addEventListener('change',renderTimeline);byId('transaction-axis').addEventListener('change',renderTimeline);
boot().catch(error=>{byId('findings').textContent=String(error)});
)JS";

}  // namespace exchange_probe::viewer_assets
