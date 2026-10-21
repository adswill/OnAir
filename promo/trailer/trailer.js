// OnAir 0.2.9 trailer: a general overview of what OnAir is, cut to an original 120 BPM hard-rock score (music_tr.py; every kick, snare and crash time is exact by construction).
// Footage: the real app (clips7: every mode on its built-in test signal, 0.2.7 interface), the official 0.2.9 screenshots (docs/img), the KA signature pictures (gen_ka_signals.py: synthesised, labelled). 60 fps.
const RED='#ff5a46', FPS9=60, BT=window.BEATS, LEN9=BT.len, SEC9=Object.fromEntries(BT.sections);
const KICK=BT.kicks, SNARE=BT.snares, CRASH=BT.crashes, ALLK=KICK.concat(SNARE).sort((a,b)=>a-b);
const S={intro:0,verse1:SEC9.verse1,pre:SEC9.pre,ch1:SEC9.chorus1,verse2:SEC9.verse2,ch2:SEC9.chorus2,bridge:SEC9.bridge,build:SEC9.build,fin:SEC9.final,outro:SEC9.outro};
const logo9=new Image(); logo9.src='assets/logo_028.png';
const lastT=(arr,t)=>{ let lo=0,hi=arr.length-1,r=-1; while(lo<=hi){ const m=(lo+hi)>>1; if(arr[m]<=t){ r=m; lo=m+1; } else hi=m-1; } return r; };
const envT=(arr,t,k)=>{ const i=lastT(arr,t); return i<0?0:Math.exp(-(t-arr[i])*k); };
const KE=t=>envT(KICK,t,9), SE=t=>envT(SNARE,t,8), CE=t=>envT(CRASH,t,5), BE=t=>Math.max(KE(t),SE(t));
const hitIn=(arr,t,d)=>{ const i=lastT(arr,t); return i>=0&&t-arr[i]<d; };
// ---------------------------------------------------------------- helpers
const c7=(n,t)=>'file:///tmp/clips7/'+n+'/f_'+pad(clamp(Math.floor(t*30),0,(window.C7N[n]||240)-1)+1,5)+'.jpg';
const CROPD={cx:1112,cy:575,z:1.19}, crop7=n=>(window.CROPS7&&window.CROPS7[n])||CROPD, has7=n=>!!(window.C7N&&window.C7N[n]);
const loopT=(n,t,a=.6)=>{ const L=Math.max(.5,(window.C7N[n]||90)/30-a-.4); return a+(((t%L)+L)%L); };
function tex7(buf,n,t,cm){ return tex(buf,x=>foot(x,c7(n,t),cm||crop7(n))); }
const rgba9=(col,a)=>col.replace('rgb','rgba').replace(')',','+a+')');
function shake9(t,amt){ const k=BE(t)*amt; return [(hash(Math.floor(t*60)*1.3)-.5)*k,(hash(Math.floor(t*60)*2.1)-.5)*k]; }
function flash9(c,k,col='255,255,255'){ if(k<=.01) return; c.save(); c.globalCompositeOperation='lighter'; c.fillStyle=`rgba(${col},${Math.min(1,k)})`; c.fillRect(0,0,W,H); c.restore(); }
function kick9(c,s,x,y,a=1,col=RED,al='left',size=26){ if(a<=0) return; txt(c,s,x,y,{size,w:800,col,ls:7,a,al}); }
const eob9=k=>{ k=clamp(k); const c1=1.70158, c3=c1+1; return 1+c3*Math.pow(k-1,3)+c1*Math.pow(k-1,2); };
const MODES=GROUPS.flatMap((g,gi)=>g.members.map(m=>({...m,g:gi,col:g.col,clip:(m.new?'n_':'o_')+m.id}))); const clipOf=m=>has7(m.clip)?m.clip:null;
const GREP=['o_dvb','o_dab','o_adsb','o_ais','n_hrpt','o_dmr','o_cell','o_pocsag','o_wifi','o_sonde','o_ft8','o_m17','o_ale','o_specan'];
function glowBg(c,t,tint,a=1){ c.fillStyle='#04060a'; c.fillRect(0,0,W,H); const g=c.createRadialGradient(W*.5,H*.45,0,W*.5,H*.45,W*.7); g.addColorStop(0,rgba9(tint||'rgb(92,168,219)',.16*a+.1*KE(t))); g.addColorStop(1,'rgba(0,0,0,0)'); c.fillStyle=g; c.fillRect(0,0,W,H); }
function vig(c,a=.4){ const v=c.createRadialGradient(W/2,H/2,H*.5,W/2,H/2,H*1.05); v.addColorStop(0,'rgba(0,0,0,0)'); v.addColorStop(1,`rgba(0,0,0,${a})`); c.fillStyle=v; c.fillRect(0,0,W,H); }
function bug(c,t,a=1){ c.save(); c.globalAlpha*=a*.9; c.drawImage(logo9,W-232,34,64,64); txt(c,'OnAir',W-160,80,{size:30,w:700,col:'#e6eaee'}); c.restore(); }
// ---------------------------------------------------------------- 0. intro: everything around you is a signal
const WORDS=[['AIRCRAFT','o_adsb'],['SHIPS','o_ais'],['SATELLITES','o_meteor'],['TELEVISION','o_dvb'],['RADIO','o_fm'],['PAGERS','o_pocsag'],['SENSORS','o_ism']];
function sIntro(c,t){
  c.fillStyle='#030405'; c.fillRect(0,0,W,H); const fi=ease(t,0,1.4);
  const wi=t>=4?Math.min(6,Math.floor((t-4)/.5)):-1, clip=wi>=0?WORDS[wi][1]:'o_general', cam=crop7(clip);
  c.save(); c.globalAlpha=(t<4?.5:.8)*fi; foot(c,c7(clip,loopT(clip,t*.9)),{...cam,z:cam.z*(1+.02*t)}); c.restore(); c.fillStyle=`rgba(3,5,8,${t<4?.62:.5})`; c.fillRect(0,0,W,H); vig(c,.5);
  c.save(); c.globalAlpha*=fi*.8; for(let i=0;i<=48;i++){ const x=60+i*(W-120)/48; c.fillStyle='rgba(255,255,255,.26)'; c.fillRect(x,92,i%6===0?2:1,i%6===0?22:10); } const mx=lerp(160,W-160,eio3(seg(t,.3,S.verse1-.1))); c.fillStyle=RED; c.fillRect(mx-1,70,3,56); txt(c,'TUNING',60,60,{size:20,w:800,col:'#8d97a1',ls:6}); c.restore();
  slam(c,'Everything around you',W/2,440,t,1.0,{size:78,w:600,col:'#b9c2cc',t1:3.7,from:1.2}); slam(c,'is a signal.',W/2,620,t,2.5,{size:190,w:800,t1:3.8,from:1.4});
  if(wi>=0){ const [w]=WORDS[wi], lt=t-(4+wi*.5), k=eob9(seg(lt,0,.18)); c.save(); c.translate(W/2,H/2+60); c.scale(lerp(1.25,1,k),lerp(1.25,1,k)); c.globalAlpha*=clamp(k*3); txt(c,w,0,0,{size:200,w:800,al:'center',ls:-6,col:'#fff'}); c.restore(); flash9(c,.22*Math.exp(-lt*14)); }
  const roll=seg(t,7,8); if(roll>0) flash9(c,roll*roll*.3*(hash(Math.floor(t*30))>.5?1:.4));
  flash9(c,KE(t)*.08*seg(t,4,8)); }
