// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------
//
// JouleOTA UI — drag-drop firmware updater with progress ring, push/pull
// modes, A/B rollback, and live device-info panel. Single self-contained
// page, mobile-friendly, glass-morphism. Auto-themes to system pref.
#pragma once
#include <Arduino.h>

namespace joule {

static const char OTA_UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<meta name="theme-color" content="#0b0d12"/>
<title>__TITLE__</title>
<style>
:root{
  --bg:#0a0c12;--panel:rgba(255,255,255,.04);--panel-s:#141a2a;
  --ink:#e8ecf5;--ink2:#a5acc1;--muted:#6b7390;--line:rgba(255,255,255,.08);
  --brand:__BRAND__;--brand-2:#22d3ee;--ok:#3ddc97;--warn:#ffb347;--err:#ff6b81;
  --r:18px;--shadow:0 12px 40px rgba(0,0,0,.3);
  --grad:linear-gradient(135deg,var(--brand),var(--brand-2));
}
:root[data-theme="light"]{--bg:#f4f6fc;--panel:rgba(255,255,255,.75);--panel-s:#fff;--ink:#0f1730;--ink2:#3a4366;--line:rgba(15,23,48,.08);--shadow:0 10px 28px rgba(20,32,80,.08)}
@media(prefers-color-scheme:light){:root[data-theme="auto"]{--bg:#f4f6fc;--panel:rgba(255,255,255,.75);--panel-s:#fff;--ink:#0f1730;--ink2:#3a4366;--line:rgba(15,23,48,.08);--shadow:0 10px 28px rgba(20,32,80,.08)}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}html,body{margin:0;height:100%}
body{font:14.5px/1.5 -apple-system,BlinkMacSystemFont,"Inter","Segoe UI",Roboto,sans-serif;color:var(--ink);background:var(--bg);
  background-image:radial-gradient(1200px 600px at 10% -10%,color-mix(in srgb,var(--brand) 18%,transparent),transparent 60%),radial-gradient(1000px 500px at 110% 10%,color-mix(in srgb,var(--brand-2) 14%,transparent),transparent 55%);
  background-attachment:fixed;min-height:100vh}
.wrap{max-width:760px;margin:0 auto;padding:18px}
header{display:flex;align-items:center;gap:12px;margin-bottom:18px}
.logo{width:38px;height:38px;border-radius:12px;background:var(--grad);display:grid;place-items:center;color:#fff;font-weight:800;font-size:16px;box-shadow:0 6px 20px color-mix(in srgb,var(--brand) 40%,transparent)}
h1{margin:0;font-size:18px;font-weight:700}
.sub{font-size:12px;color:var(--muted)}
.spacer{flex:1}
.iconbtn{width:36px;height:36px;border-radius:10px;border:1px solid var(--line);background:var(--panel);color:var(--ink);display:grid;place-items:center;cursor:pointer;transition:.15s}
.iconbtn:hover{border-color:var(--brand);color:var(--brand)}
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);padding:18px;margin-bottom:14px;backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);box-shadow:var(--shadow)}
.cardh{font-weight:700;margin-bottom:10px;display:flex;align-items:center;gap:8px}
.cardh .badge{font-size:10px;text-transform:uppercase;letter-spacing:.6px;padding:2px 8px;border-radius:99px;background:color-mix(in srgb,var(--brand) 18%,transparent);color:var(--brand)}
.grid2{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
@media(max-width:520px){.grid2{grid-template-columns:1fr}}
.kv{display:flex;justify-content:space-between;padding:8px 10px;border-radius:10px;background:color-mix(in srgb,var(--line) 50%,transparent);font-size:13px}
.kv .k{color:var(--muted)}
.kv .v{font-family:"SF Mono",ui-monospace,Menlo,monospace;font-size:12.5px;font-weight:600}
.tabs{display:flex;gap:6px;margin-bottom:14px}
.tab{flex:1;padding:10px 14px;border:1px solid var(--line);background:var(--panel);color:var(--ink2);border-radius:12px;cursor:pointer;font:inherit;font-size:13px;font-weight:600;transition:.18s;min-height:42px}
.tab.active{background:var(--grad);color:#fff;border-color:transparent;box-shadow:0 6px 18px color-mix(in srgb,var(--brand) 30%,transparent)}
.drop{border:2px dashed var(--line);border-radius:var(--r);padding:28px 18px;text-align:center;cursor:pointer;transition:.2s;position:relative;overflow:hidden}
.drop:hover,.drop.hover{border-color:var(--brand);background:color-mix(in srgb,var(--brand) 6%,transparent)}
.drop svg{width:48px;height:48px;color:var(--brand);margin-bottom:8px}
.drop .big{font-size:16px;font-weight:700;margin-bottom:4px}
.drop .small{color:var(--muted);font-size:12px}
input[type=file]{display:none}
.ring{display:flex;align-items:center;gap:18px;margin-top:14px}
.ring svg{width:96px;height:96px;flex-shrink:0}
.ring-info{flex:1}
.ring-info .pct{font-size:24px;font-weight:800;font-family:"SF Mono",monospace;background:var(--grad);-webkit-background-clip:text;background-clip:text;-webkit-text-fill-color:transparent}
.ring-info .stat{font-size:12px;color:var(--muted);margin-top:4px}
.log{max-height:140px;overflow:auto;font-family:"SF Mono",ui-monospace,Menlo,monospace;font-size:11.5px;color:var(--ink2);background:color-mix(in srgb,#000 20%,transparent);padding:10px 12px;border-radius:10px;margin-top:14px;white-space:pre-wrap;border:1px solid var(--line)}
input[type=text],input[type=url]{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--line);background:var(--panel);color:var(--ink);font:inherit;font-size:13px;font-family:"SF Mono",ui-monospace,monospace;outline:none;min-height:42px}
input:focus{border-color:var(--brand);box-shadow:0 0 0 3px color-mix(in srgb,var(--brand) 20%,transparent)}
.btn{display:inline-flex;align-items:center;gap:8px;padding:11px 16px;border:0;border-radius:12px;background:var(--grad);color:#fff;font:inherit;font-weight:700;cursor:pointer;font-size:13px;min-height:42px;box-shadow:0 4px 14px color-mix(in srgb,var(--brand) 28%,transparent);transition:.18s}
.btn:hover{transform:translateY(-1px);box-shadow:0 8px 20px color-mix(in srgb,var(--brand) 38%,transparent)}
.btn:active{transform:translateY(0)}
.btn:disabled{opacity:.5;cursor:not-allowed;transform:none}
.btn.ghost{background:var(--panel);color:var(--ink);border:1px solid var(--line);box-shadow:none}
.btn.danger{background:linear-gradient(135deg,#ff5470,#ff7e7e);color:#fff;box-shadow:0 4px 14px rgba(255,80,100,.3)}
.btn.danger:hover{box-shadow:0 8px 20px rgba(255,80,100,.4)}
.toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%) translateY(20px);background:var(--panel-s);border:1px solid var(--line);padding:12px 18px;border-radius:12px;opacity:0;transition:.25s;pointer-events:none;z-index:9;box-shadow:var(--shadow);font-weight:600}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}.toast.ok{border-color:var(--ok)}.toast.err{border-color:var(--err)}
footer{text-align:center;color:var(--muted);font-size:11px;margin:24px 0}
.row{display:flex;gap:10px;flex-wrap:wrap}
.row>*{flex:1;min-width:120px}
</style></head><body>
<div class="wrap">
<header>
  <div class="logo">⚡</div>
  <div><h1>__TITLE__</h1><div class="sub">JouleOTA · drag a firmware to flash</div></div>
  <div class="spacer"></div>
  <button class="iconbtn" id="themeBtn" title="theme">◐</button>
</header>

<div class="card">
  <div class="cardh">Device <span class="badge" id="badgeMode">live</span></div>
  <div class="grid2">
    <div class="kv"><span class="k">Hardware ID</span><span class="v" id="hwId">—</span></div>
    <div class="kv"><span class="k">Firmware</span><span class="v" id="fwVer">—</span></div>
    <div class="kv"><span class="k">Current slot</span><span class="v" id="curSlot">—</span></div>
    <div class="kv"><span class="k">Next slot</span><span class="v" id="nxtSlot">—</span></div>
    <div class="kv"><span class="k">Free for OTA</span><span class="v" id="freeSpace">—</span></div>
    <div class="kv"><span class="k">Free heap</span><span class="v" id="heap">—</span></div>
  </div>
</div>

<div class="card">
  <div class="tabs">
    <button class="tab active" data-mode="firmware">🔧 Firmware</button>
    <button class="tab" data-mode="filesystem">📁 Filesystem</button>
    <button class="tab" data-mode="pull">☁︎ Pull URL</button>
  </div>

  <div id="dropPane">
    <label class="drop" id="dropZone" for="fileInput">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M12 3v12m0 0l-4-4m4 4l4-4M5 17v2a2 2 0 002 2h10a2 2 0 002-2v-2" stroke-linecap="round" stroke-linejoin="round"/></svg>
      <div class="big">Drop a .bin here</div>
      <div class="small">or click to browse · accepts .bin and .bin.gz</div>
    </label>
    <input type="file" id="fileInput" accept=".bin,.gz,.bin.gz"/>

    <div class="ring" id="ringArea" style="display:none">
      <svg viewBox="0 0 100 100">
        <circle cx="50" cy="50" r="44" fill="none" stroke="var(--line)" stroke-width="8"/>
        <circle id="ringFill" cx="50" cy="50" r="44" fill="none" stroke="url(#og)" stroke-width="8"
                stroke-linecap="round" stroke-dasharray="0 999" transform="rotate(-90 50 50)"/>
        <defs><linearGradient id="og" x1="0" x2="1" y1="0" y2="1"><stop offset="0" stop-color="var(--brand)"/><stop offset="1" stop-color="var(--brand-2)"/></linearGradient></defs>
      </svg>
      <div class="ring-info">
        <div class="pct" id="progText">0%</div>
        <div class="stat" id="byteText">—</div>
        <div class="stat" id="speedText"></div>
      </div>
    </div>
    <div class="log" id="logPane" style="display:none"></div>
  </div>

  <div id="pullPane" style="display:none">
    <input type="url" id="pullUrl" placeholder="https://example.com/firmware.bin"/>
    <div class="row" style="margin-top:10px">
      <button class="btn" id="pullBtn">☁︎ Pull &amp; flash</button>
      <button class="btn ghost" id="pullCancel">Cancel</button>
    </div>
  </div>
</div>

<div class="card">
  <div class="cardh">Maintenance</div>
  <div class="sub" style="margin-bottom:12px">Commit marks the current firmware as known-good. Rollback returns to the previous slot.</div>
  <div class="row">
    <button class="btn ghost" id="commitBtn">✓ Commit current</button>
    <button class="btn danger" id="rollbackBtn">↺ Rollback</button>
  </div>
</div>

<footer>JouleOTA · MIT · ESP32 / ESP8266 · Chinmoy Bhuyan</footer>
</div>
<div class="toast" id="toast"></div>

<script>
const $=s=>document.querySelector(s);
let mode="firmware",running=false,startT=0;

function toast(m,k){const t=$("#toast");t.textContent=m;t.className="toast show "+(k||"");setTimeout(()=>t.className="toast",2400)}
function log(line){const p=$("#logPane");p.style.display="";p.textContent+=line+"\n";p.scrollTop=p.scrollHeight}
function fmtBytes(n){if(!n)return"—";if(n<1024)return n+" B";if(n<1048576)return (n/1024).toFixed(1)+" KB";return (n/1048576).toFixed(2)+" MB"}
function setProgress(pct){
  const c=2*Math.PI*44,len=(pct/100)*c;
  $("#ringFill").setAttribute("stroke-dasharray",`${len} 999`);
  $("#progText").textContent=pct.toFixed(1)+"%";
}

function applyTheme(){const m=localStorage.getItem("joule-theme")||"auto";document.documentElement.setAttribute("data-theme",m)}
$("#themeBtn").onclick=()=>{const c=document.documentElement.getAttribute("data-theme")||"auto";const n=c==="auto"?"dark":(c==="dark"?"light":"auto");localStorage.setItem("joule-theme",n);applyTheme();toast("Theme: "+n)};
applyTheme();

async function refreshInfo(){
  try{
    const r=await fetch("/ota/info"),j=await r.json();
    $("#hwId").textContent=j.hwId||"(none)";$("#fwVer").textContent=j.fwVersion||"-";
    $("#freeSpace").textContent=fmtBytes(j.freeOta||0);
    $("#curSlot").textContent=j.currentSlot||"-";$("#nxtSlot").textContent=j.nextSlot||"-";
    $("#heap").textContent=fmtBytes(j.freeHeap||0);
    $("#badgeMode").textContent=j.updating?"updating":"live";
  }catch(e){}
}

document.querySelectorAll(".tab").forEach(b=>b.onclick=()=>{
  document.querySelectorAll(".tab").forEach(x=>x.classList.remove("active"));
  b.classList.add("active");mode=b.dataset.mode;
  $("#dropPane").style.display=(mode==="pull")?"none":"";
  $("#pullPane").style.display=(mode==="pull")?"":"none";
});

const dz=$("#dropZone"),fi=$("#fileInput");
["dragenter","dragover"].forEach(e=>dz.addEventListener(e,ev=>{ev.preventDefault();dz.classList.add("hover")}));
["dragleave","drop"].forEach(e=>dz.addEventListener(e,ev=>{ev.preventDefault();dz.classList.remove("hover")}));
dz.addEventListener("drop",ev=>{if(ev.dataTransfer.files[0])upload(ev.dataTransfer.files[0])});
fi.addEventListener("change",()=>{if(fi.files[0])upload(fi.files[0])});

function upload(file){
  if(running){toast("upload in progress","err");return}
  if(mode==="pull"){toast("switch to firmware or filesystem tab","err");return}
  running=true;startT=Date.now();
  $("#ringArea").style.display="";$("#logPane").textContent="";setProgress(0);
  log("→ "+file.name+" ("+fmtBytes(file.size)+") as "+mode);
  const fd=new FormData();fd.append("update",file);
  const xhr=new XMLHttpRequest();
  xhr.upload.onprogress=e=>{
    if(!e.lengthComputable)return;
    const pct=(e.loaded*100/e.total);setProgress(pct);
    $("#byteText").textContent=fmtBytes(e.loaded)+" / "+fmtBytes(e.total);
    const dt=(Date.now()-startT)/1000;
    if(dt>0.4)$("#speedText").textContent=fmtBytes(e.loaded/dt)+"/s";
  };
  xhr.onload=()=>{running=false;
    if(xhr.status===200){setProgress(100);$("#byteText").textContent="complete — rebooting";toast("Update applied","ok");log("✓ OK — device rebooting");setTimeout(()=>location.reload(),5000)}
    else{toast("Update failed: "+xhr.status,"err");log("✗ "+xhr.status+" "+xhr.responseText)}
  };
  xhr.onerror=()=>{running=false;toast("network error","err");log("✗ network error")};
  xhr.open("POST","/ota/upload?mode="+mode);xhr.send(fd);
}

$("#pullBtn").onclick=async()=>{
  const url=$("#pullUrl").value.trim();if(!url){toast("enter a URL","err");return}
  $("#pullBtn").disabled=true;
  const r=await fetch("/ota/pull",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({url,mode:"firmware"})});
  $("#pullBtn").disabled=false;
  toast(r.ok?"Pull queued":"Pull rejected",r.ok?"ok":"err");
};
$("#commitBtn").onclick=async()=>{const r=await fetch("/ota/commit",{method:"POST"});toast(r.ok?"Committed":"Commit failed",r.ok?"ok":"err");refreshInfo()};
$("#rollbackBtn").onclick=async()=>{if(!confirm("Roll back to the previous firmware? The device will reboot."))return;const r=await fetch("/ota/rollback",{method:"POST"});toast(r.ok?"Rolling back…":"Rollback failed",r.ok?"ok":"err")};

try{
  const es=new EventSource("/ota/events");
  es.addEventListener("progress",e=>{const j=JSON.parse(e.data);if(j.total){setProgress(j.written*100/j.total);$("#byteText").textContent="(remote) "+fmtBytes(j.written)+" / "+fmtBytes(j.total)}});
  es.addEventListener("status",e=>log("• "+e.data));
}catch(e){}

refreshInfo();setInterval(refreshInfo,4000);
</script>
</body></html>)HTML";

} // namespace joule
