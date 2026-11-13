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
#toast.show{opacity:1}details{margin:10px 0}summary{cursor:pointer;color:var(--dim)}a{color:var(--accent)}
.photos{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px;margin:8px 0}.ph{position:relative}
.ph img{width:100%;aspect-ratio:4/3;object-fit:cover;border-radius:9px;display:block;cursor:pointer;border:1px solid var(--line);background:#000}
.ph button{position:absolute;top:6px;right:6px;padding:2px 8px;background:#0b0f22d0}
.ph .cap{position:absolute;left:6px;bottom:6px;font-size:12px;background:#0b0f22d0;border-radius:6px;padding:1px 7px;pointer-events:none}
.bar{display:flex;height:12px;border-radius:6px;background:#0b0f22;border:1px solid var(--line);overflow:hidden;margin:4px 0}.bar i{display:block;height:100%}
.bar.big{height:18px;border-radius:6px}.bar.big i{border-right:2px solid var(--card)}.bar.big i:last-child{border-right:0}
.sthead{display:flex;justify-content:space-between;align-items:baseline;margin:14px 0 6px}.sthead span{color:var(--dim);font-size:13px}
.strow{display:grid;grid-template-columns:12px 1fr auto;gap:10px;align-items:baseline;padding:8px 0;border-top:1px solid var(--line)}
.strow .sw{width:12px;height:12px;border-radius:4px}.stv{color:var(--dim);font-variant-numeric:tabular-nums;white-space:nowrap}
.legend{display:flex;flex-wrap:wrap;gap:4px 14px;font-size:13px;color:var(--dim)}.sw{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:5px;vertical-align:-1px}
#phcv{width:100%;max-width:480px;aspect-ratio:4/3;border-radius:9px;touch-action:none;cursor:grab;display:block;background:#000;border:1px solid var(--line)}.big{font-size:28px;font-weight:700}.muted{color:var(--dim);font-size:13px}
</style></head><body><main>
<h1>Chrono <span>Node</span></h1><div class="sub" id="sub">Loading...</div>

<section><h2>Status</h2><div class="big" id="time">--:--:--</div><div class="grid" id="status"></div></section>

<section><h2>Storage</h2><div id="storage"></div></section>

<section><h2>Display</h2>
<div class="row"><label>Day brightness</label><input type="range" id="bday" min="10" max="100" step="5"><span class="val" id="bdayv"></span></div>
<div class="row"><label>Night brightness</label><input type="range" id="bnight" min="0" max="100" step="5"><span class="val" id="bnightv"></span></div>
<div class="row"><label>Night mode</label><select id="non"><option value="ON">On</option><option value="OFF">Off</option></select>
<input type="time" id="nstart"> to <input type="time" id="nend"><button id="nsave">Save</button></div>
<div class="row"><label>Time format</label><select id="fmt"><option value="24">24-hour</option><option value="12">12-hour</option></select></div>
<div class="row"><label>Forecast screen every 2 min</label><select id="cyc"><option value="ON">On</option><option value="OFF">Off</option></select></div>
</section>

<section><h2>Banner</h2>
<div class="row"><label>Scrolling banner</label><select id="bnon"><option value="ON">On</option><option value="OFF">Off</option></select></div>
<div class="row"><label>Speed</label><select id="bnspeed"><option value="45">Slow</option><option value="70">Normal</option><option value="110">Fast</option></select></div>
<div id="bnlist"></div>
<div class="row"><button id="bnadd">+ Add message</button><button class="primary" id="bnsave">Save messages</button><button id="bnshow">Show now</button></div>
<div class="muted">Up to 5 messages, each with its own timer: messages due at the same time scroll one after another, and non-stop ones fill the time in between. Latin and Vietnamese letters are supported (e.g. "Ch&uacute;c m&#7915;ng n&#259;m m&#7899;i"); emoji are not.</div>
</section>

<section><h2>Photos</h2>
<div class="row"><label>Slideshow</label><select id="phevery"><option value="0">Off</option><option value="1">Every minute</option>
<option value="2">Every 2 min</option><option value="5">Every 5 min</option><option value="10">Every 10 min</option><option value="15">Every 15 min</option>
<option value="30">Every 30 min</option><option value="60">Every hour</option></select></div>
<div class="photos" id="phlist"></div>
<div id="phstore"></div>
<div class="row"><input type="file" id="phfile" accept="image/*" hidden><button id="phadd">+ Add photo</button><span class="muted" id="phfree"></span></div>
<div id="phedit" style="display:none"><canvas id="phcv" width="320" height="240"></canvas>
<div class="row"><label>Zoom</label><input type="range" id="phzoom" min="1" max="4" step="0.01" value="1"></div>
<div class="row"><button id="phcancel">Cancel</button><button class="primary" id="phup">Upload to clock</button></div>
<div class="muted">Drag to choose what's in the frame. This is exactly what the clock will show (320&times;240).</div></div>
<div class="muted">Up to 5 photos, kept on the clock (they survive firmware updates). The slideshow shows one for 12 s, never at night.
On the clock, swipe to the Photo screen and tap for the next one. Tap a thumbnail here to show it now.</div>
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
<section><h2>Firmware</h2><div class="muted" id="fwcur"></div>
<div id="rel" class="muted" style="margin:8px 0">Checking GitHub for updates...</div>
<div id="relpin" class="row" style="display:none"><label>Code shown on the clock</label><input id="relpinv" placeholder="0000" size="5" maxlength="4" inputmode="numeric">
<button class="primary" id="relgo">Install</button><button id="relcancel">Cancel</button></div>
<div id="relstate" class="muted"></div>
<details><summary>Install a .bin file instead</summary>
<div class="row"><button id="otapin">1. Show code on the clock</button><input id="otapinv" placeholder="Code" size="5" inputmode="numeric">
<input type="file" id="otafile" accept=".bin" style="flex:2 1 200px"><button class="primary" id="otago">2. Install</button></div>
<progress id="otaprog" max="100" value="0" style="width:100%;display:none"></progress>
<div class="muted">Use <b>chrono-node-fw.bin</b> from a release (not the -full image). The code appears at the bottom of the clock's screen for 2 minutes.</div></details>
<div class="muted" style="margin-top:8px">If new firmware fails within its first minute, the clock goes back to the current version by itself.</div></section>
<div class="muted">This page has no password: anyone on this network can change settings. Firmware updates need the PIN shown on the clock.</div>
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
const selVal=(id,v,label)=>{const e=$(id);v=String(v);if(![...e.options].some(o=>o.value===v))e.add(new Option(label,v));e.value=v};
function toast(t){const e=$("toast");e.textContent=t;e.classList.add("show");clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove("show"),2200)}
async function cmd(c,msg){const r=await fetch("/api/command",{method:"POST",headers:{"Content-Type":"text/plain"},body:c});
if(r.ok){toast(msg||"Saved");setTimeout(loadSettings,500);setTimeout(loadStatus,700)}else{toast("Error: "+((await r.json()).error||r.status))}}
function ago(s){return s<90?"just now":s<5400?Math.round(s/60)+" min ago":Math.round(s/3600)+" h ago"}
function up(s){const d=Math.floor(s/86400),h=Math.floor(s/3600)%24,m=Math.floor(s/60)%60;return(d?d+"d ":"")+h+"h "+m+"m"}
// Updates from GitHub: the browser asks GitHub for the latest release; the
// clock downloads and installs it after the code shown on its screen is typed.
let REL=null,relChecked=false;
const ver=t=>String(t||"").replace(/^v/,"").split(/[.-]/).map(x=>parseInt(x)||0);
function newer(a,b){const x=ver(a),y=ver(b);for(let i=0;i<3;i++){if((x[i]||0)!==(y[i]||0))return(x[i]||0)>(y[i]||0)}return false}
async function checkRelease(st){try{const r=await fetch(`https://api.github.com/repos/${st.release_repo}/releases/latest`,{headers:{Accept:"application/vnd.github+json"}});
if(r.status===404){$("rel").textContent="No releases have been published on GitHub yet.";return}if(!r.ok)throw new Error(r.status);
const j=await r.json(),asset=(j.assets||[]).find(a=>a.name===st.release_asset);REL={tag:j.tag_name};
if(!newer(j.tag_name,st.firmware)){$("rel").textContent=`\u2713 This is the latest version (v${st.firmware}).`;return}
if(!asset){$("rel").textContent=`${j.tag_name} is out, but it has no ${st.release_asset} yet.`;return}
$("rel").innerHTML=`<div class="row"><label><b style="color:var(--text)">${esc(j.tag_name)} is available</b> \u00b7 ${esc((j.published_at||"").slice(0,10))}, ${mb(asset.size)} \u00b7 <a href="${esc(j.html_url)}" target="_blank" rel="noopener">What's new</a></label><button class="primary" id="relinst">Install ${esc(j.tag_name)}</button></div>`;
$("relinst").onclick=startRelease}catch(e){$("rel").textContent="Couldn't reach GitHub to check for updates (is this phone or computer online?)."}}
async function startRelease(){const r=await fetch("/api/ota/pin",{method:"POST"});if(!r.ok){toast("Couldn't show the code on the clock");return}
$("relpin").style.display="flex";$("relpinv").value="";$("relpinv").focus();$("relstate").textContent="Look at the bottom of the clock's screen for a 4-digit code."}
$("relcancel").onclick=()=>{$("relpin").style.display="none";$("relstate").textContent=""};
$("relgo").onclick=async()=>{const pin=$("relpinv").value.trim();if(!/^\d{4}$/.test(pin)){toast("Enter the 4-digit code from the clock");return}
const r=await fetch(`/api/ota/release?tag=${encodeURIComponent(REL.tag)}&pin=${pin}`,{method:"POST"});const d=await r.json().catch(()=>({}));
if(r.status!==202){toast("Couldn't start: "+(d.error||r.status));return}$("relpin").style.display="none";waitForUpdate(REL.tag)};
function waitForUpdate(tag){const want=tag.replace(/^v/,""),t0=Date.now();
$("relstate").innerHTML=`<b style="color:var(--text)">Installing ${esc(tag)}...</b> The clock is downloading it (about a minute) and shows the progress on its screen, then restarts. Keep this page open.`;
const poll=async()=>{let s=null;try{s=await(await fetch("/api/status",{cache:"no-store",signal:AbortSignal.timeout(4000)})).json()}catch(e){}
if(s&&s.firmware===want){$("relstate").textContent=`\u2713 Updated to ${tag}.`;toast("Updated to "+tag);setTimeout(()=>location.reload(),2000);return}
if(s&&s.ota&&s.ota.error&&s.ota.progress<0){$("relstate").textContent=`Update failed: ${s.ota.error}. The clock keeps the current version.`;return}
if(Date.now()-t0>240000){$("relstate").textContent="The clock hasn't come back after 4 minutes. Check its screen; it keeps the current version if anything went wrong.";return}
setTimeout(poll,3000)};setTimeout(poll,5000)}
async function loadStatus(){try{const s=await(await fetch("/api/status")).json();if(!relChecked){relChecked=true;checkRelease(s)}
$("time").textContent=s.local_time.slice(11);$("fwcur").textContent="Installed: v"+s.firmware+" ("+s.revision+")";$("sub").textContent="Firmware "+s.firmware+" ("+s.revision+") \u00b7 "+s.wifi.host;
const w=s.weather?`${s.weather.temp_c}\u00b0C ${s.weather.condition}, ${s.weather.place} (${ago(s.weather.age_s)})`:"no data yet";
const rows=[["Date",s.local_time.slice(0,10)],["Wi-Fi",`${s.wifi.ssid} (${s.wifi.rssi} dBm)`],["IP",s.wifi.ip],["Time sync",s.ntp],
["RTC",s.rtc.present?("DS3231"+(s.rtc.temp_c!==undefined?`, ${s.rtc.temp_c}\u00b0C`:"")+(s.rtc.lost_power?", LOST POWER":"")):"not detected"],
["Weather",w],["Screen",s.screen+(s.night?" (night mode)":"")],["Uptime",up(s.uptime_s)]];
$("storage").innerHTML=flashList(s)+ramList(s);
$("status").innerHTML=rows.map(r=>`<div>${r[0]}</div><div>${r[1].html||esc(String(r[1]))}</div>`).join("")}catch(e){$("sub").textContent="Clock not reachable"}}
const kb=b=>(b<10240?(b/1024).toFixed(1):Math.round(b/1024))+" KB";
const mb=b=>b>=1048576?(b/1048576).toFixed(2)+" MB":kb(b);
// Stacked bar: parts [{v,c,t}] out of total, then a legend with the same colours.
function bar(parts,total,free){return`<div class="bar">${parts.map(p=>`<i style="width:${Math.max(0,100*p.v/total).toFixed(2)}%;background:${p.c}" title="${esc(p.t)}"></i>`).join("")}</div>
<div class="legend">${parts.concat([{c:"#0b0f22;border:1px solid var(--line)",t:"Free "+mb(free)}]).map(p=>`<span><span class="sw" style="background:${p.c}"></span>${esc(p.t)}</span>`).join("")}</div>`}
// Phone-style storage view: header, one continuous bar, then every part with
// its size and what it is. Free space is the last row.
function stList(title,total,parts,freeNote){const used=parts.reduce((a,p)=>a+p.v,0),free=Math.max(0,total-used);
return`<div class="sthead"><b>${title}</b><span>${mb(used)} of ${mb(total)} used</span></div>
<div class="bar big">${parts.map(p=>`<i style="width:${(100*p.v/total).toFixed(2)}%;background:${p.c}"></i>`).join("")}</div>`+
parts.concat([{v:free,c:"#2a3154",n:"Free",d:freeNote}]).map(p=>`<div class="strow"><span class="sw" style="background:${p.c}"></span>
<div>${p.n}<div class="muted">${p.d}</div></div><div class="stv">${mb(p.v)}</div></div>`).join("")}
function flashList(s){const f=s.flash;if(!f)return"";const fwFree=f.slot-f.firmware,phFree=f.photos_total-f.photos_used;
return stList("Flash \u00b7 "+mb(f.chip),f.chip,[
{v:f.firmware,c:"var(--accent)",n:"Firmware",d:`The clock's software, v${esc(s.firmware)}. Uses ${Math.round(100*f.firmware/f.slot)}% of its ${mb(f.slot)} slot.`},
{v:f.update_slot,c:"#8b7bff",n:"Update slot",d:"Kept free for updates: a new version is written here, and the clock goes back to the current one if it fails."},
{v:f.photos_used,c:"#ff5fa2",n:"Photos",d:`Your photos. ${mb(f.photos_total)} is set aside for them.`},
{v:f.system,c:"#6b7194",n:"System",d:"Bootloader, settings (including the Wi-Fi password) and the crash log."}],
`Room for photos (${mb(phFree)}) and for the firmware to grow (${mb(fwFree)}).`)}
function ramList(s){const t=s.heap_total,used=t-s.heap_free,peak=t-s.heap_min;
return stList("RAM \u00b7 "+kb(t),t,[{v:used,c:"var(--accent)",n:"In use",d:`Wi-Fi, web server, display and weather data. The most used since the clock started: ${kb(peak)}.`}],
"Working memory available right now. It's cleared at every restart; nothing is stored here.")}
const PHC=["#ff9a1f","#ff5fa2","#8b7bff","#2ec5ff","#3ddc97"];
function esc(t){return t.replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
let S;
async function loadSettings(){S=await(await fetch("/api/settings")).json();
$("bday").value=S.brightness_day;$("bdayv").textContent=S.brightness_day+"%";$("bnight").value=S.brightness_night;$("bnightv").textContent=S.brightness_night+"%";
$("non").value=S.night.enabled?"ON":"OFF";$("nstart").value=S.night.start;$("nend").value=S.night.end;$("fmt").value=S.hour12?"12":"24";$("cyc").value=S.forecast_cycle?"ON":"OFF";
const known=TZ.find(z=>z[1]===S.timezone);$("tz").value=known?known[0]:"custom";$("tzcustom").value=S.timezone;$("tzcustomrow").style.display=known?"none":"flex";
$("ntp").value=S.ntp_server;
const B=S.banner;$("bnon").value=B.enabled?"ON":"OFF";selVal("bnspeed",B.speed,B.speed+" px/s");
if(!document.activeElement||!document.activeElement.closest(".bnrow")){$("bnlist").innerHTML="";B.messages.forEach(m=>bnRow(m.text,m.every_min));if(!B.messages.length)bnRow("",B.every_min)}phList(S.photos);$("loccur").textContent=`Current: ${S.location.name} (${S.location.lat}, ${S.location.lon})`;
$("recent").innerHTML=S.recent_locations.map(r=>`<button data-c="${r.lat} ${r.lon} ${esc(r.name)}">${esc(r.name)} <small>${r.lat}, ${r.lon}</small></button>`).join("");}
let phSig="";
function phList(P){selVal("phevery",P.every_min,"Every "+P.every_min+" min");const n=P.items.length;
$("phadd").disabled=!P.available||n>=P.max;$("phfree").textContent=P.available?`${n} of ${P.max} photos`:"Photo storage unavailable";
// Files use whole 4 KB blocks; the rest of "used" is file-system overhead.
const parts=P.items.map(i=>({v:i.bytes,c:PHC[(i.slot-1)%5],t:`Photo ${i.slot} ${kb(i.bytes)}`})),sum=P.items.reduce((a,i)=>a+i.bytes,0);
parts.push({v:Math.max(0,P.used_bytes-sum),c:"#4a5072",t:"File system "+kb(Math.max(0,P.used_bytes-sum))});
$("phstore").innerHTML=P.available?`<div class="muted">Photo storage: ${kb(P.used_bytes)} of ${kb(P.total_bytes)} used (a section of the flash chip; see Status)</div>`+bar(parts,P.total_bytes,P.total_bytes-P.used_bytes):"";
const sig=P.items.map(i=>i.slot+":"+i.bytes).join();if(sig===phSig)return;phSig=sig;
$("phlist").innerHTML=P.items.map(i=>`<div class="ph"><img src="/api/photo?n=${i.slot}&v=${i.bytes}" data-show="${i.slot}" title="Show on the clock"><span class="cap"><span class="sw" style="background:${PHC[(i.slot-1)%5]}"></span>Photo ${i.slot} \u00b7 ${kb(i.bytes)}</span><button class="danger" data-del="${i.slot}">&#x2715;</button></div>`).join("")}
$("phlist").onclick=e=>{const t=e.target;if(t.dataset.show)cmd("PHOTO SHOW "+t.dataset.show,"Showing on the clock");
else if(t.dataset.del&&confirm("Delete this photo from the clock?"))cmd("PHOTO DEL "+t.dataset.del,"Photo deleted")};
$("phevery").onchange=e=>cmd("PHOTO EVERY "+e.target.value);
const ph={img:null,z:1,cx:0,cy:0};
function phScale(){return Math.max(320/ph.img.width,240/ph.img.height)*ph.z}
function phDraw(){const i=ph.img,s=phScale(),hw=160/s,hh=120/s;
ph.cx=Math.min(Math.max(ph.cx,hw),i.width-hw);ph.cy=Math.min(Math.max(ph.cy,hh),i.height-hh);
const g=$("phcv").getContext("2d");g.imageSmoothingEnabled=true;g.imageSmoothingQuality="high";g.fillStyle="#000";g.fillRect(0,0,320,240);
g.drawImage(i,ph.cx-hw,ph.cy-hh,2*hw,2*hh,0,0,320,240)}
// Halve big photos first: one large downscale in a canvas looks jagged.
function phSource(im){let c=im,w=im.naturalWidth,h=im.naturalHeight;
while(w/2>=1280&&h/2>=960){w=Math.round(w/2);h=Math.round(h/2);const n=document.createElement("canvas");n.width=w;n.height=h;
const g=n.getContext("2d");g.imageSmoothingQuality="high";g.drawImage(c,0,0,w,h);c=n}return c}
$("phadd").onclick=()=>$("phfile").click();
$("phfile").onchange=e=>{const f=e.target.files[0];e.target.value="";if(!f)return;const im=new Image(),url=URL.createObjectURL(f);
im.onload=()=>{URL.revokeObjectURL(url);ph.img=phSource(im);ph.z=1;$("phzoom").value=1;ph.cx=ph.img.width/2;ph.cy=ph.img.height/2;
$("phedit").style.display="block";phDraw();$("phedit").scrollIntoView({behavior:"smooth",block:"center"})};
im.onerror=()=>{URL.revokeObjectURL(url);toast("This browser can't open that image; try a JPEG or PNG")};im.src=url};
$("phzoom").oninput=e=>{ph.z=parseFloat(e.target.value);phDraw()};
{let drag=null;const c=$("phcv");
c.onpointerdown=e=>{drag={x:e.clientX,y:e.clientY};c.setPointerCapture(e.pointerId);c.style.cursor="grabbing"};
c.onpointermove=e=>{if(!drag)return;const k=320/c.clientWidth/phScale();ph.cx-=(e.clientX-drag.x)*k;ph.cy-=(e.clientY-drag.y)*k;drag={x:e.clientX,y:e.clientY};phDraw()};
c.onpointerup=c.onpointercancel=()=>{drag=null;c.style.cursor="grab"};
c.onwheel=e=>{e.preventDefault();ph.z=Math.min(4,Math.max(1,ph.z*(e.deltaY<0?1.1:1/1.1)));$("phzoom").value=ph.z;phDraw()}}
$("phcancel").onclick=()=>{$("phedit").style.display="none";ph.img=null};
async function phBlob(max){for(const q of[.92,.88,.82,.75,.65,.5]){const b=await new Promise(r=>$("phcv").toBlob(r,"image/jpeg",q));if(b&&b.size<=max)return b}return null}
$("phup").onclick=async()=>{const b=await phBlob(S.photos.max_bytes-1024);if(!b){toast("Couldn't compress the photo enough");return}
const fd=new FormData();fd.append("photo",b,"photo.jpg");$("phup").disabled=true;
try{const r=await fetch("/api/photo",{method:"POST",body:fd});const d=await r.json();if(!r.ok)throw new Error(d.error||r.status);
$("phedit").style.display="none";ph.img=null;await post("PHOTO SHOW "+d.slot);toast(`Saved (${Math.round(b.size/1024)} KB), showing it on the clock`);setTimeout(loadSettings,600)}
catch(e){toast("Upload failed: "+e.message)}finally{$("phup").disabled=false}};
const BNEV=[[0,"Non-stop"],[1,"Every minute"],[2,"Every 2 min"],[5,"Every 5 min"],[10,"Every 10 min"],[15,"Every 15 min"],[30,"Every 30 min"],[60,"Every hour"]];
function bnRow(t,ev){const d=document.createElement("div");d.className="row bnrow";
d.innerHTML='<input class="bnmsg" maxlength="120" style="flex:1 1 220px"><select class="bnev">'+BNEV.map(o=>`<option value="${o[0]}">${o[1]}</option>`).join("")+'</select><button class="danger">&#x2715;</button>';
d.querySelector("input").value=t||"";const s=d.querySelector("select"),v=String(ev??5);
if(![...s.options].some(o=>o.value===v))s.add(new Option("Every "+v+" min",v));s.value=v;
d.querySelector("button").onclick=()=>d.remove();$("bnlist").appendChild(d)}
async function post(c){const r=await fetch("/api/command",{method:"POST",headers:{"Content-Type":"text/plain"},body:c});if(!r.ok)throw new Error((await r.json()).error||r.status)}
$("bnadd").onclick=()=>{if(document.querySelectorAll(".bnmsg").length<5)bnRow("",S.banner.every_min);else toast("Up to 5 messages")};
$("bnsave").onclick=async()=>{const msgs=[...document.querySelectorAll(".bnrow")].map(r=>({t:r.querySelector(".bnmsg").value.trim(),e:r.querySelector(".bnev").value})).filter(m=>m.t);
const enc=new TextEncoder();if(msgs.some(m=>enc.encode(m.t).length>120)){toast("A message is too long (120 bytes max)");return}
try{await post("BANNER CLEAR");for(const[i,m]of msgs.entries()){await post("BANNER ADD "+m.t);await post(`BANNER EVERY ${i+1} ${m.e}`)}
toast("Banner saved");setTimeout(loadSettings,600)}catch(e){toast("Error: "+e.message)}};
$("bnshow").onclick=()=>cmd("BANNER SHOW","Scrolling now");
$("bnon").onchange=e=>cmd("BANNER "+e.target.value);$("bnspeed").onchange=e=>cmd("BANNER SPEED "+e.target.value);
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
$("otapin").onclick=async()=>{const r=await fetch("/api/ota/pin",{method:"POST"});toast(r.ok?"Enter the PIN shown on the clock":"Could not create a PIN");$("otapinv").focus()};
$("otago").onclick=()=>{const f=$("otafile").files[0],pin=$("otapinv").value.trim();
if(!f){toast("Choose a .bin file");return}if(!/^\d{4}$/.test(pin)){toast("Enter the 4-digit PIN from the clock");return}
const fd=new FormData();fd.append("firmware",f,f.name);const x=new XMLHttpRequest(),p=$("otaprog");p.style.display="block";p.value=0;
x.upload.onprogress=e=>{if(e.lengthComputable)p.value=Math.round(e.loaded*100/e.total)};
x.onload=()=>{let d={};try{d=JSON.parse(x.responseText)}catch(e){}
if(x.status===200){toast("Installed. Rebooting...");setTimeout(()=>location.reload(),25000)}else{p.style.display="none";toast("Update failed: "+(d.error||x.status))}};
x.onerror=()=>{p.style.display="none";toast("Upload interrupted")};x.open("POST","/api/ota?pin="+pin);x.send(fd)};
loadSettings();loadStatus();setInterval(loadStatus,5000);
</script></body></html>)HTML";