// ---------------------------------------------------------------- 1. title, then four verbs
function sTitle(c,t){
  const lt=t-S.verse1; glowBg(c,t); c.save(); c.globalAlpha*=.35; foot(c,c7('o_dvb',loopT('o_dvb',t*.5)),{...crop7('o_dvb'),z:1.5}); c.restore(); c.fillStyle='rgba(4,6,10,.72)'; c.fillRect(0,0,W,H); vig(c,.5);
  const [sx,sy]=shake9(t,22); c.save(); c.translate(sx,sy);
  const k=eob9(seg(lt,-.05,.35)); if(k>0){ c.save(); c.translate(W/2,330); const s=lerp(1.7,1,k); c.scale(s,s); c.globalAlpha*=clamp(k*3); c.shadowColor='rgba(255,90,70,.7)'; c.shadowBlur=100*Math.exp(-lt*1.4)+22; c.drawImage(logo9,-170,-170,340,340); c.restore(); }
  slam(c,[['On',0],['Air',1]],W/2,690,t,S.verse1+.02,{size:250,from:1.7,glow:true}); slam(c,'Receive almost anything with one radio.',W/2,800,t,S.verse1+1.0,{size:54,w:600,col:'#c8d2dc',from:1.2});
  txt(c,'FREE AND OPEN SOURCE',W/2,880,{size:24,w:800,col:RED,ls:9,al:'center',a:ease(lt,1.8,.6)}); c.restore(); flash9(c,CE(t)*.9); bug(c,t,seg(lt,2,3)*0); }
const VERBS=[['WATCH','TELEVISION',['DVB-T/T2, DVB-S2, ATSC, ISDB-T and more','A live player: picture, sound, subtitles, guide','Record, pause, rewind'],'o_dvb','rgb(92,188,220)',{cx:1112,cy:575,z:1.19}],
 ['LISTEN','RADIO',['FM with RDS, DAB+, DRM, HD Radio, AM','Airband and amateur voice','Saved stations'],'o_dab','rgb(92,210,148)',null],
 ['TRACK','AIRCRAFT AND SHIPS',['ADS-B aircraft on a live map','AIS ships, APRS, mesh nodes','Offline map files'],'o_adsb','rgb(132,160,240)',null],
 ['DECODE','DIGITAL SIGNALS',['DMR, P25, TETRA and more two-way radio','Pagers, sensors, LoRa, Meshtastic','Weather pictures from satellites'],'o_dmr','rgb(226,168,112)',null]];
