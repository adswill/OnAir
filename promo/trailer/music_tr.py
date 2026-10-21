#!/usr/bin/env python3
"""Original hard-rock score for the OnAir 0.2.9 trailer: 120 BPM, E minor, 74 bars (148 s), written for this film (no sample, no existing riff).
MIDI stems (distortion + overdrive guitars, bass, drums, organ, strings, lead guitar) rendered with FluidSynth (FluidR3 GM), then amped (saturation, cabinet EQ),
doubled, reverbed and glued. Writes /tmp/tr/trailer_music.wav and trailer_beats.js (every kick / snare / crash time and the section starts, exact by construction)."""
import numpy as np, subprocess, os, random, json
from mido import MidiFile, MidiTrack, Message, MetaMessage, bpm2tempo
from scipy.io import wavfile
from scipy import signal
SR=44100; OUT='/tmp/tr/work'; os.makedirs(OUT,exist_ok=True); SF2='/usr/share/sounds/sf2/FluidR3_GM.sf2'
BPM=120; BEAT=.5; BAR=2.0; NB=74; DUR=NB*BAR; rnd=random.Random(3)
# sections (bar index): name, start bar, energy 0..1
SECS=[('intro',0,.25),('verse1',4,.55),('pre',12,.7),('chorus1',20,.9),('verse2',28,.7),('chorus2',36,.95),('bridge',44,.5),('build',52,.8),('final',60,1.0),('outro',72,.3)]
def sec(b):
    cur=SECS[0]
    for s in SECS:
        if b>=s[1]: cur=s
    return cur
class Stem:
    def __init__(s,prog,ch=0,pan=0): s.ev=[]; s.prog=prog; s.ch=ch
    def add(s,t,d,n,v): s.ev.append((max(0,t+rnd.uniform(-.003,.003)),d,n,int(max(1,min(127,v+rnd.randint(-3,3))))))
    def save(s,p):
        m=MidiFile(ticks_per_beat=480); tr=MidiTrack(); m.tracks.append(tr); tr.append(MetaMessage('set_tempo',tempo=bpm2tempo(BPM)))
        if s.ch!=9: tr.append(Message('program_change',program=s.prog,channel=s.ch,time=0))
        ms=[]
        for t,d,n,v in s.ev: ms.append((int(round(t/BEAT*480)),1,Message('note_on',note=n,velocity=v,channel=s.ch))); ms.append((int(round((t+d)/BEAT*480)),0,Message('note_off',note=n,velocity=0,channel=s.ch)))
        ms.sort(key=lambda x:(x[0],x[1])); last=0
        for tk,_,mm in ms: mm.time=tk-last; last=tk; tr.append(mm)
        m.save(p)
