// Light WebUI page (served by /). Kept in a separate header so the JavaScript
// `function` keywords and braces in this raw string literal never reach the
// PlatformIO .ino prototype generator, which text-scans .ino files (ignoring
// #ifdef) and mis-parses raw strings.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-2CAN FSD</title>
<style>
body{font-family:system-ui,Arial,sans-serif;margin:0;padding:12px;background:#111;color:#eee}
h1{font-size:18px}h2{font-size:15px;margin:14px 0 6px;color:#8cf}
.card{background:#1c1c1c;border:1px solid #333;border-radius:8px;padding:10px;margin-bottom:12px}
label{display:flex;justify-content:space-between;align-items:center;margin:4px 0;font-size:14px}
input[type=number]{width:90px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:3px}
button{background:#2a6;color:#fff;border:0;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0;font-size:14px}
button.alt{background:#37c}button.warn{background:#a33}
.kv{display:flex;justify-content:space-between;font-size:13px;padding:2px 0;border-bottom:1px solid #262626}
.kv span:last-child{color:#9f9;font-variant-numeric:tabular-nums}
</style></head><body>
<h1>T-2CAN FSD &middot; runtime settings</h1>

<div class="card">
<h2>Config</h2>
<label>FSD enabled<input type="checkbox" id="fsdEnabled"></label>
<label>Auto speed offset<input type="checkbox" id="autoSpeedOffsetEnabled"></label>
<label>Slew %/s<input type="number" id="slewPctPerSec" min="0" max="100"></label>
<label>Low-speed max PCT4 raw<input type="number" id="lowSpeedMaxPctRaw" min="0" max="255"></label>
<label>Target &lt;60<input type="number" id="targetBelow60" min="0" max="255"></label>
<label>Target 60..69<input type="number" id="target60" min="0" max="255"></label>
<label>Target 70..79<input type="number" id="target70" min="0" max="255"></label>
<label>Target 80..89<input type="number" id="target80" min="0" max="255"></label>
<label>Target 90..99<input type="number" id="target90" min="0" max="255"></label>
<label>Target 100..119<input type="number" id="target100" min="0" max="255"></label>
<label>Target 120..139<input type="number" id="target120" min="0" max="255"></label>
<label>CAN B enabled<input type="checkbox" id="canbEnabled"></label>
<label>CAN B service mode<input type="checkbox" id="canbServiceModeEnabled"></label>
<label>CAN B filter (reserved)<input type="checkbox" id="canbFilterEnabled"></label>
<div>
<button onclick="applyConfig()">Apply (RAM)</button>
<button class="alt" onclick="saveConfig()">Save (Flash)</button>
<button class="warn" onclick="webOff()">Close WebUI</button>
</div>
</div>

<div class="card">
<h2>Status <label style="display:inline;font-size:13px">poll<input type="checkbox" id="poll" onchange="setPolling(this.checked)"></label></h2>
<div class="kv"><span>CAN1 RX</span><span id="can1Rx">-</span></div>
<div class="kv"><span>CAN1 TX</span><span id="can1Tx">-</span></div>
<div class="kv"><span>CAN1 TX fail</span><span id="can1TxFail">-</span></div>
<div class="kv"><span>TWAI bus-off</span><span id="twaiBusOffCount">-</span></div>
<div class="kv"><span>Fused limit kph</span><span id="fusedLimitKph">-</span></div>
<div class="kv"><span>Target kph</span><span id="targetSpeedKph">-</span></div>
<div class="kv"><span>Offset kph</span><span id="offsetKph">-</span></div>
<div class="kv"><span>Offset raw</span><span id="offsetRaw">-</span></div>
<div class="kv"><span>CAN B RX</span><span id="canbRx">-</span></div>
<div class="kv"><span>CAN B TX</span><span id="canbTx">-</span></div>
<div class="kv"><span>CAN B TX fail</span><span id="canbTxFail">-</span></div>
<div class="kv"><span>CAN B last ID</span><span id="canbLastId">-</span></div>
<div class="kv"><span>Uptime s</span><span id="uptime">-</span></div>
</div>

<script>
let pollTimer=null,loaded=false;
const ids=["fsdEnabled","autoSpeedOffsetEnabled","slewPctPerSec","lowSpeedMaxPctRaw","targetBelow60","target60","target70","target80","target90","target100","target120","canbEnabled","canbServiceModeEnabled","canbFilterEnabled"];
const stats=["can1Rx","can1Tx","can1TxFail","twaiBusOffCount","fusedLimitKph","targetSpeedKph","offsetKph","offsetRaw","canbRx","canbTx","canbTxFail","canbLastId","uptime"];
function setVal(id,v){const e=document.getElementById(id);if(!e)return;if(e.type==="checkbox")e.checked=!!v;else e.value=v;}
function pollStatus(){
  fetch("/status").then(r=>r.json()).then(j=>{
    stats.forEach(k=>{const e=document.getElementById(k);if(e&&k in j)e.textContent=(k==="canbLastId")?("0x"+(j[k]>>>0).toString(16)):j[k];});
    if(!loaded){ids.forEach(k=>{if(k in j)setVal(k,j[k]);});loaded=true;}
  }).catch(()=>{});
}
function setPolling(on){
  if(on&&!pollTimer){pollStatus();pollTimer=setInterval(pollStatus,1000);}
  if(!on&&pollTimer){clearInterval(pollTimer);pollTimer=null;}
}
function body(){
  const p=new URLSearchParams();
  ids.forEach(k=>{const e=document.getElementById(k);p.set(k,e.type==="checkbox"?(e.checked?1:0):e.value);});
  return p;
}
function applyConfig(){fetch("/config",{method:"POST",body:body()}).then(()=>pollStatus());}
function saveConfig(){fetch("/config",{method:"POST",body:body()}).then(()=>fetch("/save",{method:"POST"}));}
function webOff(){setPolling(false);document.getElementById("poll").checked=false;fetch("/web/off",{method:"POST"});}
pollStatus();
</script>
</body></html>)HTML";