function shot(c,t,t0,t1,o){ const lt=t-t0, dur=t1-t0, p=seg(t,t0,t1), left=o.left, side=left?1:-1, inK=eio3(seg(lt,-.03,.3)), outK=eio3(seg(t,t1-.2,t1)); const [sx,sy]=shake9(t,10);
  glowBg(c,t,o.col,1); const gg=c.createRadialGradient(left?W*.72:W*.28,H*.45,0,left?W*.72:W*.28,H*.45,W*.55); gg.addColorStop(0,rgba9(o.col,.3)); gg.addColorStop(1,'rgba(0,0,0,0)'); c.fillStyle=gg; c.fillRect(0,0,W,H);
  c.save(); c.translate(sx,sy); const cm=o.cm||crop7(o.clip);
  if(o.img){ screen3(c,img(o.img),{pos:[side*(lerp(1500,560,inK)+outK*1200),0,lerp(900,100,inK)-40*p],w:1120,ry:-side*(lerp(.95,.3,inK)-.05*p),rx:.03,glow:.75,sheen:p,a:1-outK*.6,nx:20,ny:12}); }
  else screen3(c,tex7(TEXL[o.buf||0],o.clip,loopT(o.clip,lt*(o.rate||.8)+(o.t0||0)),{cx:lerp(cm.cx,o.zoom?o.zoom.cx:cm.cx,eio3(seg(lt,.5,dur-.4))),cy:lerp(cm.cy,o.zoom?o.zoom.cy:cm.cy,eio3(seg(lt,.5,dur-.4))),z:lerp(cm.z,o.zoom?o.zoom.z:cm.z,eio3(seg(lt,.5,dur-.4)))}),{pos:[side*(lerp(1500,560,inK)+outK*1200),0,lerp(900,100,inK)-40*p],w:1120,ry:-side*(lerp(.95,.3,inK)-.05*p),rx:.03,glow:.75,sheen:p,a:1-outK*.6,nx:20,ny:12});
  c.save(); const gs=c.createLinearGradient(left?0:W,0,left?W*.58:W*.42,0); gs.addColorStop(0,'rgba(3,5,8,.92)'); gs.addColorStop(1,'rgba(3,5,8,0)'); c.fillStyle=gs; c.fillRect(0,0,W,H); c.restore();
  const x=left?110:W-110, al=left?'left':'right'; kick9(c,o.kick,x,262,ease(lt,0,.3)*(1-outK),RED,al);
  slam(c,[[o.word,1]],x,410,t,t0+.04,{size:o.size||170,al,from:1.3,d:.14,t1:t1-.2}); 
  const yb=470; c.save(); c.globalAlpha*=(1-outK); o.lines.forEach((b,j)=>{ const k=seg(t,t0+.45+j*.25,t0+.65+j*.25); if(k<=0) return; c.save(); c.globalAlpha*=k; c.translate((1-k)*(left?-20:20),0); c.fillStyle=RED; c.beginPath(); c.arc(left?x+6:x-6,yb+j*58-11,7,0,7); c.fill(); txt(c,b,left?x+28:x-28,yb+j*58,{size:32,w:500,col:'#e1e8ef',al}); c.restore(); }); c.restore();
  c.restore(); flash9(c,Math.exp(-(t-t0)*12)*.22+SE(t)*.05); }
function sVerbs(c,t){ const t0=S.verse1+2; if(t<t0+8){ const i=Math.min(3,Math.floor((t-t0)/2)); const v=VERBS[i]; shot(c,t,t0+i*2,t0+i*2+2,{left:i%2===0,word:v[0]+'.',kick:v[1],lines:v[2],clip:v[3],col:v[4],cm:v[5],buf:i%2,rate:.7}); } else sOneApp(c,t); }
function sOneApp(c,t){ const t0=S.verse1+10, lt=t-t0; glowBg(c,t,'rgb(92,168,219)'); const cl=['o_dvb','o_fm','o_adsb','o_dmr'], [sx,sy]=shake9(t,16); c.save(); c.translate(sx,sy); const e=eio3(seg(lt,0,1.2));
  cl.forEach((n,i)=>{ const gx=(i%2?1:-1), gy=(i<2?-1:1); const pos=[gx*lerp(1400,420,e),gy*lerp(900,235,e),lerp(900,260,e)-60*KE(t)]; screen3(c,tex7(TEXS[i],n,loopT(n,t*.8)),{pos,w:760,h:428,ry:-gx*.22*e,rx:gy*.1*e,glow:.5,a:e,nx:10,ny:6}); });
  c.restore(); c.fillStyle='rgba(3,5,8,.35)'; c.fillRect(0,0,W,H); vig(c,.4); const [a,b]=[S.pre-4.0,S.pre-2.0];
  slam(c,'One app.',W/2,H/2+60,t,t0+.05,{size:230,t1:t0+1.8,from:1.6}); slam(c,'One radio.',W/2,H/2+60,t,t0+2.0,{size:230,t1:t0+3.8,from:1.6}); slam(c,[['Every ',0],['signal.',1]],W/2,H/2+60,t,t0+4.0,{size:230,t1:S.pre-.04,from:1.6});
  flash9(c,KE(t)*.1+CE(t)*.4); }
// ---------------------------------------------------------------- 2. fourteen groups, one cut a second
function sGroups(c,t){ const lt=t-S.pre; let i=Math.min(13,Math.floor(lt)), tt=lt-i; if(lt>=14){ glowBg(c,t); const k=lt-14; c.fillStyle='#04060a'; c.fillRect(0,0,W,H); slam(c,'Fourteen groups.',W/2,H/2+40,t,S.pre+14.05,{size:200,from:1.5,t1:S.ch1-.3}); const f=hash(Math.floor(t*30)); flash9(c,seg(lt,15,16)*.35*f); bug(c,t); return; }
  const G=GROUPS[i], clip=GREP[i], cam=crop7(clip), [sx,sy]=shake9(t,14); c.fillStyle='#000'; c.fillRect(0,0,W,H); c.save(); c.translate(sx,sy); foot(c,c7(clip,loopT(clip,t*.9+i*.3)),{...cam,z:cam.z*(1.04+.08*tt)}); c.fillStyle='rgba(3,5,8,.38)'; c.fillRect(0,0,W,H); scrim(c,1);
  const gc=G.col; slam(c,G.name,110,H-250,t,S.pre+i,{size:G.name.length>16?108:140,al:'left',from:1.25,d:.09,col:'#fff'}); txt(c,String(i+1).padStart(2,'0')+' / 14',112,H-370,{size:30,w:800,col:gc,ls:6,mono:true}); txt(c,G.members.length+' modes',112,H-140,{size:34,w:700,col:'#c8d2dc'});
  const names=G.members.slice(0,6).map(m=>m.n); names.forEach((n,j)=>{ const k=eob9(seg(tt,.1+j*.05,.3+j*.05)); c.save(); c.globalAlpha*=clamp(k); txt(c,n,W-110,H-420+j*52,{size:34,w:600,al:'right',col:j===0?'#fff':'#c8d2dc'}); c.restore(); });
  c.restore(); for(let j=0;j<14;j++){ c.fillStyle='rgba(255,255,255,.14)'; c.fillRect(110+j*128,H-60,118,6); c.fillStyle=GROUPS[j].col; c.fillRect(110+j*128,H-60,118*(j<i?1:j===i?tt:0),6); }
  flash9(c,Math.exp(-tt*14)*.3+SE(t)*.05); bug(c,t); }
