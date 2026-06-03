// Light WebUI page (served by /). Kept in a separate header so the JavaScript
// `function` keywords and braces in this raw string literal never reach the
// PlatformIO .ino prototype generator, which text-scans .ino files (ignoring
// #ifdef) and mis-parses raw strings.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-2CAN FSD 设置</title>
<style>
body{font-family:system-ui,Arial,sans-serif;margin:0;padding:12px;background:#111;color:#eee}
h1{font-size:18px}h2{font-size:15px;margin:14px 0 6px;color:#8cf}
.card{background:#1c1c1c;border:1px solid #333;border-radius:8px;padding:10px;margin-bottom:12px}
label{display:flex;justify-content:space-between;align-items:center;margin:5px 0;font-size:14px;gap:12px}
input[type=number]{width:90px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:3px}
input[type=text]{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:6px;margin-top:4px}
button{background:#2a6;color:#fff;border:0;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0;font-size:14px}
button.alt{background:#37c}button.warn{background:#a33}
.kv{display:flex;justify-content:space-between;font-size:13px;padding:2px 0;border-bottom:1px solid #262626}
.kv span:last-child{color:#9f9;font-variant-numeric:tabular-nums}
.hint{font-size:12px;color:#aaa;margin:4px 0 0;line-height:1.4}
.result{font-size:12px;color:#ffd479;margin:8px 0 0;min-height:18px}
</style></head><body>
<h1>T-2CAN FSD 运行参数</h1>

<div class="card">
<h2>配置</h2>
<label>FSD 启用<input type="checkbox" id="fsdEnabled"></label>
<label>自动速度偏移<input type="checkbox" id="autoSpeedOffsetEnabled"></label>
<label>缓降百分比/秒<input type="number" id="slewPctPerSec" min="0" max="100"></label>
<label>低速最大 raw<input type="number" id="lowSpeedMaxPctRaw" min="0" max="255"></label>
<label>目标速度 &lt;60<input type="number" id="targetBelow60" min="0" max="255"></label>
<label>目标速度 60..69<input type="number" id="target60" min="0" max="255"></label>
<label>目标速度 70..79<input type="number" id="target70" min="0" max="255"></label>
<label>目标速度 80..89<input type="number" id="target80" min="0" max="255"></label>
<label>目标速度 90..99<input type="number" id="target90" min="0" max="255"></label>
<label>目标速度 100..119<input type="number" id="target100" min="0" max="255"></label>
<label>目标速度 120..139<input type="number" id="target120" min="0" max="255"></label>
<label>CAN B 启用<input type="checkbox" id="canbEnabled"></label>
<label>CAN B 维修模式<input type="checkbox" id="canbServiceModeEnabled"></label>
<label>CAN B 硬件过滤<input type="checkbox" id="canbFilterEnabled"></label>
<label>高光爆闪启用<input type="checkbox" id="highBeamStrobeEnabled"></label>
<label>后雾灯刹车爆闪启用<input type="checkbox" id="rearFogBrakeStrobeEnabled"></label>
<p class="hint">高光爆闪：CAN B 0x249，超车灯拨杆下拉两次触发 8 次；输出 status=1 PULL / status=0 idle，75ms 开 / 75ms 关。</p>
<p class="hint">后雾灯爆闪：缓减速触发 3 次，急减速触发 6 次；0x145/0x273/0x148/0x185 作为刹车与制动确认。</p>
<div>
<button onclick="applyConfig()">应用到内存</button>
<button class="alt" onclick="saveConfig()">保存到 Flash</button>
<button class="warn" onclick="webOff()">关闭 WebUI</button>
</div>
<div>
<button class="alt" onclick="testCanB('strobe')">测试 0x249 高光爆闪</button>
<button class="alt" onclick="testCanB('fog')">测试 0x273 后雾灯</button>
</div>
<label>倒挡双闪雾灯启用<input type="checkbox" id="reverseStrobeEnabled"></label>
<p class="hint">倒挡双闪雾灯：CAN A 0x118 识别 DI_gear=R，或 踩刹车+右滚轮向后(0x3C2 rightScrollTicks&lt;0)，触发双闪(0x3C2 hazard 按钮 byte0 bit3)+后雾灯 4 次；双闪按钮帧需实车验证。</p>
<label>电池预热启用<input type="checkbox" id="batteryPreheatEnabled"></label>
<p class="hint">电池预热：开启后 CAN A 每 1000ms 发送 0x082 UI_tripPlanning ON 帧(AF 50 AC 3C FF 03 9A 0F)；关闭后补发数帧 OFF(01 50 AC 3C FF 03 9A 0F)。</p>
<div class="result" id="testResult"></div>
</div>

<div class="card">
<h2>CAN 抓包</h2>
<p class="hint">默认抓取 CAN A/B 灯光与 FSD 相关上下文。清空输入框可抓取全部帧。下载 CSV 前请先停止抓包。</p>
<input type="text" id="recIds" value="">
<div>
<button onclick="startRec()">开始抓包</button>
<button class="warn" onclick="stopRec()">停止</button>
<a class="alt" id="recDownload" href="/rec_download" download="can_recording.csv" style="display:none;color:#fff;text-decoration:none;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0;background:#37c">下载 CSV</a>
</div>
<div class="kv"><span>抓包状态</span><span id="recState">-</span></div>
<div class="kv"><span>帧数</span><span id="recCount">-</span></div>
<div class="kv"><span>PSRAM 缓冲</span><span id="recPsram">-</span></div>
</div>

<div class="card">
<h2>状态 <label style="display:inline;font-size:13px">轮询<input type="checkbox" id="poll" onchange="setPolling(this.checked)"></label></h2>
<div class="kv"><span>CAN1 接收</span><span id="can1Rx">-</span></div>
<div class="kv"><span>CAN1 发送</span><span id="can1Tx">-</span></div>
<div class="kv"><span>CAN1 发送失败</span><span id="can1TxFail">-</span></div>
<div class="kv"><span>TWAI bus-off 次数</span><span id="twaiBusOffCount">-</span></div>
<div class="kv"><span>融合限速 kph</span><span id="fusedLimitKph">-</span></div>
<div class="kv"><span>目标速度 kph</span><span id="targetSpeedKph">-</span></div>
<div class="kv"><span>速度偏移 kph</span><span id="offsetKph">-</span></div>
<div class="kv"><span>速度偏移 raw</span><span id="offsetRaw">-</span></div>
<div class="kv"><span>CAN B 就绪</span><span id="canbReady">-</span></div>
<div class="kv"><span>CAN B 硬件过滤</span><span id="canbHardwareFilterEnabled">-</span></div>
<div class="kv"><span>CAN B 接收</span><span id="canbRx">-</span></div>
<div class="kv"><span>CAN B 发送</span><span id="canbTx">-</span></div>
<div class="kv"><span>CAN B 发送失败</span><span id="canbTxFail">-</span></div>
<div class="kv"><span>CAN B 最后 ID</span><span id="canbLastId">-</span></div>
<div class="kv"><span>0x249 爆闪中</span><span id="highBeamStrobeActive">-</span></div>
<div class="kv"><span>0x249 剩余次数</span><span id="highBeamStrobeRemaining">-</span></div>
<div class="kv"><span>后雾灯爆闪中</span><span id="rearFogBrakeStrobeActive">-</span></div>
<div class="kv"><span>后雾灯剩余次数</span><span id="rearFogBrakeStrobeRemaining">-</span></div>
<div class="kv"><span>运行时间 秒</span><span id="uptime">-</span></div>
<div class="kv"><span>倒挡双闪雾灯中</span><span id="reverseStrobeActive">-</span></div>
<div class="kv"><span>倒挡剩余次数</span><span id="reverseStrobeRemaining">-</span></div>
<div class="kv"><span>电池预热发送中</span><span id="batteryPreheatActive">-</span></div>
</div>

<script>
let pollTimer=null,recTimer=null,loaded=false;
const ids=["fsdEnabled","autoSpeedOffsetEnabled","slewPctPerSec","lowSpeedMaxPctRaw","targetBelow60","target60","target70","target80","target90","target100","target120","canbEnabled","canbServiceModeEnabled","canbFilterEnabled","highBeamStrobeEnabled","rearFogBrakeStrobeEnabled","reverseStrobeEnabled","batteryPreheatEnabled"];
const stats=["can1Rx","can1Tx","can1TxFail","twaiBusOffCount","fusedLimitKph","targetSpeedKph","offsetKph","offsetRaw","canbReady","canbHardwareFilterEnabled","canbRx","canbTx","canbTxFail","canbLastId","highBeamStrobeActive","highBeamStrobeRemaining","rearFogBrakeStrobeActive","rearFogBrakeStrobeRemaining","reverseStrobeActive","reverseStrobeRemaining","batteryPreheatActive","uptime"];
function setVal(id,v){const e=document.getElementById(id);if(!e)return;if(e.type==="checkbox")e.checked=!!v;else e.value=v;}
function showResult(text){const e=document.getElementById("testResult");if(e)e.textContent=text;}
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
function applyConfig(){fetch("/config",{method:"POST",body:body()}).then(async r=>{showResult(await r.text());pollStatus();});}
function saveConfig(){fetch("/config",{method:"POST",body:body()}).then(()=>fetch("/save",{method:"POST"})).then(async r=>{showResult(await r.text());pollStatus();});}
function testCanB(type){
  const p=new URLSearchParams();p.set("type",type);
  fetch("/test",{method:"POST",body:p}).then(async r=>{
    const text=await r.text();
    showResult((r.ok?"成功 ":"失败 ")+type+" "+text);
    pollStatus();
  }).catch(e=>showResult("失败 "+type+" "+e));
}
function recQuery(){
  const ids=document.getElementById("recIds").value.trim();
  return ids?("?ids="+encodeURIComponent(ids)):"";
}
function setRecUi(j){
  const active=j&&j.active;
  document.getElementById("recState").textContent=active?"抓包中":(j&&j.saved?"已保存":"空闲");
  document.getElementById("recCount").textContent=j?(j.count+" / "+j.cap):"-";
  document.getElementById("recPsram").textContent=j?((j.psram?"可用":"不可用")+" / "+Math.round((j.bytes||0)/1024)+" KB"):"-";
  document.getElementById("recDownload").style.display=(!active&&j&&j.saved)?"inline-block":"none";
}
function pollRec(){
  fetch("/rec_status").then(r=>r.json()).then(setRecUi).catch(()=>{});
}
function startRec(){
  fetch("/rec_start"+recQuery(),{method:"POST"}).then(async r=>{
    showResult(await r.text());
    pollRec();
    if(!recTimer)recTimer=setInterval(pollRec,800);
  });
}
function stopRec(){
  fetch("/rec_stop",{method:"POST"}).then(async r=>{
    showResult(await r.text());
    pollRec();
    if(recTimer){clearInterval(recTimer);recTimer=null;}
  });
}
function webOff(){setPolling(false);document.getElementById("poll").checked=false;fetch("/web/off",{method:"POST"});}
pollStatus();
pollRec();
</script>
</body></html>)HTML";
