// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The page the headless server serves at GET /: a spectrum and waterfall, the receiver's controls, a Listen button and the telemetry, in one file with no
// external script, font or image (the server's Content-Security-Policy allows none). It talks to the remote-control API of the same server.
#pragma once
namespace onair_server {

static const char* const kIndexHtml = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OnAir server</title>
<style>
:root{--bg:#f4f5f7;--fg:#1c2128;--mut:#59626d;--card:#fff;--line:#d5d9df;--acc:#1f6feb;--bad:#c62828;--ok:#1a7f37}
@media (prefers-color-scheme:dark){:root{--bg:#0e1116;--fg:#e6e9ee;--mut:#98a2b0;--card:#171b22;--line:#2b323c;--acc:#58a6ff;--bad:#ff7b72;--ok:#56d364}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:center;padding:10px 16px;border-bottom:1px solid var(--line);background:var(--card)}
h1{font-size:17px;margin:0}#st{color:var(--mut);font-size:13px}main{padding:12px 16px;display:grid;gap:12px;max-width:1200px;margin:0 auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px}
.row{display:flex;flex-wrap:wrap;gap:8px 12px;align-items:end}label{display:flex;flex-direction:column;font-size:12px;color:var(--mut);gap:2px}
select,input,button{font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:6px 8px;min-height:36px}
button{cursor:pointer}button.p{background:var(--acc);border-color:var(--acc);color:#fff}button.on{outline:2px solid var(--acc)}
canvas{width:100%;display:block;border-radius:4px;background:#000}#sp{height:180px}#wf{height:200px;margin-top:6px}
table{border-collapse:collapse;width:100%;font-size:13px}td{padding:3px 6px;border-bottom:1px solid var(--line)}td:first-child{color:var(--mut);width:45%}
#err{color:var(--bad);min-height:1.2em;font-size:13px}.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--bad);margin-right:6px}.dot.ok{background:var(--ok)}
</style></head><body>
<header><h1>OnAir server</h1><span id="st"><span class="dot" id="dot"></span><span id="stt">connecting</span></span></header>
<main>
<div class="card"><div class="row">
<label>Radio<select id="dev"></select></label>
<label>Mode<select id="mode"></select></label>
<label>Frequency (MHz)<input id="freq" type="number" step="any" style="width:130px"></label>
<label>Gain (dB)<input id="gain" type="number" step="1" style="width:80px"></label>
<label>Sample rate (Msps, 0 = auto)<input id="rate" type="number" step="any" style="width:90px"></label>
<button id="go" class="p">Start</button>
</div><div id="err"></div></div>
<div class="card"><canvas id="sp" width="1024" height="180"></canvas><canvas id="wf" width="1024" height="200"></canvas>
<div class="row" style="margin-top:8px"><span id="info" style="color:var(--mut);font-size:13px"></span></div></div>
<div class="card"><div class="row">
<button id="ls">Listen</button>
<label>Demodulator<select id="dm"><option>auto</option><option>nfm</option><option>am</option><option>usb</option><option>lsb</option></select></label>
<label>Data use<select id="bw"><option value="auto">Automatic</option><option value="low">Low (slow link)</option><option value="high">High (fast link)</option></select></label>
<span id="aud" style="color:var(--mut);font-size:13px"></span></div></div>
<div class="card"><table id="tel"></table></div>
</main>
<script>
"use strict";
var tok=null;try{var q=new URLSearchParams(location.search);tok=q.get("token")||sessionStorage.getItem("tok");if(q.get("token")){sessionStorage.setItem("tok",tok);history.replaceState(null,"",location.pathname)}}catch(e){}
var $=function(i){return document.getElementById(i)};
function api(p,b){var o={headers:{}};if(tok)o.headers.Authorization="Bearer "+tok;if(b!==undefined){o.method="POST";o.headers["Content-Type"]="application/json";o.body=JSON.stringify(b)}
 return fetch("/api/v1"+p,o).then(function(r){return r.json().then(function(j){if(!r.ok)throw new Error(j.error||r.status);return j})})}
function err(e){$("err").textContent=e?String(e.message||e):""}
var modes=[],ws=null,spec=null,aud=null,listening=false,st={};
var prof={low:{bins:256,fps:2,rate:8000},high:{bins:1024,fps:10,rate:24000},auto:{bins:512,fps:5,rate:16000}};
function fillModes(){var s=$("mode");s.innerHTML="";modes.forEach(function(m,i){var o=document.createElement("option");o.value=i;o.textContent=m.name+(m.sub?"":"");s.appendChild(o)})}
function fillDevs(d){var s=$("dev");s.innerHTML="";d.forEach(function(x){var o=document.createElement("option");o.value=x.index;o.textContent=x.name;if(x.selected)o.selected=true;s.appendChild(o)})}
function showStatus(s){st=s;$("dot").className="dot"+(s.running?" ok":"");$("stt").textContent=s.running?"receiving - "+s.device:(s.wanted?"waiting for the radio":"stopped");
 $("go").textContent=s.running||s.wanted?"Stop":"Start";if(document.activeElement!==$("freq"))$("freq").value=(s.frequencyHz/1e6).toFixed(6).replace(/0+$/,"").replace(/\.$/,"");
 if(document.activeElement!==$("gain"))$("gain").value=s.gainDb;
 for(var i=0;i<modes.length;i++)if(modes[i].family===s.family&&(modes[i].sub||null)===(s.sub||null)){if(document.activeElement!==$("mode"))$("mode").value=i}
 if(s.error)err(s.error);$("dm").value=s.demod;
 $("info").textContent=s.running?(s.sampleRateHz/1e6).toFixed(3)+" Msps, load "+s.loadPct+"%, dropped "+s.droppedSamples:""}
function poll(){api("/status").then(showStatus).catch(function(){})}
function flat(o,p,out,d){for(var k in o){var v=o[k];var n=p?p+"."+k:k;if(v&&typeof v==="object"){if(d<2&&!Array.isArray(v))flat(v,n,out,d+1)}else if(out.length<60)out.push([n,v])}}
function showTel(t){var r=[];flat(t,"",r,0);var h="";r.forEach(function(x){var v=typeof x[1]==="number"?(Math.abs(x[1])<1e5?+x[1].toFixed(3):x[1]):x[1];h+="<tr><td>"+esc(x[0])+"</td><td>"+esc(String(v))+"</td></tr>"});$("tel").innerHTML=h}
function esc(s){return s.replace(/[&<>"]/g,function(c){return{"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]})}
var sp=$("sp"),spc=sp.getContext("2d"),wf=$("wf"),wfc=wf.getContext("2d");
function drawSpec(db,c,r){spec={c:c,r:r};var w=sp.width,h=sp.height,n=db.length;spc.fillStyle="#000";spc.fillRect(0,0,w,h);spc.strokeStyle="#58a6ff";spc.beginPath();
 for(var i=0;i<n;i++){var x=i*w/(n-1),y=h-(db[i]+120)/120*h;if(i)spc.lineTo(x,y);else spc.moveTo(x,y)}spc.stroke();
 spc.fillStyle="#98a2b0";spc.font="11px sans-serif";spc.fillText((c/1e6-r/2e6).toFixed(3),2,h-3);var t=(c/1e6+r/2e6).toFixed(3);spc.fillText(t,w-spc.measureText(t).width-2,h-3);
 wfc.drawImage(wf,0,0,wf.width,wf.height-1,0,1,wf.width,wf.height-1);var im=wfc.createImageData(wf.width,1);
 for(var x=0;x<wf.width;x++){var v=db[Math.min(n-1,Math.floor(x*n/wf.width))],u=Math.max(0,Math.min(1,(v+110)/90));var k=x*4;im.data[k]=Math.round(255*Math.max(0,(u-.5)*2));im.data[k+1]=Math.round(255*Math.max(0,Math.min(1,(u-.2)*2)));im.data[k+2]=Math.round(255*Math.min(1,u*2.5)*(1-Math.max(0,u-.7)*2));im.data[k+3]=255}
 wfc.putImageData(im,0,0)}
function clickTune(e){if(!spec)return;var b=e.target.getBoundingClientRect(),f=spec.c+((e.clientX-b.left)/b.width-.5)*spec.r;api("/frequency",{hz:Math.round(f)}).then(poll).catch(err)}
sp.onclick=wf.onclick=clickTune;
function sub(){if(ws&&ws.readyState===1){var p=prof[$("bw").value];ws.send(JSON.stringify({telemetry:true,spectrum:true,bins:p.bins,fps:p.fps,fmt:"u8"}));if(listening)ws.send(JSON.stringify({audio:true,arate:p.rate}))}}
function connect(){ws=new WebSocket((location.protocol==="https:"?"wss://":"ws://")+location.host+"/api/v1/ws"+(tok?"?token="+encodeURIComponent(tok):""));ws.binaryType="arraybuffer";
 ws.onopen=function(){sub();poll()};ws.onclose=function(){$("stt").textContent="disconnected - retrying";$("dot").className="dot";setTimeout(connect,2000)};
 ws.onmessage=function(m){if(typeof m.data==="string"){var j=JSON.parse(m.data);if(j.type==="telemetry")showTel(j.data);else if(j.type==="spectrum"&&j.data.dbfs&&j.data.dbfs.length)drawSpec(j.data.dbfs,j.data.centerHz,j.data.sampleRateHz);else if(j.type==="error")err(j.error);return}
  var d=new DataView(m.data);if(d.getUint8(0)===1){var c=d.getFloat32(5,true),r=d.getFloat32(9,true),lo=d.getFloat32(13,true),hi=d.getFloat32(17,true),a=[];for(var i=21;i<m.data.byteLength;i++)a.push(lo+d.getUint8(i)/255*(hi-lo));drawSpec(a,c,r)}
  else if(d.getUint8(0)===2)play(new Int16Array(m.data.slice(1,1+((m.data.byteLength-1)&~1))))}}
function play(s){if(!aud||!s.length)return;var rate=prof[$("bw").value].rate,b=aud.createBuffer(1,s.length,rate),f=b.getChannelData(0);for(var i=0;i<s.length;i++)f[i]=s[i]/32768;
 var t=Math.max(aud.currentTime+.08,nextT);if(t-aud.currentTime>.7){return}var src=aud.createBufferSource();src.buffer=b;src.connect(aud.destination);src.start(t);nextT=t+b.duration}
var nextT=0;
$("ls").onclick=function(){listening=!listening;$("ls").className=listening?"on":"";$("ls").textContent=listening?"Stop listening":"Listen";
 if(listening){aud=aud||new(window.AudioContext||window.webkitAudioContext)();aud.resume();nextT=0;$("aud").textContent="";sub()}else if(ws&&ws.readyState===1)ws.send(JSON.stringify({audio:false}))};
$("bw").onchange=sub;$("dm").onchange=function(){api("/x/demod",{demod:$("dm").value}).catch(err)};
$("go").onclick=function(){err();(st.running||st.wanted?api("/stop",{}):api("/start",{device:+$("dev").value})).then(poll).catch(err)};
$("dev").onchange=function(){if(!st.running&&!st.wanted)return;api("/stop",{}).then(function(){return api("/start",{device:+$("dev").value})}).then(poll).catch(err)};
$("mode").onchange=function(){var m=modes[+$("mode").value];err();api("/mode",m.sub?{family:m.family,sub:m.sub}:{family:m.family}).then(poll).catch(err)};
$("freq").onchange=function(){api("/frequency",{mhz:+$("freq").value}).then(poll).catch(err)};
$("gain").onchange=function(){api("/gain",{db:+$("gain").value}).then(poll).catch(err)};
$("rate").onchange=function(){api("/samplerate",{msps:+$("rate").value}).then(poll).catch(err)};
Promise.all([api("/modes"),api("/devices")]).then(function(r){modes=r[0];fillModes();fillDevs(r[1]);poll();connect();setInterval(poll,3000)}).catch(function(e){err(e);$("stt").textContent="cannot reach the server (is the token in the address?)"});
</script></body></html>
)HTML";

} // namespace onair_server