// ---------------------------------------------------------------- 3. the count and the wall
function sCount(c,t){ const lt=t-S.ch1; glowBg(c,t,'rgb(150,200,255)',1.4); c.save(); c.globalAlpha*=.3; foot(c,c7('o_specan',loopT('o_specan',t*.6)),{...crop7('o_specan'),z:1.3}); c.restore(); c.fillStyle='rgba(3,5,8,.55)'; c.fillRect(0,0,W,H); vig(c,.45);
  const [sx,sy]=shake9(t,22); c.save(); c.translate(sx,sy); const p=eio3(seg(lt,0,1.6)), n=Math.round(142*p), big=n>=142; c.save(); c.translate(W/2,590); const s_=big?1+.15*KE(t):1; c.scale(s_,s_); if(big){ c.shadowColor='rgba(255,90,70,.85)'; c.shadowBlur=100; } txt(c,String(n),0,0,{size:620,w:900,al:'center',col:big?'#fff':'#d4dbe2',ls:-26}); c.restore();
  slam(c,'modes',W/2,730,t,S.ch1+.1,{size:90,w:700,col:big?RED:'#9aa6b2'}); kick9(c,'FROM ONE SOFTWARE-DEFINED RADIO',W/2,810,ease(lt,1.6,.4),RED,'center',24);
  const chips=['TV','RADIO','AIRCRAFT','SHIPS','SATELLITES','TWO-WAY','PAGERS','IoT','AMATEUR','WEATHER']; c.font='700 26px Inter'; const ws=chips.map(s=>c.measureText(s).width+44), tot=ws.reduce((a,b)=>a+b+14,-14); let x=W/2-tot/2;
  chips.forEach((s,j)=>{ const t0=S.ch1+1.6+j*.2, k=eob9(seg(t,t0,t0+.2)); if(k>0){ c.save(); c.globalAlpha*=clamp(k*2); c.translate(x+ws[j]/2,890); c.scale(k,k); rr(c,-ws[j]/2,-26,ws[j],52,26); c.fillStyle=rgba9(GROUPS[Math.min(13,j)].col,.2); c.fill(); c.strokeStyle=GROUPS[Math.min(13,j)].col; c.lineWidth=2.5; c.stroke(); txt(c,s,0,9,{size:26,w:800,al:'center'}); c.restore(); } x+=ws[j]+14; });
  c.restore(); flash9(c,SE(t)*.12+(lt<.1?.7:0)+CE(t)*.3); bug(c,t); }
function sWall(c,t){ const t0=S.ch1+4, t1=S.verse2, lt=t-t0, span=t1-t0, k=eio3(seg(lt,0,span)), [sx,sy]=shake9(t,20);
  c.fillStyle='#04060a'; c.fillRect(0,0,W,H); const gl=c.createRadialGradient(W/2,H*.45,0,W/2,H*.45,W*.7); gl.addColorStop(0,'rgba(90,140,200,.3)'); gl.addColorStop(1,'rgba(0,0,0,0)'); c.fillStyle=gl; c.fillRect(0,0,W,H); c.save(); c.translate(sx,sy);
  const cols=12, tw=300, th=169, gx=22, gy=22, rows=10, ry=lerp(-.5,.1,k), rx=lerp(.45,.08,k), off=[lerp(-700,0,k),lerp(300,-10,k),lerp(2800,1500,eio3(seg(lt,0,1.4)))+lerp(0,-300,k)];
  const flick=ALLK.filter(e=>e<=t&&t-e<.18).map(e=>Math.floor(hash(e*91)*MODES.length));
  const items=MODES.map((m,i)=>{ const gxp=((i%cols)-(cols-1)/2)*(tw+gx), gyp=(Math.floor(i/cols)-(rows-1)/2)*(th+gy), q=rot3([gxp,gyp,0],rx,ry); return {m,i,gxp,gyp,pos:[q[0]+off[0],q[1]+off[1],q[2]+off[2]]}; }).sort((a,b)=>b.pos[2]-a.pos[2]);
  items.forEach(it=>{ const fly=clamp(lt*3-hash(it.i*1.7)*1.4); if(fly<=0) return; const cl=clipOf(it.m), P=(u,v)=>{ const q=rot3([it.gxp+(u-.5)*tw,it.gyp+(v-.5)*th,0],rx,ry); return proj([q[0]+off[0],q[1]+off[1],q[2]+off[2]]); }; const cs=[P(0,0),P(1,0),P(1,1),P(0,1)]; if(cs.some(p=>p[2]<=0)) return;
    c.save(); c.globalAlpha*=clamp(fly); if(cl) screen3(c,img('file:///tmp/clips7/'+cl+'/thumb.jpg'),{pos:it.pos,w:tw,h:th,rx,ry,nx:2,ny:2,glow:it.m.new?.5:0}); else { c.beginPath(); cs.forEach((p,j)=>j?c.lineTo(p[0],p[1]):c.moveTo(p[0],p[1])); c.closePath(); c.fillStyle='#0d1219'; c.fill(); }
    c.beginPath(); cs.forEach((p,j)=>j?c.lineTo(p[0],p[1]):c.moveTo(p[0],p[1])); c.closePath(); if(flick.includes(it.i)){ c.fillStyle='rgba(255,255,255,.55)'; c.fill(); } c.strokeStyle=rgba9(it.m.col,.8); c.lineWidth=1.6; c.stroke(); const s=cs[0][2]; txt(c,it.m.n,cs[3][0]+8*s,cs[3][1]-10*s,{size:Math.max(8,22*s),w:800,col:'#fff'}); c.restore(); });
  c.save(); c.fillStyle='rgba(3,5,8,.5)'; c.fillRect(0,H/2-150,W,250); c.restore(); slam(c,[['ONE ',0],['APP.',1]],W/2,H/2+50,t,t0+4,{size:240,from:2.3,glow:true,t1:t1-.2}); c.restore(); flash9(c,KE(t)*.06+CE(t)*.3); bug(c,t); }