gtrL=Stem(30,0); gtrR=Stem(30,1); gtrO=Stem(29,2); bass=Stem(34,3); drums=Stem(0,9); organ=Stem(18,4); strings=Stem(48,5); lead=Stem(29,6)
KICK,SNARE,HAT,OHAT,CRASH,TOM1,TOM2,TOM3,RIDE=36,38,42,46,49,50,47,45,51
kicks=[]; snares=[]; crashes=[]
# chord roots (MIDI) per bar, 4-bar loops: Em - C - D - Bm/ B5 variations
RIFF=[40,40,43,45]            # E2 E2 G2 A2 as palm-muted 8th roots
PROG=[[40,36,38,35],[40,36,38,43],[36,38,40,35],[40,43,45,47]]
def chord_root(b):
    s=sec(b); p=PROG[(b//4)%4 if s[0] not in ('verse1','verse2') else 0]; return p[b%4]
def pw(root,oct=0): return [root+12+oct,root+19+oct]    # power chord: root + fifth
for b in range(NB):
    t0=b*BAR; name,sb,E=sec(b); rb=b-sb; r=chord_root(b); last=(b==NB-1)
    # ---------------- drums
    def d(t,n,v,dur=.1): drums.add(t0+t,dur,n,v)
    if name=='intro':
        if b>=2:
            for k in range(8): d(k*.25+0,HAT,40+k*3) if False else d(k*.25,HAT,38+4*b)
            d(0,KICK,90); kicks.append(t0)
        if b==3:
            for k,n in enumerate([SNARE]*8): d(1+k*.125,n,50+k*9)
    elif name in ('verse1','verse2','bridge','pre','outro') and name!='outro':
        hard=name=='pre'; half=name=='bridge'
        for k in range(8): d(k*.25,HAT if not half else RIDE,62+(10 if k%2==0 else 0))
        pat=[0,1.0] if not half else [0,.75,1.25]
        for tb in ([0,.75,1.0,1.75] if name=='verse2' or hard else [0,1.0,1.5]): d(tb,KICK,104); kicks.append(t0+tb)
        for tb in ([.5,1.5] if not half else [1.0]): d(tb,SNARE,110); snares.append(t0+tb)
        if hard and rb%4==3:
            for k in range(8): d(1+k*.125,SNARE if k<6 else TOM1,70+k*7); 
            snares.append(t0+1.875)
        if rb%4==3 and not hard and not half: d(1.75,TOM2,90); d(1.875,TOM3,96)
        if rb%4==0 and rb>0 or (name=='verse2' and rb==0): d(0,CRASH,100,.5); crashes.append(t0)
    elif name in ('chorus1','chorus2','final'):
        for k in range(8): d(k*.25,OHAT if k%4==3 else HAT,70+(12 if k%2==0 else 0))
        for tb in [0,.5,1.0,1.5]: d(tb,KICK,112 if tb in(0,1.0) else 96); kicks.append(t0+tb)
        for tb in [.5,1.5]: d(tb,SNARE,118); snares.append(t0+tb)
        if rb%4==0: d(0,CRASH,112,.8); crashes.append(t0)
        if rb%4==3: d(1.5,TOM1,100); d(1.625,TOM2,104); d(1.75,TOM3,110); d(1.875,KICK,112); kicks.append(t0+1.875)
    elif name=='build':
        steps=(1,2,4,8)[min(3,rb//2)]; n=int(2/ (1/ (2*steps)) ) if False else None
        div=[.5,.25,.125,.0625][min(3,rb//2)] if rb<8 else .0625
        t=0
        while t<2-1e-6: d(t,SNARE,50+int(75*(rb*2+t)/16)); t+=div
        for tb in [0,1.0]: d(tb,KICK,100); kicks.append(t0+tb)
        snares.append(t0+0.5)
    elif name=='outro':
        if rb==0: d(0,CRASH,120,2.0); d(0,KICK,120); kicks.append(t0); crashes.append(t0)
    # ---------------- bass + guitars
    pm=(name in('verse1','verse2','pre','intro','bridge','build'))   # palm-muted 8ths
    if name=='intro':
        bass.add(t0,BAR*.98,r,70)
        if b>=2:
            for k in range(8): gtrL.add(t0+k*.25,.2,r+12,50+k*2)
    elif name=='outro':
        if rb==0:
            for n in (40,47,52): gtrL.add(t0,6,n+12,100); gtrR.add(t0,6,n+12,100)
            bass.add(t0,5,40,100)
    elif pm:
        sw=[0,0,1,0,0,2,0,3] if name!='bridge' else [0,0,0,0,0,0,0,0]
        for k in range(8):
            n=r+[0,0,7,0,0,10,0,12][k] if name!='bridge' else r
            v=92 if k%2==0 else 76
            if name=='bridge': 
                if k in (0,3,6): gtrL.add(t0+k*.25,.4,r+12,100); gtrR.add(t0+k*.25,.4,r+19,100); bass.add(t0+k*.25,.4,r,98)
                continue
            gtrL.add(t0+k*.25,.2,n+12,v); gtrR.add(t0+k*.25,.2,n+12+(7 if k%4==2 else 0),v-6); bass.add(t0+k*.25,.24,n,v-4)
        if name=='build':
            for k in range(0,8,2): gtrO.add(t0+k*.25,.4,r+24,60+rb*4)
    else:   # chorus: open power chords, half-bar each
        for k,(tb,dur) in enumerate([(0,.5),(.5,.5),(1.0,.5),(1.5,.5)]):
            rr=chord_root(b) if k<3 else chord_root(b)+(2 if b%2 else 0)
            for n in pw(rr): gtrL.add(t0+tb,dur*.95,n,104); gtrR.add(t0+tb,dur*.95,n,104); gtrO.add(t0+tb,dur*.95,n+12,70)
            bass.add(t0+tb,dur*.95,rr,100)
    # ---------------- organ / strings pads
    if name in('pre','chorus1','chorus2','final','build','bridge'):
        for n in (r+12,r+19,r+24): organ.add(t0,BAR,n,44+int(18*E))
    if name in('chorus1','chorus2','final','bridge','outro'):
        for n in (r+12,r+19,r+28): strings.add(t0,BAR+.1,n,40+int(24*E))
    # ---------------- lead guitar: a simple singing melody over choruses, a solo in the bridge/build
    MEL=[[76,79,76,74],[72,74,76,79],[74,76,79,81],[79,76,74,71]]
    if name in('chorus1','chorus2','final'):
        m=MEL[(b//1)%4]
        for k,n in enumerate(m): lead.add(t0+k*.5,.55 if k<3 else .9,n,100)
    if name=='final' and rb>=4:
        m=MEL[(b+1)%4]
        for k,n in enumerate(m): lead.add(t0+k*.5,.5,n+12,98)
    if name=='bridge':
        sol=[[83,81,79,76],[79,81,83,86],[88,86,83,81],[79,76,79,83,]]
        if rb>=4:
            for k,n in enumerate(sol[rb%4]): lead.add(t0+k*.5,.55,n,100)
# the last bar: the full stop
stems=dict(gtrL=gtrL,gtrR=gtrR,gtrO=gtrO,bass=bass,drums=drums,organ=organ,strings=strings,lead=lead)
audio={}
for nme,st in stems.items():
    mid=f'{OUT}/{nme}.mid'; wav=f'{OUT}/{nme}.wav'; st.save(mid)
    subprocess.run(['fluidsynth','-ni','-q','-g','0.9','-r',str(SR),'-F',wav,SF2,mid],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    sr,x=wavfile.read(wav); x=x.astype(np.float32)/32768
    if x.ndim==1: x=np.stack([x,x],1)
    n=int((DUR+8)*SR); y=np.zeros((n,2),np.float32); y[:min(n,len(x))]=x[:n]; audio[nme]=y
hp=lambda x,f:signal.sosfilt(signal.butter(2,f,'hp',fs=SR,output='sos'),x,axis=0)
lp=lambda x,f:signal.sosfilt(signal.butter(2,f,'lp',fs=SR,output='sos'),x,axis=0)
bp=lambda x,a,b:signal.sosfilt(signal.butter(2,[a,b],'bp',fs=SR,output='sos'),x,axis=0)
def amp(x,drive):    # preamp: pre-EQ, asymmetric saturation, cabinet (low-pass + presence)
    y=hp(x,90)*drive; y=np.tanh(y*2.2+.15*y**2)*.7; y=lp(y,5200); y=y+bp(y,1500,3200)*.35; return y
def rev(t60,pre,damp,seed):
    r=np.random.default_rng(seed); n=int(t60*1.3*SR); t=np.arange(n)/SR; ir=r.standard_normal((n,2))*np.exp(-6.9*t/t60)[:,None]
    ir=signal.sosfilt(signal.butter(2,damp,fs=SR,output='sos'),ir,axis=0); ir[:int(pre*SR)]=0; return (ir/np.sqrt((ir**2).sum()/2)).astype(np.float32)
HALL=rev(2.8,.02,5000,1); ROOM=rev(.7,.004,7000,2)
rv=lambda x,ir:np.stack([signal.fftconvolve(x[:,c],ir[:,c])[:len(x)] for c in (0,1)],1)
def norm(x,tg): r=float(np.sqrt((x[int(4*SR):int(DUR*SR)]**2).mean()))+1e-9; return x*tg/r
mix=np.zeros_like(audio['drums'])
L=amp(audio['gtrL'],6)[:,[0,0]]*np.array([1,.25]); R=amp(audio['gtrR'],6)[:,[0,0]]*np.array([.25,1])
L=norm(L,.075); R=norm(R,.075); mix+=L+R
O=norm(amp(audio['gtrO'],3),.03); mix+=O+rv(O,HALL)*.4
B=audio['bass']; B=lp(np.tanh(B*3)*.5,1800); B=norm(B,.07); mix+=B
Dr=norm(audio['drums'],.085); mix+=Dr+rv(Dr,ROOM)*.45
Og=norm(hp(lp(audio['organ'],4000),150),.02); mix+=Og+rv(Og,HALL)*.4
St=norm(hp(lp(audio['strings'],5000),150),.028); mix+=St+rv(St,HALL)*.55
Ld=amp(audio['lead'],4); Ld=norm(Ld,.05); mix+=Ld+rv(Ld,HALL)*.5+np.roll(Ld,int(.25*SR),0)*.12
T=np.arange(len(mix))/SR; mix*=np.minimum(1,T/.5)[:,None]; mix*=np.clip((DUR+6-T)/5.5,0,1)[:,None]
mix=hp(mix,35); mix=lp(mix,15000)
# glue: soft clip + slow gain ride to hold ~-14 LUFS
mix/=np.abs(mix).max(); mix=np.where(np.abs(mix)>.55,np.sign(mix)*(.55+.45*np.tanh((np.abs(mix)-.55)/.45)),mix); mix/=np.abs(mix).max()*1.05
wavfile.write('/tmp/tr/trailer_music.wav',SR,(mix*32000).astype('<i2'))
J=dict(bpm=BPM,len=DUR,kicks=sorted(set(round(k,3) for k in kicks)),snares=sorted(set(round(k,3) for k in snares)),crashes=sorted(set(round(k,3) for k in crashes)),sections=[[s[0],s[1]*BAR] for s in SECS])
open('/tmp/tr/trailer_beats.js','w').write('window.BEATS='+json.dumps(J)+';\n'); print('ok',DUR,len(kicks),len(snares),len(crashes))
