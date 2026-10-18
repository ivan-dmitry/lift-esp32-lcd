// CAN debug page — http://<ip>/debug
// Frames arrive from the port-82 stream (drv_candebug.cpp). Everything else —
// per-ID table, change highlighting, recording, markers, file export — runs in
// the browser, so the ESP32 only forwards frames.
#pragma once
#include <Arduino.h>

static const char DEBUGPAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>CAN debug</title>
<style>
:root{--bg:#0a0d12;--s:#141a23;--s2:#1c2531;--l:#293547;--t:#e9eef6;--d:#8695ab;--ok:#16a34a;--al:#e11d22;--fa:#f0a500;--ar:#22d3ee}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--t);font:13px system-ui,sans-serif}
header{position:sticky;top:0;z-index:3;background:var(--bg);border-bottom:1px solid var(--l);padding:8px 12px;display:flex;flex-wrap:wrap;gap:8px;align-items:center}
h1{font-size:16px;margin:0 6px 0 0}
.pill{background:var(--s);border:1px solid var(--l);border-radius:99px;padding:3px 10px;color:var(--d)}
.pill b{color:var(--t)}.on{border-color:var(--ok)}.bad{border-color:var(--al);color:#f87171}
main{display:grid;grid-template-columns:minmax(0,1fr) 340px;gap:12px;padding:12px}
@media(max-width:900px){main{grid-template-columns:1fr}}
.box{background:var(--s);border:1px solid var(--l);border-radius:8px;padding:10px;margin-bottom:12px}
h2{font-size:14px;margin:0 0 8px;display:flex;gap:8px;align-items:center}h2 .sp{flex:1}
table{border-collapse:collapse;width:100%;font:12px ui-monospace,Consolas,monospace}
th{color:var(--d);font-weight:500;text-align:left;padding:3px 4px;border-bottom:1px solid var(--l)}
td{padding:3px 4px;border-bottom:1px solid #1d2633;white-space:nowrap}
td.b{text-align:center;width:26px;border-radius:3px}td.hot{background:#3a2c05;color:var(--fa)}
td.dv{color:var(--d);font-size:10px}
tr.tx td:first-child{color:var(--ar)}
button,input,select{background:var(--s2);border:1px solid var(--l);color:var(--t);border-radius:5px;padding:5px 9px;font:inherit}
button{cursor:pointer}button.p{background:var(--ok);border-color:var(--ok)}button.r{background:#5a1418;border-color:var(--al)}
button:disabled{opacity:.4;cursor:not-allowed}
pre{margin:0;max-height:320px;overflow:auto;font:11px ui-monospace,Consolas,monospace;color:#c8d2e0;white-space:pre}
.row{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:6px 0}
.n{color:var(--d);font-size:12px}.err{color:#f87171}.okc{color:#4ade80}
.fl{display:grid;grid-template-columns:repeat(auto-fill,minmax(46px,1fr));gap:5px}
.fl button{padding:8px 0;font-weight:700}.fl button.lit{background:var(--ok)}
.mk{color:var(--fa)}
td.co{color:var(--ar);font:11px system-ui,sans-serif;white-space:normal;min-width:150px}
.nd td{font-size:12px}
</style>
<header>
  <h1>CAN debug</h1>
  <span class=pill id=conn>connecting…</span>
  <span class=pill>bus <b id=st>—</b></span>
  <span class=pill><b id=kb>—</b> kbit/s</span>
  <span class=pill><b id=fps>0</b> frames/s</span>
  <span class=pill>errors <b id=er>0</b> · lost <b id=dr>0</b></span>
  <span class=pill><b id=nid>0</b> IDs</span>
  <span style=flex:1></span>
  <button id=pause>Pause view</button><button id=clr>Clear table</button>
  <a href=/ style=color:var(--ar)>status</a> · <a href=/cfg style=color:var(--ar)>settings</a>
</header>
<main>
<div>
  <div class=box><h2>Frames by ID <span class=n>amber = byte changed in the last 1.5 s · digits under a byte = distinct values seen</span><span class=sp></span>
    <input id=flt placeholder="filter IDs: 100,1A0-1AF,!301" size=24></h2>
    <table><thead><tr><th>ID<th>DLC<th colspan=8>data (B0 … B7)<th>meaning (Fuji — or CANopen once heartbeats are heard)<th>count<th>/s<th></tr></thead><tbody id=tb></tbody></table>
  </div>
  <div class=box><h2>Raw signal <span class=n>logic analyzer on the RX pin — high = recessive (idle), low = dominant</span><span class=sp></span>
    <select id=rwMs><option>20<option selected>50<option>100</select><span class=n>ms</span><button class=p id=rwGo>Capture</button></h2>
    <div class=row><span class=n>from</span><input type=range id=rwOff min=0 max=0 value=0 style=flex:1>
      <span class=n>width</span><select id=rwW><option value=0.5>0.5 ms<option value=1>1 ms<option value=2 selected>2 ms<option value=5>5 ms<option value=20>20 ms<option value=100>all</select></div>
    <svg id=rwSvg viewBox="0 0 1000 120" preserveAspectRatio=none style="width:100%;height:120px;background:#0d1219;border-radius:6px"></svg>
    <p class=n id=rwSum>Capture shows the real line: every edge, no gap rules — also on a busy or odd bus. The bit time is worked out, every burst is decoded as CAN (stuff bits, errors) and as a UART.</p>
    <pre id=rwDec style="max-height:260px"></pre>
  </div>
  <div class=box><h2>Errors reported by the CAN controller <span class=n id=ecK></span></h2>
    <table class=nd><tbody id=ecT></tbody></table>
    <p class=n>Error type and the part of the frame where it happened, sampled at each new bus error. Only meaningful once the bit rate is right — at a wrong rate everything is a stuff or form error.</p></div>
  <div class=box><h2>Live log <span class=n id=logn></span></h2><pre id=log></pre></div>
</div>
<div>
  <div class=box><h2>CANopen nodes <span class=n>heartbeat 0x700+node</span></h2>
    <p class=n id=coV>No heartbeat yet. Every CiA 417 device sends one each second — none means this is not a CANopen bus, or the bit rate is wrong (CiA 417 uses 250 kbit/s).</p>
    <table class=nd><tbody id=coT></tbody></table></div>
  <div class=box><h2>Recorder</h2>
    <div class=row><button class=p id=rec>● Record</button><span id=rinfo class=n>not recording</span></div>
    <div class=row><input id=mtxt placeholder="marker, e.g. door open at 3" style=flex:1><button id=mbtn>Mark</button></div>
    <div class=row id=qm></div>
    <div class=row><button id=dlLog>Download .log (candump)</button><button id=dlCsv>Download .csv</button></div>
    <p class=n>Recording runs in this browser tab. Mark what you do at the lift (call a floor, open the door…) and the moment lands in the file — the frames right after a marker are the ones to look at. The .log opens in SavvyCAN / python-can / cantools.</p>
  </div>
  <div class=box><h2>Control</h2>
    <div id=login class=row><input id=pin type=password placeholder="technician code" size=10><button id=lg>Log in</button></div>
    <div class=row><select id=cmode><option value=car>Car call<option value=up>Hall up<option value=dn>Hall down</select>
      <button id=dop>Door open</button><button id=dcl>Door close</button><button id=dho>Door hold</button></div>
    <div class=fl id=fl></div>
    <p class=n id=why></p>
    <h2 style=margin-top:12px>Send raw frame</h2>
    <div class=row><input id=rid placeholder="ID hex" size=8><input id=rdata placeholder="data hex, e.g. 0105" size=18 style=flex:1>
      <label class=n><input type=checkbox id=rext> 29-bit</label></div>
    <div class=row><button class=r id=rsend>Send</button><span class=n id=rmsg>needs: code · remote commands ON · send commands ON · ACK mode (panel)</span></div>
    <h2 style=margin-top:12px>Bus</h2>
    <div class=row><select id=kbs><option value=12>12.5<option value=16>16<option value=20>20<option value=25>25<option value=2778>27.78<option value=33>33.3<option value=40>40<option value=47>47.6<option value=50>50<option value=62>62.5<option value=66>66.7<option value=80>80<option value=83>83.3<option value=100>100<option value=125>125<option value=200>200<option value=250>250<option value=400>400<option value=500>500<option value=666>666.7<option value=800>800<option value=1000>1000</select>
      <button id=kbset>Set bit rate</button><button id=auto>Detect bit rate</button><button id=probe>Measure bit timing</button></div>
    <p class=n id=pres>Measure bit timing times the pulses on the RX pin (0.1 µs) and works out the real bit rate — also non-standard ones.</p>
  </div>
</div>
</main>
<script>
const $=i=>document.getElementById(i);
const T=()=>sessionStorage.t||'';
const ids=new Map(),nodes=new Map();let logA=[],paused=false,rec=null,lastT=0,dirty=true,flt=null;

// ── CANopen / CiA 417 names. Only a guess until heartbeats prove the bus is CANopen. ──
// 0x400+lift-1: call controller → panels, 0x480+node: panel → call controller (6-byte
// record: function, sub-function, lift mask, floor, door, data bit 0 = on).
// 0x180+16·(lift-1): drive / position PDOs — verified for lift 1 only (WEG ADL300 manual).
const NMT={0:'boot-up',4:'stopped',5:'operational',127:'pre-operational'};
function rec4(d){const b=i=>parseInt(d.substr(i*2,2),16),on=b(5)&1?'ON':'off';
 switch(b(0)){case 0x40:return `position ${b(3)!=255?b(3):b(1)} ${on}`;
  case 0x42:return `arrow ${b(1)&1?'up':b(1)&2?'down':'?'} ${on}`;
  case 2:return `hall call ${b(1)==1?'up':b(1)==2?'down':'up+dn'} floor ${b(3)} ${on}`;
  case 5:return `car call floor ${b(3)} ${on}`;
  default:return `function ${d.substr(0,2)} · sub ${d.substr(2,2)} · floor ${b(3)} ${on}`}}
// ── Fuji hall-call bus (27.78 kbit/s), as decoded on site — used while no CANopen heartbeat is heard ──
function bitsList(d,lo){const out=[];for(let i=0;i<8;i++){const v=parseInt(d.substr(i*2,2),16)||0;for(let k=0;k<8;k++)if(v>>k&1)out.push(i*8+k+lo)}return out.join(',')||'none'}
function fuji(id,dlc,d){const v=parseInt(id,16),b=i=>parseInt(d.substr(i*2,2),16)||0;
 if(v===0x030){const s=b(0);return `Fuji status · floor ${b(7)-2}${s&0x80?' · UP':''}${s&0x40?' · DOWN':''} · ${s&0x20?'running':'stopped'} · door ${s&0x08?'closed':'NOT closed'}${b(4)&0x10?' · door zone':''}`}
 if(v===0x031)return `controller clock, item ${b(7)} = ${d.substr(12,2)}`;
 if(v===0x033)return 'car calls: '+bitsList(d,1);
 if(v===0x034)return 'hall calls up: '+bitsList(d,1);
 if(v===0x035)return 'hall calls down: '+bitsList(d,1);
 if(v===0x037)return 'arrival lantern'+(b(3)&2?' UP':'')+(b(3)&1?' DOWN':'')+(b(3)&3?'':' off');
 if(v===0x07C)return 'door'+(b(1)&0x80?' fully open':'')+(b(1)&0x40?' fully closed':'')+(b(1)&0xC0?'':' moving');
 if(v===0x07D)return 'car button pressed: '+bitsList(d,1);
 if(v===0x404)return 'controller info page '+d.substr(0,2);
 if(v===0x545)return 'speed '+b(6);
 if(v>=0x640&&v<0x660)return `hall button UP at floor ${v-0x640+1}`+(b(0)?'':' (released)');
 if(v>=0x6C0&&v<0x6E0)return `hall button DOWN at floor ${v-0x6C0+1}`+(b(0)?'':' (released)');
 return ''}
function co(id,dlc,d){if(id.length>3)return '';if(!nodes.size)return fuji(id,dlc,d);const v=parseInt(id,16),b=i=>parseInt(d.substr(i*2,2),16);
 if(v==0)return 'NMT command';if(v==0x80)return 'SYNC';if(v>0x80&&v<0x100)return 'EMCY node '+(v-0x80)+(dlc>=2?' · code '+d.substr(2,2)+d.substr(0,2):'');
 if(v==0x100)return 'TIME';
 if(v>=0x180&&v<0x200){const L=((v-0x180)>>4)+1,k=(v-0x180)&15;
  return `lift ${L} `+({0:'target position',1:'control effort',2:'drive command',
   3:'drive status'+(dlc==8?' · v='+((b(4)|b(5)<<8|b(6)<<16|b(7)<<24)):''),12:'car position'+(dlc>=4?' '+((b(0)|b(1)<<8|b(2)<<16|b(3)<<24)>>>0):'')}[k]||'PDO')}
 if(v>=0x400&&v<0x408&&dlc>=6)return `lift ${v-0x3FF} → panels: `+rec4(d);
 if(v>0x480&&v<0x500&&dlc>=6)return `panel node ${v-0x480} → call request: `+rec4(d);
 if(v>0x500&&v<0x580)return 'MPDO node '+(v-0x500)+(dlc==8&&b(0)&0x80?` · write ${d.substr(4,2)}${d.substr(2,2)}h.${d.substr(6,2)} = ${d.substr(8)}`:'');
 if(v>0x580&&v<0x600)return 'SDO reply, node '+(v-0x580);if(v>0x600&&v<0x680)return 'SDO request, node '+(v-0x600);
 if(v>0x700&&v<0x780)return 'heartbeat node '+(v-0x700)+(dlc==1?' · '+(NMT[b(0)&0x7f]||'?'):'');
 if(v==0x7E4||v==0x7E5)return 'LSS';return ''}
function renderNodes(){const now=performance.now(),r=[...nodes.entries()].sort((a,b)=>a[0]-b[0]);
 if(!r.length)return;const live=r.filter(([,x])=>now-x.t<5000).length;
 $('coV').innerHTML=live?`<span class=okc>CANopen network: ${live} node(s) alive.</span> Node 1 is normally the lift controller.`:'<span class=err>Heartbeats stopped.</span>';
 $('coT').innerHTML=r.map(([n,x])=>`<tr><td>node ${n}<td>${NMT[x.s]||'?'}<td>${((now-x.t)/1000).toFixed(0)} s ago<td>${x.n}`).join('')}
setInterval(renderNodes,1000);

// ── filter "100,1A0-1AF,!301" ──
function parseFlt(s){s=s.trim();if(!s)return null;const inc=[],exc=[];
 for(let p of s.split(/[ ,]+/)){if(!p)continue;const neg=p[0]=='!';if(neg)p=p.slice(1);
  const [a,b]=p.split('-').map(x=>parseInt(x,16));if(isNaN(a))continue;(neg?exc:inc).push([a,isNaN(b)?a:b])}
 return {inc,exc}}
function pass(id){if(!flt)return true;const v=parseInt(id,16);
 if(flt.exc.some(([a,b])=>v>=a&&v<=b))return false;return !flt.inc.length||flt.inc.some(([a,b])=>v>=a&&v<=b)}
$('flt').oninput=()=>{flt=parseFlt($('flt').value);dirty=true};

// ── frames ──
function frame(line){
  const [t,id,dlc,data='',dir]=line.split(' ');if(!id)return;
  const tt=+t;lastT=tt;const now=performance.now();
  let e=ids.get(id+dir);
  if(!e){e={id,dir,dlc:+dlc,data:'',chg:new Array(8).fill(0),vals:Array.from({length:8},()=>new Set()),n:0,win:0,rate:0};ids.set(id+dir,e)}
  for(let i=0;i<8;i++){const b=data.substr(i*2,2);if(b&&b!==e.data.substr(i*2,2)){e.chg[i]=now;if(e.vals[i].size<64)e.vals[i].add(b)}}
  e.data=data;e.dlc=+dlc;e.n++;e.win++;e.co=co(id,+dlc,data);
  const hv=parseInt(id,16);
  if(id.length<=3&&hv>0x700&&hv<0x780&&+dlc==1){const x=nodes.get(hv-0x700)||{n:0};x.s=parseInt(data,16)&0x7f;x.t=now;x.n++;nodes.set(hv-0x700,x)}
  if(rec)rec.push([tt,id,+dlc,data,dir]);
  if(pass(id)){logA.push(`${(tt/1e6).toFixed(6)}  ${dir=='t'?'TX':'  '} ${id.padStart(8)}  [${dlc}]  ${data.replace(/(..)/g,'$1 ').padEnd(24)}${e.co?'  '+e.co:''}`);if(logA.length>400)logA.splice(0,logA.length-400)}
  dirty=true}
setInterval(()=>{for(const e of ids.values()){e.rate=e.win;e.win=0}},1000);

function render(){if(paused||!dirty)return;dirty=false;const now=performance.now();
 const rows=[...ids.values()].filter(e=>pass(e.id)).sort((a,b)=>parseInt(a.id,16)-parseInt(b.id,16)||(a.dir<b.dir?-1:1));
 $('nid').textContent=ids.size;
 $('tb').innerHTML=rows.map(e=>{let c='';for(let i=0;i<8;i++){const b=e.data.substr(i*2,2);
   c+=`<td class="b${b&&now-e.chg[i]<1500?' hot':''}" title="${[...e.vals[i]].join(' ')}">${b||''}<div class=dv>${b?e.vals[i].size:''}</div></td>`}
  return `<tr class=${e.dir=='t'?'tx':''}><td>${e.id}${e.dir=='t'?' TX':''}<td>${e.dlc}${c}<td class=co>${e.co||''}<td>${e.n}<td>${e.rate}<td><button data-r="${e.id}|${e.data}">replay</button>`}).join('');
 const pre=$('log');const atEnd=pre.scrollTop+pre.clientHeight>=pre.scrollHeight-20;
 pre.textContent=logA.join('\n');if(atEnd)pre.scrollTop=pre.scrollHeight;$('logn').textContent=`last ${logA.length} lines`;
 if(rec)$('rinfo').textContent=`recording · ${rec.length} frames · ${((performance.now()-rec.start)/1000).toFixed(0)} s`}
setInterval(render,250);
$('tb').onclick=e=>{const r=e.target.dataset.r;if(!r)return;const [id,d]=r.split('|');$('rid').value=id;$('rdata').value=d;$('rext').checked=id.length>3;$('rmsg').textContent='loaded into Send raw frame'};
$('pause').onclick=()=>{paused=!paused;$('pause').textContent=paused?'Resume view':'Pause view';dirty=true};
$('clr').onclick=()=>{ids.clear();logA=[];dirty=true};

// ── stream ──
function connect(){
 const es=new EventSource('/api/can/stream');
 es.onopen=()=>{$('conn').textContent='live';$('conn').className='pill on'};
 es.onerror=()=>{$('conn').textContent='reconnecting…';$('conn').className='pill bad'};
 es.onmessage=m=>{for(const l of m.data.split('\n'))if(l)frame(l)};
 es.addEventListener('stat',m=>{const s=JSON.parse(m.data);$('fps').textContent=s.fps;$('er').textContent=s.err;
  $('dr').textContent=s.dropped+s.miss;$('kb').textContent=lab(s.kbps);$('st').textContent=s.state;
  if(![...$('kbs').options].some(o=>+o.value===s.kbps))$('kbs').add(new Option(lab(s.kbps)+' (measured)',s.kbps));$('kbs').value=s.kbps});
}
connect();

// ── recorder ──
const QM=['door open','door close','car call','hall call up','hall call down','arrived'];
$('qm').innerHTML=QM.map(q=>`<button data-m="${q}">${q}</button>`).join('');
$('qm').onclick=e=>{if(e.target.dataset.m)mark(e.target.dataset.m)};
function mark(t){if(!rec){$('rinfo').textContent='start recording first';return}
 rec.push([lastT,'#',0,t,'m']);logA.push(`--------  MARK  ${t}`);dirty=true}
$('mbtn').onclick=()=>{if($('mtxt').value.trim()){mark($('mtxt').value.trim());$('mtxt').value=''}};
$('mtxt').onkeydown=e=>{if(e.key=='Enter')$('mbtn').click()};
$('rec').onclick=()=>{if(rec){rec.stopAt=performance.now();$('rec').textContent='● Record';$('rinfo').textContent=`stopped · ${rec.length} rows — download below`;window._last=rec;rec=null}
 else{rec=[];rec.start=performance.now();rec.epoch=Date.now()/1000-lastT/1e6;$('rec').textContent='■ Stop'}};
function save(name,text){const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([text],{type:'text/plain'}));a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),5000)}
const stamp=()=>new Date().toISOString().slice(0,19).replace(/[:T]/g,'-');
const R=()=>rec||window._last;
$('dlLog').onclick=()=>{const r=R();if(!r)return;save(`lift-can-${stamp()}.log`,r.filter(x=>x[4]!='m').map(([t,id,,d])=>`(${(r.epoch+t/1e6).toFixed(6)}) can0 ${id}#${d}`).join('\n')+'\n')};
$('dlCsv').onclick=()=>{const r=R();if(!r)return;save(`lift-can-${stamp()}.csv`,'time_s,id,ext,dlc,data,dir,mark\n'+
 r.map(([t,id,n,d,dir])=>dir=='m'?`${(t/1e6).toFixed(6)},,,,,,"${d.replace(/"/g,"'")}"`:`${(t/1e6).toFixed(6)},${id},${id.length>3?1:0},${n},${d},${dir=='t'?'tx':'rx'},`).join('\n')+'\n')};

// ── control (same locks as the panel: code + remote commands + send commands + ACK) ──
async function api(u,b){const r=await fetch(u,{method:'POST',body:new URLSearchParams({t:T(),...b})});
 if(r.status==401){sessionStorage.t='';showLogin();throw 0}return r.json()}
function showLogin(){$('login').style.display=T()?'none':'flex'}
$('lg').onclick=async()=>{const r=await fetch('/api/login',{method:'POST',body:new URLSearchParams({pin:$('pin').value})});const j=await r.json();
 if(j.t){sessionStorage.t=j.t;showLogin()}else $('why').textContent=j.err};
showLogin();
let S=null;
async function state(){try{S=await(await fetch('/api/state')).json()}catch{return}
 $('why').textContent=S.cmdOk?'Calls allowed.':'Calls locked: '+S.why;$('why').className=S.cmdOk?'n okc':'n err';
 const mode=$('cmode').value,bm=mode=='car'?S.car:mode=='up'?S.up:S.dn;
 const big=BigInt('0x'+bm);
 $('fl').innerHTML=S.floors.map((l,i)=>`<button data-f=${i} class="${(big>>BigInt(i))&1n?'lit':''}">${l}</button>`).reverse().join('')}
setInterval(state,1500);state();
$('cmode').onchange=state;
$('fl').onclick=async e=>{const f=e.target.dataset.f;if(f===undefined)return;
 try{const j=await api('/api/cmd',{c:$('cmode').value,f});$('why').textContent=j.msg}catch{}};
$('dop').onclick=async()=>{try{$('why').textContent=(await api('/api/cmd',{c:'open'})).msg}catch{}};
$('dcl').onclick=async()=>{try{$('why').textContent=(await api('/api/cmd',{c:'close'})).msg}catch{}};
$('dho').onclick=async()=>{try{$('why').textContent=(await api('/api/cmd',{c:'hold'})).msg}catch{}};
$('rsend').onclick=async()=>{const id=$('rid').value.trim(),d=$('rdata').value.replace(/[^0-9a-f]/gi,'');
 if(!id){$('rmsg').textContent='ID needed';return}
 if(!confirm(`Send ID ${id} [${d}] onto the LIFT's bus?`))return;
 try{const j=await api('/api/tx',{id,data:d,ext:$('rext').checked?1:0});$('rmsg').textContent=j.msg;$('rmsg').className=j.ok?'n okc':'n err'}catch{}};
$('kbset').onclick=async()=>{try{await api('/api/set',{kbps:$('kbs').value});$('rmsg').textContent='bit rate set (listen-only first)'}catch{}};
$('auto').onclick=async()=>{try{const j=await api('/api/can/autobaud',{});$('rmsg').textContent=j.msg}catch{}};
$('probe').onclick=async()=>{$('pres').textContent='measuring…';try{const p=await(await fetch('/api/can/probe')).json();
 $('pres').innerHTML=!p.pulses?'No edges at all on RX: idle bus, or the RX line / transceiver is not connected.'
  :p.pulses<20?`Only ${p.pulses} edges in 0.6 s (${p.raw}) — no CAN traffic at any rate. Check CANH/CANL/GND; the line may not be a CAN bus.`
  :`${p.pulses} pulses · RX high ${p.highPct}% · shortest ${p.minUs} µs · bit ${p.bitUs.toFixed(2)} µs ≈ ${(p.rate/1000).toFixed(1)} kbit/s · fit ${p.fit.toFixed(3)} `+
   (p.fit>0.15?'— does NOT look like CAN (check CANH/CANL and GND)':p.uart&&!p.nearestOk?`— this is a UART / RS-485 line at ${p.uart} baud, NOT CAN`:
    p.nearestOk?`→ set ${p.nearestLabel} kbit/s`:'— no supported rate within 3 %')+
   ` · ${p.edgesPerSec} edges/s · longest level ${p.maxRun} bits${p.maxRun>6?' (CAN never holds a level > 6 bits inside a frame)':''}`+
   (p.raw?`<br><span class=n>first pulses µs: ${p.raw}</span>`:'')+
   (!p.nearestOk&&p.fit<=0.15&&p.rate>=10000&&p.rate<=1000000&&!p.uart?` <button id=pUse>Use measured ${lab(rateCode(p.rate))} kbit/s</button>`:'');
 if($('pUse'))$('pUse').onclick=()=>useRate(p.rate);
 if(p.nearestOk&&p.fit<=0.15)$('kbs').value=p.nearest}catch{$('pres').textContent='probe failed'}};

// ── rate codes: above 1000 = a measured rate, bit/s ÷ 10 ──
const lab=k=>k>1000?(k/100).toFixed(2):({12:'12.5',33:'33.3',47:'47.6',62:'62.5',66:'66.7',83:'83.3',666:'666.7'}[k]||String(k));
const TAB={12:12500,16:16000,20:20000,25:25000,2778:27780,33:33333,40:40000,47:47619,50:50000,62:62500,66:66667,80:80000,83:83333,100:100000,125:125000,200:200000,250:250000,400:400000,500:500000,666:666667,800:800000,1000:1000000};
function rateCode(bps){let c=Math.round(bps/10);for(const [k,v] of Object.entries(TAB))if(Math.abs(v-bps)<=v*0.01)c=+k;return c}
async function useRate(bps){const c=rateCode(bps);
 try{await api('/api/set',{kbps:c});$('rmsg').textContent=`bit rate set to ${lab(c)} kbit/s (listen-only first)`;$('rmsg').className='n okc'}catch{}}

// ── raw capture ──
let RW=null;
$('rwGo').onclick=async()=>{$('rwSum').textContent='capturing…';
 try{rwSet(await(await fetch('/api/can/raw?ms='+$('rwMs').value)).json())}catch{$('rwSum').textContent='capture failed'}};
function bitTime(d){const p=d.slice(1).filter(x=>x>3).sort((a,b)=>a-b);if(p.length<10)return {T:0,fit:9};
 const base=p[Math.floor(p.length/10)];let bt=base,be=1e9;
 for(let T=base*0.8;T<=base*1.25;T+=base*0.0025){let e=0,u=0;for(const x of p){const r=x/T;if(r<0.5||r>12.5)continue;e+=Math.abs(r-Math.round(r));u++}
  if(u>p.length/3&&e/u<be){be=e/u;bt=T}}
 return {T:bt,fit:be}}
function rwSet(j){const d=j.d||[];
 if(d.length<3){RW=null;$('rwSvg').innerHTML='';$('rwDec').textContent='';
  $('rwSum').textContent=`${j.edges||0} edges in ${j.ms} ms — the line is static: `+(j.lvl0?'HIGH = recessive / idle (no traffic, or CANH/CANL not connected)':'LOW = dominant all the time (shorted, stuck, or CANH/CANL on the wrong pair)');return}
 const b=bitTime(d);RW={...j,T:b.T,fit:b.fit};
 let hi=0,tot=0,lv=j.lvl0;for(const x of d){if(lv)hi+=x;tot+=x;lv^=1}
 RW.tot=tot;const bps=RW.T?1e7/RW.T:0;
 $('rwSum').innerHTML=`${j.edges} edges in ${j.ms} ms${j.full?' (buffer full)':''} · high ${Math.round(hi*100/tot)} % · bit ≈ ${(RW.T/10).toFixed(2)} µs ≈ <b>${(bps/1000).toFixed(2)} kbit/s</b> · fit ${b.fit.toFixed(3)}${b.fit>0.15?' (not clean multiples: not CAN-like)':''}`+
  (bps>=10000&&bps<=1000000&&b.fit<=0.15?` <button id=rwUse>Use ${lab(rateCode(bps))} kbit/s</button>`:'');
 if($('rwUse'))$('rwUse').onclick=()=>useRate(bps);
 $('rwOff').max=Math.max(0,Math.floor(tot/1000));$('rwOff').value=0;rwDraw();rwDecode()}
function rwDraw(){if(!RW)return;const off=+$('rwOff').value*1000,w=+$('rwW').value*10000,X=v=>(v-off)/w*1000,Y=l=>l?22:92;
 let t=0,lv=RW.lvl0,s='',prev=null;
 for(const x of RW.d){const a=t,e=t+x;t=e;const l=lv;lv^=1;if(e<off)continue;if(a>off+w)break;
  const xa=Math.max(0,X(a)),xe=Math.min(1000,X(e));
  s+=(prev===null?'M':'L')+xa.toFixed(1)+','+Y(l)+'L'+xe.toFixed(1)+','+Y(l);prev=l}
 let g='';if(RW.T&&w/RW.T<=250)for(let k=Math.ceil(off/RW.T);k*RW.T<=off+w;k++){const gx=X(k*RW.T).toFixed(1);g+=`<line x1=${gx} y1=12 x2=${gx} y2=102 stroke="#1d2633"/>`}
 $('rwSvg').innerHTML=g+`<path d="${s}" fill=none stroke="#22d3ee" stroke-width=1.5 vector-effect=non-scaling-stroke />`+
  `<text x=4 y=14 fill="#8695ab" font-size=10>recessive</text><text x=4 y=114 fill="#8695ab" font-size=10>dominant</text>`+
  `<text x=996 y=114 fill="#8695ab" font-size=10 text-anchor=end>${(off/10000).toFixed(2)} – ${((off+w)/10000).toFixed(2)} ms${RW.T&&w/RW.T<=250?' · grid = 1 bit':''}</text>`}
$('rwOff').oninput=rwDraw;$('rwW').onchange=rwDraw;

// Bursts = what lies between recessive gaps of ≥ 11 bits (a CAN frame always ends with 11)
function rwDecode(){if(!RW||!RW.T){$('rwDec').textContent='';return}
 const T=RW.T,bursts=[];let cur=null,lv=RW.lvl0;
 RW.d.forEach((x,i)=>{const n=x/T,l=lv;lv^=1;if(i===0)return;
  if(l===1&&n>=10.5){if(cur&&cur.length)bursts.push(cur);cur=[];return}
  if(cur)cur.push((l?'1':'0').repeat(Math.max(1,Math.round(n))))});
 const map=new Map();for(const b of bursts){const k=b.join('');map.set(k,(map.get(k)||0)+1)}
 const L=[`${bursts.length} complete bursts, ${map.size} different (most frequent first). 0 = dominant, 1 = recessive:`];
 for(const [bits,cnt] of [...map.entries()].sort((a,b)=>b[1]-a[1]).slice(0,10)){
  L.push('');L.push(`×${cnt}  ${bits.length} bits  ${bits.replace(/(.{10})/g,'$1 ')}`);
  L.push('   as CAN : '+canDec(bits+'1'.repeat(11)));
  L.push('   as UART: '+uartDec(bits+'1'.repeat(10)));
 }
 $('rwDec').textContent=L.join('\n')}
function canDec(b){if(b[0]!=='0')return 'no dominant start bit';
 let run=1,last='0',s='0',stuff=0,need=0;
 for(let i=1;i<b.length;i++){const c=b[i];
  if(need&&s.length>=need)return `frame complete: ${fields(s)} · ${stuff} stuff bits · then ACK slot ${b[i+1]==='0'?'dominant = acknowledged':'recessive = NOT acknowledged'}`;
  if(run===5){if(c===last){const m=b.slice(i+1).match(/^0{6,12}/);
    return `STUFF ERROR at bit ${i} (6 × ${last==='0'?'dominant':'recessive'})`+(m?` → ${m[0].length} dominant bits follow = an error flag: a node rejected the frame`:'')+` · decoded before it: ${fields(s)}`}
   stuff++;run=1;last=c;continue}
  if(c===last)run++;else{run=1;last=c}
  s+=c;
  if(!need&&s.length>=19){const ide=s[13];if(ide==='0'){const dlc=Math.min(8,parseInt(s.slice(15,19),2));need=19+8*dlc+15}else need=1e9}}
 return `ends inside the frame after ${s.length} bits: ${fields(s)}`}
function fields(s){if(s.length<12)return `ID bits ${s.slice(1)||'—'}`;
 const id=parseInt(s.slice(1,12),2);let r=`ID 0x${id.toString(16).toUpperCase().padStart(3,'0')}`;
 if(s.length>12)r+=` RTR ${s[12]}`;if(s.length>13)r+=` IDE ${s[13]}${s[13]==='1'?' (29-bit)':''}`;
 if(s.length>=19&&s[13]==='0'){const dlc=parseInt(s.slice(15,19),2);r+=` DLC ${dlc}`;const n=Math.min(8,dlc),dat=s.slice(19,19+8*n);
  if(dat.length>=8)r+=' data '+dat.match(/.{8}/g).map(x=>parseInt(x,2).toString(16).toUpperCase().padStart(2,'0')).join(' ')}
 return r}
function uartDec(s){const out=[];let i=0;
 while(i<s.length){if(s[i]!=='0'){i++;continue}if(i+10>s.length)break;
  const v=parseInt(s.slice(i+1,i+9).split('').reverse().join(''),2);out.push(v.toString(16).toUpperCase().padStart(2,'0')+(s[i+9]==='1'?'':'!'));i+=10}
 return (out.join(' ')||'—')+' (8N1, ! = no stop bit)'}

// ── what the CAN controller reports ──
async function ecc(){try{const j=await(await fetch('/api/can/errors')).json();
 $('ecK').textContent=`at ${j.kbps} kbit/s · ${j.samples} samples`;
 $('ecT').innerHTML=j.codes.map(c=>`<tr><td>${c.type} error<td>${c.rx?'receiving':'transmitting'}<td>${c.seg}<td>${c.n}`).join('')||'<tr><td class=n>no bus error since the last rate change'}catch{}}
setInterval(ecc,3000);ecc();
</script>)HTML";