// ---------------------------------------------------------------- 4. what it is like to use: four tools
const TOOLS=[{word:'See inside.',kick:'EVERY STAGE, LIVE',lines:['Spectrum, waterfall and constellations','Sync, error correction, signal quality','Zoom into the part that matters'],clip:'f_dvb',col:'rgb(92,188,220)',zoom:{cx:700,cy:520,z:1.6},left:true},
 {word:'Map it.',kick:'ONE MAP OF EVERYTHING',lines:['Aircraft, ships, APRS, mesh nodes','Satellites, passes and GNSS','Offline map files'],clip:'f_map',col:'rgb(132,160,240)',zoom:{cx:614,cy:490,z:1.4},left:false},
 {word:'Talk to the world.',kick:'AMATEUR RADIO',lines:['FT8, FT4, WSPR, JS8Call, PSK, RTTY, SSTV','FreeDV, APRS, MeshCom and more','30 amateur modes'],clip:'o_ft8',col:'rgb(218,130,214)',zoom:null,left:true,size:100},
 {word:'Your own modes.',kick:'THE KA FAMILY',lines:['Six digital modes designed from scratch','Own codecs, a callsign in every transmission','Tested in simulation, not yet on the air'],img:'file:///tmp/ka/assets/kaws_wide.png',col:'rgb(110,214,196)',zoom:null,left:false,size:96}];
function sTools(c,t){ const i=Math.min(3,Math.floor((t-S.verse2)/4)), t0=S.verse2+i*4, o=TOOLS[i]; shot(c,t,t0,t0+4,{...o,buf:i%2,rate:.7,t0:o.clip==='f_dvb'?2:0}); if(i===3){ const k=ease(t-t0,.8,.5); c.save(); c.globalAlpha*=k*(1-eio3(seg(t,t0+3.8,t0+4))); txt(c,'Signature pictures are synthesised from the published parameters.',W-110,1010,{size:20,w:400,col:'#8d97a1',al:'right'}); c.restore(); } bug(c,t); }
// ---------------------------------------------------------------- 5. any radio, any screen
const RADIOS=['HackRF','RTL-SDR','Airspy','SDRplay','BladeRF','LimeSDR','USRP','PlutoSDR','Fobos SDR','RX888 MkII','RFNM','Hermes Lite 2','Red Pitaya','KiwiSDR','SpyServer','SDR++ server','TCI','Sound card','Hamlib rigs','SoapySDR'];
function sRadios(c,t){ const lt=t-S.ch2; glowBg(c,t,'rgb(92,168,219)',1.2); c.save(); c.globalAlpha*=.22; foot(c,c7('o_specx',loopT('o_specx',t*.5)),{...crop7('o_specx'),z:1.2}); c.restore(); c.fillStyle='rgba(3,5,8,.6)'; c.fillRect(0,0,W,H); vig(c,.4); const [sx,sy]=shake9(t,12); c.save(); c.translate(sx,sy);
  kick9(c,'NO LOCK-IN',110,262,ease(lt,0,.3)*(1-seg(lt,7.6,8)),RED); slam(c,[['Any ',0],['radio.',1]],110,400,t,S.ch2+.04,{size:170,al:'left',from:1.4,t1:S.ch2+7.8});
  RADIOS.forEach((r,j)=>{ const t0=S.ch2+.5+j*.35, k=eob9(seg(t,t0,t0+.25)); if(k<=0) return; const col=j%5, row=Math.floor(j/5), x=110+col*345, y=480+row*125, out=seg(lt,7.7,8); c.save(); c.globalAlpha*=clamp(k*2)*(1-out); c.translate(x+95,y+40); c.scale(k,k); rr(c,-95,-40,190,80,14); c.fillStyle='#0d141b'; c.fill(); c.strokeStyle=r==='HackRF'?RED:'rgba(255,255,255,.2)'; c.lineWidth=2; c.stroke(); c.font='700 25px Inter'; const wdt=c.measureText(r).width, sc=Math.min(1,168/wdt); c.scale(sc,sc); txt(c,r,0,9,{size:25,w:700,al:'center'}); c.restore(); });
  c.save(); c.globalAlpha*=ease(lt,5.4,.5)*(1-seg(lt,7.6,8)); txt(c,'USB, network and sound-card radios and Hamlib rigs. No radio yet? A built-in demo signal lets you explore first.',110,1010,{size:28,w:500,col:'#b7c0ca'}); c.restore();
  const l2=t-(S.ch2+8); if(l2>0){ const pl=[['macOS','Apple silicon and Intel'],['Windows','10 and 11'],['Linux','x86-64 and arm64'],['Android','receive']]; kick9(c,'ON YOUR SCREEN, YOUR WAY',110,262,ease(l2,0,.3),RED); slam(c,[['Any ',0],['screen.',1]],110,400,t,S.ch2+8.04,{size:150,al:'left',from:1.4,t1:S.ch2+15.8});
    const e=eio3(seg(l2,0,.8)); screen3(c,tex7(TEXL[0],'f_dvb',2+l2*.5),{pos:[lerp(1500,420,e),-170,lerp(900,160,e)],w:880,ry:-.28*e,rx:.03,glow:.6,a:e,nx:16,ny:9}); screen3(c,img('file:///tmp/tr/assets/touch.jpg'),{pos:[lerp(2100,660,e),250,lerp(900,60,e)],w:560,ry:-.45*e,rx:-.03,glow:.5,a:e*seg(l2,.5,1.2),nx:12,ny:8}); screen3(c,tex7(TEXS[3],'f_light',2+l2*.4),{pos:[lerp(1800,190,e),270,lerp(900,-40,e)],w:480,ry:-.5*e,glow:.5,a:e*seg(l2,.9,1.6),nx:10,ny:6});
    pl.forEach(([a,b],j)=>{ const t0=S.ch2+8.6+j*.5, k=eob9(seg(t,t0,t0+.25)); if(k<=0) return; c.save(); c.globalAlpha*=clamp(k*2)*(1-seg(lt,15.7,16)); c.translate(110+j*215,780); c.scale(k,k); rr(c,0,0,200,120,14); c.fillStyle='#0d141b'; c.fill(); c.strokeStyle='rgba(255,255,255,.2)'; c.lineWidth=2; c.stroke(); txt(c,a,18,52,{size:34,w:800}); txt(c,b,18,92,{size:16,w:500,col:'#aeb8c2'}); c.restore(); }); }
  c.restore(); flash9(c,KE(t)*.06+CE(t)*.4); bug(c,t); }
