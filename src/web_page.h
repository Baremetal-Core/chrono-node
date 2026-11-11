// Chrono Node - embedded web UI (served by web_server.cpp).
#pragma once

#include <pgmspace.h>

const char kWebPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chrono Node</title>
<style>
:root{--bg:#06081a;--card:#11152b;--line:#232a4a;--text:#e6e8f2;--dim:#8a90ad;--accent:#ff9a1f;--err:#ff5466}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 -apple-system,system-ui,Segoe UI,Roboto,sans-serif}
main{max-width:720px;margin:auto;padding:16px}h1{font-size:22px;margin:4px 0 2px}h1 span{color:var(--accent)}
.sub{color:var(--dim);font-size:13px;margin-bottom:14px}
section{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px;margin:12px 0}
h2{font-size:15px;margin:0 0 10px;color:var(--accent);font-weight:600;letter-spacing:.3px}
.row{display:flex;align-items:center;gap:10px;margin:8px 0;flex-wrap:wrap}.row label{flex:1 1 150px;color:var(--dim)}
.grid{display:grid;grid-template-columns:auto 1fr;gap:4px 14px}.grid div:nth-child(odd){color:var(--dim)}
input,select,button{font:inherit;color:var(--text);background:#0b0f22;border:1px solid var(--line);border-radius:9px;padding:7px 10px}
input[type=range]{flex:2 1 180px;padding:0;accent-color:var(--accent)}input[type=time]{width:110px}
button{cursor:pointer;background:#1b2142}button.primary{background:var(--accent);color:#111;border-color:var(--accent);font-weight:600}
button.danger{border-color:var(--err);color:var(--err)}.val{min-width:44px;text-align:right}
.results button,.recent button{display:block;width:100%;text-align:left;margin:6px 0}
.results small,.recent small{color:var(--dim)}#toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);background:#1b2142;border:1px solid var(--line);padding:8px 14px;border-radius:10px;opacity:0;transition:opacity .3s}
#toast.show{opacity:1}.big{font-size:28px;font-weight:700}.muted{color:var(--dim);font-size:13px}
</style></head><body><main>
<h1>Chrono <span>Node</span></h1><div class="sub" id="sub">Loading...</div>

<section><h2>Status</h2><div class="big" id="time">--:--:--</div><div class="grid" id="status"></div></section>

<section><h2>Display</h2>
<div class="row"><label>Day brightness</label><input type="range" id="bday" min="10" max="100" step="5"><span class="val" id="bdayv"></span></div>
<div class="row"><label>Night brightness</label><input type="range" id="bnight" min="0" max="100" step="5"><span class="val" id="bnightv"></span></div>
<div class="row"><label>Night mode</label><select id="non"><option value="ON">On</option><option value="OFF">Off</option></select>
<input type="time" id="nstart"> to <input type="time" id="nend"><button id="nsave">Save</button></div>
<div class="row"><label>Time format</label><select id="fmt"><option value="24">24-hour</option><option value="12">12-hour</option></select></div>
<div class="row"><label>Forecast screen every 2 min</label><select id="cyc"><option value="ON">On</option><option value="OFF">Off</option></select></div>
</section>

<section><h2>Time</h2>
<div class="row"><label>Time zone</label><select id="tz"></select><button id="tzdetect">Detect</button></div>
<div class="row" id="tzcustomrow"><label>POSIX TZ string</label><input id="tzcustom" placeholder="e.g. <+07>-7"><button id="tzsave">Save</button></div>
<div class="row"><label>NTP server</label><input id="ntp"><button id="ntpsave">Save</button><button id="ntpsync">Sync now</button></div>
<div class="row"><label>No internet time?</label><button id="setclock">Set clock from this device</button></div>
</section>

<section><h2>Weather location</h2>
<div class="muted" id="loccur"></div>
<div class="row"><input id="q" placeholder="Search a city or district" style="flex:2 1 220px">
<label style="flex:0 0 auto"><input type="checkbox" id="vn" checked> Vietnam only</label><button class="primary" id="search">Search</button></div>
<div class="results" id="results"></div>
<div class="row"><label>Or coordinates</label><input id="lat" placeholder="lat" size="8"><input id="lon" placeholder="lon" size="8"><input id="lname" placeholder="name (optional)" size="14"><button id="locsave">Save</button></div>
<div class="muted">Recent</div><div class="recent" id="recent"></div>
</section>

<section><h2>Wi-Fi &amp; device</h2>
<div class="row"><button id="portal">Open Wi-Fi setup portal</button><button class="danger" id="forget">Forget Wi-Fi</button><button class="danger" id="reboot">Reboot</button></div>
<div class="muted">The setup portal disconnects the clock from this network; join "Chrono-Node-XXXX" to continue.</div>
</section>
<div class="muted">This page has no password: anyone on this network can change settings.</div>
</main><div id="toast"></div>
<script>
const TZ=[["Asia/Ho_Chi_Minh","<+07>-7"],["Asia/Bangkok","<+07>-7"],["Asia/Jakarta","WIB-7"],["Asia/Singapore","<+08>-8"],
["Asia/Kuala_Lumpur","<+08>-8"],["Asia/Manila","PST-8"],["Asia/Shanghai","CST-8"],["Asia/Hong_Kong","HKT-8"],["Asia/Taipei","CST-8"],
["Asia/Tokyo","JST-9"],["Asia/Seoul","KST-9"],["Asia/Kolkata","IST-5:30"],["Asia/Dubai","<+04>-4"],["Australia/Perth","AWST-8"],
["Australia/Sydney","AEST-10AEDT,M10.1.0,M4.1.0/3"],["Pacific/Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3"],["Europe/London","GMT0BST,M3.5.0/1,M10.5.0"],
["Europe/Berlin","CET-1CEST,M3.5.0,M10.5.0/3"],["Europe/Paris","CET-1CEST,M3.5.0,M10.5.0/3"],["Europe/Amsterdam","CET-1CEST,M3.5.0,M10.5.0/3"],
["Europe/Helsinki","EET-2EEST,M3.5.0/3,M10.5.0/4"],["Europe/Moscow","MSK-3"],["America/New_York","EST5EDT,M3.2.0,M11.1.0"],
["America/Chicago","CST6CDT,M3.2.0,M11.1.0"],["America/Denver","MST7MDT,M3.2.0,M11.1.0"],["America/Phoenix","MST7"],
["America/Los_Angeles","PST8PDT,M3.2.0,M11.1.0"],["America/Sao_Paulo","<-03>3"],["UTC","UTC0"]];
const $=id=>document.getElementById(id);
function toast(t){const e=$("toast");e.textContent=t;e.classList.add("show");clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove("show"),2200)}
async function cmd(c,msg){const r=await fetch("/api/command",{method:"POST",headers:{"Content-Type":"text/plain"},body:c});
if(r.ok){toast(msg||"Saved");setTimeout(loadSettings,500);setTimeout(loadStatus,700)}else{toast("Error: "+((await r.json()).error||r.status))}}
function ago(s){return s<90?"just now":s<5400?Math.round(s/60)+" min ago":Math.round(s/3600)+" h ago"}
function up(s){const d=Math.floor(s/86400),h=Math.floor(s/3600)%24,m=Math.floor(s/60)%60;return(d?d+"d ":"")+h+"h "+m+"m"}
async function loadStatus(){try{const s=await(await fetch("/api/status")).json();
$("time").textContent=s.local_time.slice(11);$("sub").textContent="Firmware "+s.firmware+" ("+s.revision+") \u00b7 "+s.wifi.host;
const w=s.weather?`${s.weather.temp_c}\u00b0C ${s.weather.condition}, ${s.weather.place} (${ago(s.weather.age_s)})`:"no data yet";
const rows=[["Date",s.local_time.slice(0,10)],["Wi-Fi",`${s.wifi.ssid} (${s.wifi.rssi} dBm)`],["IP",s.wifi.ip],["Time sync",s.ntp],
["RTC",s.rtc.present?("DS3231"+(s.rtc.temp_c!==undefined?`, ${s.rtc.temp_c}\u00b0C`:"")+(s.rtc.lost_power?", LOST POWER":"")):"not detected"],
["Weather",w],["Screen",s.screen+(s.night?" (night mode)":"")],["Uptime",up(s.uptime_s)],["Memory",Math.round(s.heap_free/1024)+" KB free"]];
$("status").innerHTML=rows.map(r=>`<div>${r[0]}</div><div>${esc(String(r[1]))}</div>`).join("")}catch(e){$("sub").textContent="Clock not reachable"}}
function esc(t){return t.replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
let S;
async function loadSettings(){S=await(await fetch("/api/settings")).json();
$("bday").value=S.brightness_day;$("bdayv").textContent=S.brightness_day+"%";$("bnight").value=S.brightness_night;$("bnightv").textContent=S.brightness_night+"%";
$("non").value=S.night.enabled?"ON":"OFF";$("nstart").value=S.night.start;$("nend").value=S.night.end;$("fmt").value=S.hour12?"12":"24";$("cyc").value=S.forecast_cycle?"ON":"OFF";
const known=TZ.find(z=>z[1]===S.timezone);$("tz").value=known?known[0]:"custom";$("tzcustom").value=S.timezone;$("tzcustomrow").style.display=known?"none":"flex";
$("ntp").value=S.ntp_server;$("loccur").textContent=`Current: ${S.location.name} (${S.location.lat}, ${S.location.lon})`;
$("recent").innerHTML=S.recent_locations.map(r=>`<button data-c="${r.lat} ${r.lon} ${esc(r.name)}">${esc(r.name)} <small>${r.lat}, ${r.lon}</small></button>`).join("");}
$("tz").innerHTML=TZ.map(z=>`<option value="${z[0]}">${z[0].replace("_"," ")}</option>`).join("")+'<option value="custom">Custom POSIX string...</option>';
$("bday").oninput=e=>$("bdayv").textContent=e.target.value+"%";$("bnight").oninput=e=>$("bnightv").textContent=e.target.value+"%";
$("bday").onchange=e=>cmd("BRIGHT DAY "+e.target.value);$("bnight").onchange=e=>cmd("BRIGHT NIGHT "+e.target.value);
$("non").onchange=e=>cmd("NIGHT "+e.target.value);$("nsave").onclick=()=>cmd(`NIGHT ${$("nstart").value} ${$("nend").value}`);
$("fmt").onchange=e=>cmd("FORMAT "+e.target.value);$("cyc").onchange=e=>cmd("WEATHER CYCLE "+e.target.value);
$("tz").onchange=e=>{const z=TZ.find(t=>t[0]===e.target.value);if(z)cmd("TZ "+z[1],"Time zone: "+z[0]);else $("tzcustomrow").style.display="flex"};
$("tzsave").onclick=()=>cmd("TZ "+$("tzcustom").value.trim());
$("tzdetect").onclick=()=>{const n=Intl.DateTimeFormat().resolvedOptions().timeZone,z=TZ.find(t=>t[0]===n);
if(z){$("tz").value=z[0];cmd("TZ "+z[1],"Time zone: "+z[0])}else toast(n+" isn't in the list: use a custom POSIX string")};
$("ntpsave").onclick=()=>cmd("NTP SERVER "+$("ntp").value.trim());$("ntpsync").onclick=()=>cmd("NTP SYNC","Syncing...");
$("setclock").onclick=()=>cmd("SETEPOCH "+Math.round(Date.now()/1000),"Clock set from this device");
$("search").onclick=async()=>{const q=$("q").value.trim();if(q.length<2)return;$("results").innerHTML='<div class="muted">Searching...</div>';
const r=await fetch("/api/geocode?q="+encodeURIComponent(q)+($("vn").checked?"&country=VN":""));const d=await r.json();
if(!r.ok){$("results").innerHTML=`<div class="muted">${esc(d.error||"Search failed")}</div>`;return}
const list=d.results||[];$("results").innerHTML=list.length?list.map(x=>{const where=[x.admin2,x.admin1,x.country].filter(Boolean).join(", ");
const pop=x.population?" \u00b7 pop. "+x.population.toLocaleString():"";
return`<button data-c="${x.latitude.toFixed(4)} ${x.longitude.toFixed(4)} ${esc(x.name)}">${esc(x.name)} <small>${esc(where)}${pop}</small></button>`}).join(""):'<div class="muted">No matches</div>'};
$("q").onkeydown=e=>{if(e.key==="Enter")$("search").click()};
document.addEventListener("click",e=>{const b=e.target.closest("button[data-c]");if(b){cmd("WEATHER LOC "+b.dataset.c,"Location saved");$("results").innerHTML=""}});
$("locsave").onclick=()=>{const la=parseFloat($("lat").value),lo=parseFloat($("lon").value);if(isNaN(la)||isNaN(lo)){toast("Enter latitude and longitude");return}
cmd(`WEATHER LOC ${la} ${lo} ${$("lname").value.trim()}`.trim(),"Location saved")};
$("portal").onclick=()=>confirm("Open the setup portal? The clock leaves this network until set up again.")&&cmd("WIFI PORTAL","Portal opening");
$("forget").onclick=()=>confirm("Erase the saved Wi-Fi network?")&&cmd("WIFI FORGET","Wi-Fi forgotten");
$("reboot").onclick=()=>confirm("Reboot the clock?")&&cmd("REBOOT","Rebooting...");
loadSettings();loadStatus();setInterval(loadStatus,5000);
</script></body></html>)HTML";
