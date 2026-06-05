// Light WebUI page served by /. Kept in a separate header so the PlatformIO
// .ino prototype generator never has to parse the JavaScript braces.
#pragma once

static const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>T-2CAN FSD 设置</title>
<style>
body{font-family:system-ui,Arial,sans-serif;margin:0;padding:12px;background:#111;color:#eee}
h1{font-size:18px;margin:0 0 12px}h2{font-size:15px;margin:0 0 8px;color:#8cf}h3{font-size:13px;margin:12px 0 4px;color:#6a9;border-top:1px solid #2a2a2a;padding-top:8px}
.card{background:#1c1c1c;border:1px solid #333;border-radius:8px;padding:10px 12px;margin-bottom:12px}
label{display:flex;justify-content:space-between;align-items:center;margin:6px 0;font-size:14px;gap:12px}
input[type=number]{width:90px;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:3px}
input[type=text],select{width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:4px;padding:6px;margin-top:4px}
button{background:#2a6;color:#fff;border:0;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0;font-size:14px}
button.alt,.linkbtn{background:#37c}button.warn{background:#a33}.linkbtn{display:none;color:#fff;text-decoration:none;border-radius:6px;padding:8px 12px;margin:4px 4px 0 0}
.kv{display:flex;justify-content:space-between;font-size:13px;padding:3px 0;border-bottom:1px solid #262626;gap:12px}.kv span:last-child{color:#9f9;font-variant-numeric:tabular-nums;text-align:right}
.hint{font-size:12px;color:#aaa;margin:2px 0 6px;line-height:1.45}.result{font-size:12px;color:#ffd479;margin:8px 0 0;min-height:18px}.bar{position:sticky;top:0;z-index:5;background:#111;padding:6px 0 4px}
</style></head><body>
<h1>T-2CAN FSD 运行参数</h1>

<div class="card bar">
<button onclick="applyConfig()">应用到内存</button>
<button class="alt" onclick="saveConfig()">保存到 Flash</button>
<button class="warn" onclick="webOff()">关闭 WebUI</button>
<div class="result" id="testResult"></div>
</div>

<div class="card">
<h2>通道定义</h2>
<p class="hint">官方 LILYGO T-2CAN V1.0：物理 CANA = MCP2515/SPI；物理 CANB = ESP32-S3 原生 TWAI。当前固件 CSV：bus=1/TWAI/物理CANB，bus=2/MCP2515/物理CANA。</p>
<div class="kv"><span>bus=1</span><span>TWAI / physical CANB / GPIO7,6</span></div>
<div class="kv"><span>bus=2</span><span>MCP2515 / physical CANA / SPI + INT8</span></div>
</div>

<div class="card">
<h2>FSD / 速度</h2>
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
</div>

<div class="card">
<h2>灯光 / 车身</h2>
<label>高光爆闪启用<input type="checkbox" id="highBeamStrobeEnabled"></label>
<p class="hint">bus=2/MCP2515/物理CANA，0x249，超车灯拨杆下拉两次触发 8 次，75ms ON / 75ms OFF，结束强制 idle。</p>
<label>后雾灯刹车爆闪启用<input type="checkbox" id="rearFogBrakeStrobeEnabled"></label>
<p class="hint">缓减速触发 3 次，急减速或车身刹车灯触发 6 次，输出使用 0x273 后雾灯位。</p>
<label>倒挡双闪雾灯启用<input type="checkbox" id="reverseStrobeEnabled"></label>
<p class="hint">R 档或 踩刹车+右滚轮后滚 触发 hazard + 后雾灯；右滚轮前滚视为 D 意图并取消。</p>
</div>

<div class="card">
<h2>滚轮换挡 / 预热 / MCP2515</h2>
<label>滚轮换挡启用<input type="checkbox" id="scrollGearInjectEnabled"></label>
<p class="hint">踩刹车 + 右滚轮，bus=2/MCP2515/物理CANA 发送 0x229，默认关闭。</p>
<label>电池预热启用<input type="checkbox" id="batteryPreheatEnabled"></label>
<p class="hint">当前固件：bus=2/MCP2515/物理CANA 每 1000ms 发送 0x082 ON；关闭后补发 OFF。抓包中 0x082 出现在 bus=2，所以当前发送路线与抓包一致。</p>
<label>bus=1/TWAI/物理CANB 只收不发<input type="checkbox" id="can1ReceiveOnly"></label>
<p class="hint">开启后只屏蔽 TWAI 发送；MCP2515/物理CANA 上的灯光、滚轮和 0x082 不受这个开关阻断。</p>
<h3>MCP2515 / 物理 CANA</h3>
<label>启用<input type="checkbox" id="canbEnabled"></label>
<label>维修模式 0x339<input type="checkbox" id="canbServiceModeEnabled"></label>
<label>硬件过滤模式<select id="canbFilterMode"><option value="0">接收全部：调试/抓包</option><option value="1">功能相关 ID：含 0x082</option><option value="2">最小运行 ID</option></select></label>
</div>

<div class="card">
<h2>CAN 抓包</h2>
<p class="hint">ID 留空=不做录制层过滤；bus=1/TWAI 仍只记录固件关注的 FSD/底盘 ID，bus=2/MCP2515 是否全量取决于上方硬件过滤模式。多个 ID 用逗号分隔，例如 229,082,273。CSV 会输出 controller 和 physical 两列。</p>
<input type="text" id="recIds" value="">
<div>
<button onclick="startRec()">开始抓包</button>
<button class="warn" onclick="stopRec()">停止</button>
<a class="linkbtn" id="recDownload" href="/rec_download" download="can_recording.csv">下载 CSV</a>
</div>
<div class="kv"><span>状态</span><span id="recState">-</span></div>
<div class="kv"><span>帧数</span><span id="recCount">-</span></div>
<div class="kv"><span>bus=1/TWAI/物理CANB</span><span id="recBus1">-</span></div>
<div class="kv"><span>bus=2/MCP2515/物理CANA</span><span id="recBus2">-</span></div>
<div class="kv"><span>丢帧/停止原因</span><span id="recDrop">-</span></div>
<div class="kv"><span>PSRAM 缓冲</span><span id="recPsram">-</span></div>
</div>

<div class="card">
<h2>状态 <label style="display:inline;font-size:13px">轮询<input type="checkbox" id="poll" onchange="setPolling(this.checked)"></label></h2>
<h3>CAN 总线</h3>
<div class="kv"><span>bus=1 RX</span><span id="can1Rx">-</span></div>
<div class="kv"><span>bus=1 TX</span><span id="can1Tx">-</span></div>
<div class="kv"><span>bus=1 TX fail</span><span id="can1TxFail">-</span></div>
<div class="kv"><span>TWAI state / bus-off</span><span><b id="twaiState">-</b> / <b id="twaiBusOffCount">-</b></span></div>
<div class="kv"><span>MCP2515 ready</span><span id="canbReady">-</span></div>
<div class="kv"><span>MCP2515 filter mode</span><span id="canbHardwareFilterMode">-</span></div>
<div class="kv"><span>MCP2515 RX / TX / fail</span><span><b id="canbRx">-</b> / <b id="canbTx">-</b> / <b id="canbTxFail">-</b></span></div>
<div class="kv"><span>MCP2515 last ID</span><span id="canbLastId">-</span></div>
<div class="kv"><span>MCP2515 EFLG / RX overflow</span><span><b id="canbErrorFlags">-</b> / <b id="canbRxOverflowCount">-</b></span></div>
<h3>速度 / FSD</h3>
<div class="kv"><span>融合限速 kph</span><span id="fusedLimitKph">-</span></div>
<div class="kv"><span>目标速度 kph</span><span id="targetSpeedKph">-</span></div>
<div class="kv"><span>速度偏移 kph/raw</span><span><b id="offsetKph">-</b> / <b id="offsetRaw">-</b></span></div>
<h3>灯光 / 预热</h3>
<div class="kv"><span>高光爆闪 / 剩余</span><span><b id="highBeamStrobeActive">-</b> / <b id="highBeamStrobeRemaining">-</b></span></div>
<div class="kv"><span>后雾灯爆闪 / 剩余</span><span><b id="rearFogBrakeStrobeActive">-</b> / <b id="rearFogBrakeStrobeRemaining">-</b></span></div>
<div class="kv"><span>倒挡爆闪 / 剩余</span><span><b id="reverseStrobeActive">-</b> / <b id="reverseStrobeRemaining">-</b></span></div>
<div class="kv"><span>电池预热发送中</span><span id="batteryPreheatActive">-</span></div>
<h3>滚轮换挡</h3>
<div class="kv"><span>当前挡位 0x118</span><span id="currentGear">-</span></div>
<div class="kv"><span>刹车 / 车速</span><span><b id="brakeActive">-</b> / <b id="vehicleSpeedKph">-</b></span></div>
<div class="kv"><span>右滚轮 ticks 0x3C2</span><span id="rightScrollTicks">-</span></div>
<div class="kv"><span>右拨杆 status/counter 0x229</span><span><b id="rightStalkStatus">-</b> / <b id="rightStalkCounter">-</b></span></div>
<div class="kv"><span>换挡意图 / 注入中 / 目标</span><span><b id="scrollGearIntent">-</b> / <b id="scrollGearInjectActive">-</b> / <b id="scrollGearInjectTarget">-</b></span></div>
<div class="kv"><span>结果确认 / 阻止原因</span><span><b id="scrollGearInjectOk">-</b> / <b id="scrollGearInjectBlocked">-</b></span></div>
<h3>系统</h3>
<div class="kv"><span>运行时间 秒</span><span id="uptime">-</span></div>
</div>

<script>
let pollTimer=null,recTimer=null,loaded=false;
const cfgIds=["fsdEnabled","autoSpeedOffsetEnabled","slewPctPerSec","lowSpeedMaxPctRaw","targetBelow60","target60","target70","target80","target90","target100","target120","canbEnabled","canbServiceModeEnabled","canbFilterMode","highBeamStrobeEnabled","rearFogBrakeStrobeEnabled","reverseStrobeEnabled","batteryPreheatEnabled","scrollGearInjectEnabled","can1ReceiveOnly"];
const stats=["can1Rx","can1Tx","can1TxFail","twaiState","twaiBusOffCount","fusedLimitKph","targetSpeedKph","offsetKph","offsetRaw","canbReady","canbHardwareFilterMode","canbRx","canbTx","canbTxFail","canbLastId","canbErrorFlags","canbRxOverflowCount","highBeamStrobeActive","highBeamStrobeRemaining","rearFogBrakeStrobeActive","rearFogBrakeStrobeRemaining","reverseStrobeActive","reverseStrobeRemaining","batteryPreheatActive","currentGear","brakeActive","vehicleSpeedKph","rightScrollTicks","rightStalkStatus","rightStalkCounter","scrollGearIntent","scrollGearInjectActive","scrollGearInjectTarget","scrollGearInjectOk","scrollGearInjectBlocked","uptime"];
function setVal(id,v){const e=document.getElementById(id);if(!e)return;if(e.type==="checkbox")e.checked=!!v;else e.value=v;}
function getVal(e){return e.type==="checkbox"?(e.checked?1:0):e.value}
function showResult(text){const e=document.getElementById("testResult");if(e)e.textContent=text}
function pollStatus(){fetch("/status").then(r=>r.json()).then(j=>{stats.forEach(k=>{const e=document.getElementById(k);if(e&&k in j)e.textContent=(k==="canbLastId"||k==="canbErrorFlags")?("0x"+(j[k]>>>0).toString(16)):j[k]});if(!loaded){cfgIds.forEach(k=>{if(k in j)setVal(k,j[k])});loaded=true}}).catch(()=>{})}
function setPolling(on){if(on&&!pollTimer){pollStatus();pollTimer=setInterval(pollStatus,1000)}if(!on&&pollTimer){clearInterval(pollTimer);pollTimer=null}}
function body(){const p=new URLSearchParams();cfgIds.forEach(k=>{const e=document.getElementById(k);p.set(k,getVal(e))});return p}
function applyConfig(){fetch("/config",{method:"POST",body:body()}).then(async r=>{showResult(await r.text());pollStatus()})}
function saveConfig(){fetch("/config",{method:"POST",body:body()}).then(()=>fetch("/save",{method:"POST"})).then(async r=>{showResult(await r.text());pollStatus()})}
function recQuery(){const ids=document.getElementById("recIds").value.trim();return ids?("?ids="+encodeURIComponent(ids)):""}
function stopReason(v){return v===1?"满":(v===2?"超时":"手动/无")}
function setRecUi(j){const active=j&&j.active;document.getElementById("recState").textContent=active?"抓包中":(j&&j.saved?"已保存":"空闲");document.getElementById("recCount").textContent=j?(j.count+" / "+j.cap):"-";document.getElementById("recBus1").textContent=j?j.bus1:"-";document.getElementById("recBus2").textContent=j?j.bus2:"-";document.getElementById("recDrop").textContent=j?(j.dropped+" / "+stopReason(j.stopReason)):"-";document.getElementById("recPsram").textContent=j?((j.psram?"可用":"不可用")+" / "+Math.round((j.bytes||0)/1024)+" KB"):"-";document.getElementById("recDownload").style.display=(!active&&j&&j.saved)?"inline-block":"none"}
function pollRec(){fetch("/rec_status").then(r=>r.json()).then(setRecUi).catch(()=>{})}
function startRec(){fetch("/rec_start"+recQuery(),{method:"POST"}).then(async r=>{showResult(await r.text());pollRec();if(!recTimer)recTimer=setInterval(pollRec,800)})}
function stopRec(){fetch("/rec_stop",{method:"POST"}).then(async r=>{showResult(await r.text());pollRec();if(recTimer){clearInterval(recTimer);recTimer=null}})}
function webOff(){setPolling(false);document.getElementById("poll").checked=false;fetch("/web/off",{method:"POST"})}
pollStatus();pollRec();
</script>
</body></html>)HTML";