// ---------------------------------------------------------------- 6. bridge: free, open, honest (half-time, heavy)
function sFree(c,t){ const lt=t-S.bridge; glowBg(c,t,'rgb(120,150,230)',1); c.save(); c.globalAlpha*=.28; foot(c,c7('f_dvb',3+lt*.15),{...crop7('f_dvb'),z:1.25+.01*lt}); c.restore(); c.fillStyle='rgba(3,5,8,.72)'; c.fillRect(0,0,W,H); vig(c,.5); const [sx,sy]=shake9(t,26); c.save(); c.translate(sx,sy);
  const card=Math.min(3,Math.floor(lt/4)), l=lt-card*4, out=seg(l,3.65,4)*(card<3?1:0), a=1-out;
  c.save(); c.globalAlpha*=a;
  if(card===0){ slam(c,'Free.',W/2,560,t,S.bridge+.02,{size:420,from:1.5,glow:true}); txt(c,'GPL-3.0-or-later, source on GitHub',W/2,690,{size:44,w:600,col:'#c8d2dc',al:'center',a:ease(l,1,.6)}); }
  else if(card===1){ kick9(c,'BUILT IN THE OPEN',W/2,300,ease(l,0,.3),RED,'center'); const st=[['1,376','commits'],['24','authors'],['82','people credited']]; st.forEach(([n,s],j)=>{ const t0=S.bridge+4+.5+j*1.0, k=eob9(seg(t,t0,t0+.3)); if(k<=0) return; c.save(); c.globalAlpha*=clamp(k*2); c.translate(W/2+(j-1)*560,620); c.scale(k,k); txt(c,n,0,0,{size:170,w:800,al:'center',col:'#fff',ls:-6}); txt(c,s,0,70,{size:40,w:500,al:'center',col:'#aeb8c2'}); c.restore(); }); txt(c,'Fixes, bug reports, recordings, translations and ideas from the community.',W/2,860,{size:32,w:400,col:'#8d97a1',al:'center',a:ease(l,2.4,.6)}); }
  else if(card===2){ slam(c,'Honest.',W/2,470,t,S.bridge+8.02,{size:300,from:1.5}); ['It shows what happens inside: sync, errors, decoded data.','Newer modes say when they are tested on generated signals only.','A receiver first. Transmit is off by default, behind a licence notice.'].forEach((s,j)=>txt(c,s,W/2,610+j*64,{size:38,w:500,col:'#c8d2dc',al:'center',a:ease(l,.9+j*.55,.5)})); }
  else { slam(c,'New here?',W/2,440,t,S.bridge+12.02,{size:230,from:1.5}); txt(c,'Press Tour: a little TV walks you through OnAir',W/2,590,{size:46,w:600,col:'#e6eaee',al:'center',a:ease(l,.9,.5)}); txt(c,'with a built-in demo signal. No radio needed.',W/2,650,{size:46,w:600,col:'#aeb8c2',al:'center',a:ease(l,1.5,.5)}); }
  c.restore(); c.restore(); flash9(c,SE(t)*.1+CE(t)*.4); bug(c,t); }
// ---------------------------------------------------------------- 7. build: the cuts double up, a counter runs to the last mode
const CUTS=(()=>{ const T=[]; let t=S.build; while(t<S.fin-.05){ const lt=t-S.build; const d=lt<4?.5:lt<8?.25:lt<12?.125:.0667; T.push(t); t+=d; } return T; })(), MON=MODES.filter(m=>clipOf(m));
function sBuild(c,t){ const ki=Math.max(0,lastT(CUTS,t)), tk=CUTS[ki]||S.build, m=MON[(ki*7)%MON.length], cl=clipOf(m), lt=t-tk, [sx,sy]=shake9(t,18+ki*.2);
  c.fillStyle='#000'; c.fillRect(0,0,W,H); c.save(); c.translate(sx,sy); if(cl){ const cm=crop7(cl); foot(c,c7(cl,loopT(cl,t*.8+ki*.37)),{...cm,z:cm.z*(1.03+.1*seg(lt,0,.3))}); } c.fillStyle='rgba(3,5,8,.3)'; c.fillRect(0,0,W,H); scrim(c,1);
  const spd=t-S.build<8?1:0; const sz=m.n.length>14?110:150; txt(c,m.n,110,H-170,{size:sz,w:800,ls:-4}); txt(c,GROUPS[m.g].name,112,H-330,{size:28,w:800,col:m.col,ls:5});
  const cnt=Math.round(142*(ki+1)/CUTS.length); txt(c,String(Math.min(142,cnt)).padStart(3,'0')+' / 142',W-110,H-170,{size:92,w:900,mono:true,al:'right'}); c.restore(); flash9(c,Math.exp(-lt*16)*.3+CE(t)*.4+(t>S.fin-.5?seg(t,S.fin-.5,S.fin)*.7:0)); bug(c,t); }
// ---------------------------------------------------------------- 8. final chorus
const FW=['TELEVISION','RADIO','AIRCRAFT','SHIPS','SATELLITES','TWO-WAY RADIO','PAGERS','HAM RADIO','SENSORS'];
function sFinal(c,t){ const lt=t-S.fin, [sx,sy]=shake9(t,22); c.fillStyle='#03050a'; c.fillRect(0,0,W,H);
  if(lt<8){ const e=eio3(seg(lt,0,.5)); c.save(); c.translate(sx,sy); const ids=['o_dvb','o_fm','o_adsb','o_ais','o_meteor','o_dmr','o_pocsag','o_ft8','o_ism']; ids.forEach((n,i)=>{ const col=i%3-1, row=Math.floor(i/3)-1, sw=Math.floor((t-S.fin)*2); const nn=(sw+i)%ids.length; const id=ids[(nn)%ids.length]; screen3(c,tex7(TEXS[i],id,loopT(id,t*.7+i)),{pos:[col*660*e,row*372*e,lerp(1200,300,e)-70*KE(t)],w:620,h:349,rx:row*-.05,ry:col*.22,glow:.4,a:e,nx:8,ny:5}); });
    c.restore(); c.fillStyle='rgba(3,5,8,.38)'; c.fillRect(0,0,W,H); slam(c,'Everything.',W/2,H/2+70,t,S.fin+.05,{size:300,from:1.7,t1:S.fin+3.8,glow:true}); slam(c,[['In ',0],['one app.',1]],W/2,H/2+70,t,S.fin+4.0,{size:300,from:1.7,t1:S.fin+7.9}); }
  else if(lt<16){ const l=lt-8; glowBg(c,t,'rgb(92,168,219)',1.6); const e=eio3(seg(l,0,1)); screen3(c,tex7(TEXL[1],'f_dvb',2+l*.8),{pos:[0,60,lerp(900,40,e)+40*Math.sin(l*.8)],w:1180,ry:.12*Math.sin(l*.5),rx:.03,glow:.9,a:e,nx:20,ny:12});
    const wi=Math.min(FW.length-1,Math.floor(l/.88)); FW.forEach((w,j)=>{ if(j>wi) return; const t0=S.fin+8+j*.88, k=eob9(seg(t,t0,t0+.25)); const ang=j/FW.length*Math.PI*2-Math.PI/2+.35, rx_=800, ry_=360, x=W/2+Math.cos(ang)*rx_, y=H/2+70+Math.sin(ang)*ry_; c.save(); c.globalAlpha*=clamp(k*2)*(j===wi?1:.65); c.translate(x,y); c.scale(k,k); rr(c,-130,-30,260,60,30); c.fillStyle='rgba(8,12,18,.82)'; c.fill(); c.strokeStyle=j===wi?RED:'rgba(255,255,255,.25)'; c.lineWidth=2.5; c.stroke(); txt(c,w,0,9,{size:26,w:800,al:'center'}); c.restore(); });
    slam(c,'Watch. Listen. Decode.',W/2,120,t,S.fin+8.05,{size:84,t1:S.fin+15.9,from:1.4}); }
  else { const l=lt-16; glowBg(c,t,'rgb(120,150,230)',1.4); c.save(); c.globalAlpha*=.25; foot(c,c7('o_dvb',loopT('o_dvb',t*.5)),{...crop7('o_dvb'),z:1.4}); c.restore(); c.fillStyle='rgba(3,5,8,.6)'; c.fillRect(0,0,W,H); vig(c,.5);
    c.save(); c.translate(sx,sy); slam(c,'Free.',W/2,420,t,S.fin+16.02,{size:300,from:1.6,t1:S.fin+19.9}); slam(c,[['Open ',0],['source.',1]],W/2,420,t,S.fin+20.0,{size:260,from:1.6,t1:S.outro-.1}); txt(c,'For everyone with an antenna.',W/2,560,{size:56,w:500,col:'#c8d2dc',al:'center',a:ease(l,1.4,.6)*(1-seg(t,S.outro-.2,S.outro))});
    ['macOS','Windows','Linux','Android'].forEach((p,j)=>{ const t0=S.fin+17.5+j*.5, k=eob9(seg(t,t0,t0+.25)); if(k<=0) return; c.save(); c.globalAlpha*=clamp(k*2)*(1-seg(t,S.outro-.2,S.outro)); c.translate(W/2+(j-1.5)*300,700); c.scale(k,k); rr(c,-125,-38,250,76,38); c.fillStyle='rgba(8,12,18,.85)'; c.fill(); c.strokeStyle='rgba(255,255,255,.3)'; c.lineWidth=2; c.stroke(); txt(c,p,0,10,{size:32,w:700,al:'center'}); c.restore(); }); c.restore(); }
  flash9(c,KE(t)*.07+CE(t)*.5); bug(c,t); }
function sOutro(c,t){ const lt=t-S.outro; c.fillStyle='#04060a'; c.fillRect(0,0,W,H); glowBg(c,t,'rgb(255,120,100)',1.4+CE(t)*2); vig(c,.5); const [sx,sy]=shake9(t,26); c.save(); c.translate(sx,sy); const s=lerp(1,1.04,seg(t,S.outro,LEN9));
  c.save(); c.translate(W/2,H/2); c.scale(s,s); c.translate(-W/2,-H/2); c.shadowColor='rgba(255,90,70,.7)'; c.shadowBlur=100*Math.exp(-lt*1.2)+24; c.drawImage(logo9,W/2-200,100,400,400); c.restore();
  slam(c,[['OnAir ',0],['0.2.9',1]],W/2,670,t,S.outro,{size:170,from:1.8}); slam(c,'on-air.dev',W/2,770,t,S.outro+.4,{size:70,w:700,col:'#8cc8ff',from:1.3}); txt(c,'github.com/adswill/OnAir',W/2,840,{size:42,w:600,col:'#aeb8c2',al:'center',a:ease(lt,.9,.5)});
  txt(c,'Many of the newer modes are experimental and tested on generated signals. Reports and recordings welcome.',W/2,925,{size:24,w:400,col:'#8d97a1',al:'center',a:ease(lt,1.4,.6)}); c.restore(); flash9(c,CE(t)*.9); ring9(c,W/2,300,t,S.outro); }
function ring9(c,x,y,t,t0,R=1500){ const k=seg(t,t0,t0+.8); if(k<=0||k>=1) return; c.save(); c.globalCompositeOperation='lighter'; c.strokeStyle=`rgba(255,180,160,${.6*(1-k)})`; c.lineWidth=2+10*(1-k); c.beginPath(); c.arc(x,y,eio3(k)*R,0,7); c.stroke(); c.restore(); }
// ---------------------------------------------------------------- timeline, transitions, post
const SEQ=[[0,S.verse1,sIntro],[S.verse1,S.verse1+2,sTitle],[S.verse1+2,S.pre,sVerbs],[S.pre,S.ch1,sGroups],[S.ch1,S.ch1+4,sCount],[S.ch1+4,S.verse2,sWall],[S.verse2,S.ch2,sTools],[S.ch2,S.bridge,sRadios],[S.bridge,S.build,sFree],[S.build,S.fin,sBuild],[S.fin,S.outro,sFinal],[S.outro,LEN9+1,sOutro]];
const TA=mk(), tax=TA.getContext('2d'), TRD=.2, HARD=new Set([S.verse1,S.pre,S.ch1,S.verse2,S.ch2,S.bridge,S.build,S.fin,S.outro]);
function frame9(t){ let k=SEQ.findIndex(s=>t<s[1]); if(k<0) k=SEQ.length-1; const [a,b,fn]=SEQ[k];
  // a punch-through into the big sections: the old scene scales up and fades under the first 0.2 s of the new one
  if(HARD.has(a)&&t-a<TRD&&k>0){ const e=eio3((t-a)/TRD), pv=SEQ[k-1]; tax.setTransform(1,0,0,1,0,0); tax.clearRect(0,0,W,H); pv[2](tax,a-.001); fn(ctx,t); ctx.save(); ctx.translate(W/2,H/2); const sa=lerp(1,1.4,e); ctx.scale(sa,sa); ctx.translate(-W/2,-H/2); ctx.globalAlpha=1-e; ctx.filter=`blur(${(e*12).toFixed(1)}px)`; ctx.drawImage(TA,0,0); ctx.restore(); return; }
  fn(ctx,t); }
const PB=mk(), pbx=PB.getContext('2d'), grain9=mk(256,256); { const g=grain9.getContext('2d'), id=g.createImageData(256,256); for(let i=0;i<65536;i++){ const v=(hash(i*1.37)*255)|0; id.data[i*4]=id.data[i*4+1]=id.data[i*4+2]=v; id.data[i*4+3]=255; } g.putImageData(id,0,0); }
function post9(t,i){ const big=CE(t), sn=SE(t), kk=KE(t), k=Math.max(big,sn*.4); if(k>.15||kk>.12){ const z=1+.012*kk+.03*big; pbx.clearRect(0,0,W,H); pbx.drawImage(cv,0,0); ctx.save(); ctx.fillStyle='#000'; ctx.fillRect(0,0,W,H); ctx.translate(W/2,H/2); ctx.scale(z,z); ctx.translate(-W/2,-H/2);
    if(k>.15){ ctx.globalCompositeOperation='lighter'; const o=.5+7*k; ctx.filter='url(#fr)'; ctx.drawImage(PB,-o,0); ctx.filter='url(#fg)'; ctx.drawImage(PB,0,0); ctx.filter='url(#fb)'; ctx.drawImage(PB,o,0); } else ctx.drawImage(PB,0,0); ctx.restore(); }
  ctx.save(); ctx.globalAlpha=.025; ctx.globalCompositeOperation='overlay'; const ox=(hash(i*1.7)*256)|0, oy=(hash(i*2.3)*256)|0; for(let x=-ox;x<W;x+=256) for(let y=-oy;y<H;y+=256) ctx.drawImage(grain9,x,y); ctx.restore();
  const o=seg(t,LEN9-1.6,LEN9); if(o>0){ ctx.fillStyle=`rgba(0,0,0,${o})`; ctx.fillRect(0,0,W,H); } const f=1-seg(t,0,.5); if(f>0){ ctx.fillStyle=`rgba(0,0,0,${f})`; ctx.fillRect(0,0,W,H); } }
window.NF=Math.round(LEN9*FPS9); window.ready=Promise.all([window.ready,logo9.decode()]);
window.renderFrame=async(i)=>{ const t=i/FPS9; NEED=new Set(); frame9(t); const need=NEED; NEED=null; if(need.size){ await ensure(need); frame9(t); } post9(t,i); return t; };
