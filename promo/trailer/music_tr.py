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
for b in range(NB):
    t0=b*BAR; name,sb,E=sec(b); rb=b-sb; last=(b==NB-1)
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

# ================================================================ harmony and the guitar parts (E minor; everything stays in E minor pentatonic / natural minor)
# power chords as MIDI notes (root, fifth, octave) and the bass note one octave down
PC={'E':[40,47,52],'G':[43,50,55],'D':[50,57,62],'A':[45,52,57],'C':[48,55,60],'B':[47,54,59]}
BASS={'E':28,'G':31,'D':38,'A':33,'C':36,'B':35}
RIFF_A=[40,40,40,43,40,40,45,43]; RIFF_B=[40,40,40,43,40,47,45,43]       # E2 E2 E2 G2 E2 E2 A2 G2 / ... B2 A2 G2
def fifth(n): return [n,n+7,n+12]
def chord_for(b):
    name,sb,E=sec(b); rb=b-sb
    if name in('chorus1','chorus2','final'): return 'EGDA'[rb%4]
    if name=='pre': return ['C','C','D','D','C','C','D','B'][rb]
    if name=='build': return ['E','E','G','G','D','D','A','B'][rb]
    if name=='bridge': return 'E' if rb<4 else 'EGDB'[rb-4]
    if name=='outro': return 'E'
    return 'E'
RG=[]      # (t, dur, [midi], vel, mute) rhythm guitar events
LEADN=[]   # (t, dur, midi, vel, vib)
for b in range(NB):
    t0=b*BAR; name,sb,E=sec(b); rb=b-sb; ch=chord_for(b); P=PC[ch]
    if name=='intro':
        bass.add(t0,BAR*.98,28,70)
        if b>=2:
            for k in range(8): RG.append((t0+k*.25,.2,fifth(40),52+(10 if k%4==0 else 0)+4*b,True))
    elif name in('verse1','verse2'):
        riff=RIFF_A if rb%2==0 else RIFF_B
        for k in range(8):
            n=riff[k]; acc=k in(0,3,6)
            RG.append((t0+k*.25,.24 if not acc else .3,fifth(n),100 if acc else 82,True)); bass.add(t0+k*.25,.24,n-12,88 if acc else 72)
        if name=='verse2' and rb%2==1:                          # open stab at the end of every second bar
            RG.append((t0+1.5,.5,PC['E'],96,False))
    elif name=='pre':
        for k in range(8):
            RG.append((t0+k*.25,.24,[P[0],P[1]],96 if k%2==0 else 80,True)); bass.add(t0+k*.25,.24,BASS[ch],84 if k%2==0 else 70)
        if rb==7: RG.append((t0+1.5,.5,PC['B'],110,False))
    elif name in('chorus1','chorus2','final'):
        RG.append((t0,.72,P,108,False)); RG.append((t0+.75,.22,[P[0],P[1]],96,True)); RG.append((t0+1.0,.72,P,106,False)); RG.append((t0+1.75,.22,[P[0],P[1]],96,True))
        bass.add(t0,.74,BASS[ch],100); bass.add(t0+.75,.24,BASS[ch],86); bass.add(t0+1.0,.74,BASS[ch],98); bass.add(t0+1.75,.24,BASS[ch],86)
        if name=='final': RG.append((t0,.72,[P[0]+12,P[1]+12],70,False))
        hook={'E':[(71,0,.5),(74,.5,.5),(76,1.0,1.0)],'G':[(74,0,.5),(71,.5,.5),(69,1.0,.5),(71,1.5,.5)],'D':[(69,0,.5),(74,.5,.5),(76,1.0,.5),(74,1.5,.5)],'A':[(76,0,1.0),(74,1.0,.5),(71,1.5,.5)]}[ch]
        for n,st,du in hook: LEADN.append((t0+st,du*.98,n,104,du>=1.0))
        if name=='final' and rb>=4:
            for n,st,du in hook: LEADN.append((t0+st,du*.98,n+12 if n<76 else n+7,80,False))
    elif name=='bridge':
        if rb<4:
            for k in (0,3,5): RG.append((t0+k*.25,.4 if k!=5 else .24,[P[0],P[1]],108,True)); bass.add(t0+k*.25,.4,BASS[ch],100)
            for k in (6,7): RG.append((t0+k*.25,.2,[P[0],P[1]],90,True)); bass.add(t0+k*.25,.2,BASS[ch],80)
        else:
            RG.append((t0,.72,P,104,False)); RG.append((t0+1.0,.72,P,102,False)); bass.add(t0,.74,BASS[ch],96); bass.add(t0+1.0,.74,BASS[ch],94)
            sol={0:[(71,0,.25),(74,.25,.25),(76,.5,.25),(79,.75,.25),(76,1.0,.25),(74,1.25,.25),(71,1.5,.5)],
                 1:[(74,0,.5),(71,.5,.25),(74,.75,.25),(79,1.0,.5),(76,1.5,.5)],
                 2:[(69,0,.25),(71,.25,.25),(74,.5,.25),(76,.75,.25),(79,1.0,.25),(76,1.25,.25),(74,1.5,.5)],
                 3:[(76,0,.25),(79,.25,.25),(81,.5,.25),(83,.75,.9)]}[rb-4]
            for n,st,du in sol: LEADN.append((t0+st,du*.97,n,106,du>=.5))
    elif name=='build':
        for k in range(8):
            RG.append((t0+k*.25,.24,[P[0],P[1]],(92+rb*2) if k%2==0 else 76,True)); bass.add(t0+k*.25,.24,BASS[ch],86 if k%2==0 else 72)
    elif name=='outro':
        if rb==0:
            RG.append((t0,6,[40,47,52,59,64],110,False)); bass.add(t0,5,28,100)
    # organ / strings: root, fifth, octave only (no thirds, so nothing clashes with the pentatonic melody)
    if name in('pre','chorus1','chorus2','final','build','bridge'):
        for n in (P[0]+12,P[1]+12,P[0]+24): organ.add(t0,BAR,n,44+int(18*E))
    if name in('chorus1','chorus2','final','bridge','outro'):
        for n in (P[0]+12,P[1]+12,P[0]+24): strings.add(t0,BAR+.1,n,40+int(24*E))
stems=dict(bass=bass,drums=drums,organ=organ,strings=strings)
audio={}
for nme,st in stems.items():
    mid=f'{OUT}/{nme}.mid'; wav=f'{OUT}/{nme}.wav'; st.save(mid)
    subprocess.run(['fluidsynth','-ni','-q','-g','0.9','-r',str(SR),'-F',wav,SF2,mid],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    sr,x=wavfile.read(wav); x=x.astype(np.float32)/32768
    if x.ndim==1: x=np.stack([x,x],1)
    n=int((DUR+8)*SR); y=np.zeros((n,2),np.float32); y[:min(n,len(x))]=x[:n]; audio[nme]=y

# ---------------------------------------------------------------- guitars: additive plucked strings through a two-stage tube-style amp and a cabinet
def pluck(freq,dur,vel,mute,rng,bright=1.0,vib=False):
    n=int((dur+(.08 if mute else .9))*SR); t=np.arange(n)/SR; out=np.zeros(n)
    dec0=14.0 if mute else 2.2; dec1=.012 if mute else .004
    for k in range(1,26):
        f=freq*k*(1+.00004*k*k)
        if f>9000: break
        pa=np.sin(np.pi*k*.14)                           # pick position
        amp=abs(pa)/k**(.9 if not mute else 1.3)*bright
        d=dec0+dec1*k*k*(1 if not mute else 40)
        ph=2*np.pi*f*t
        if vib: ph=ph+0.0*t
        out+=amp*np.sin(ph+rng.uniform(0,6.28))*np.exp(-t*d*(1 if not mute else 1))
    pick=rng.standard_normal(n)*np.exp(-t/.004)*.06; out+=pick
    env=np.minimum(1,t/.003)
    if mute: env*=np.clip((dur+.04-t)/.04,0,1)
    else: env*=np.clip((dur+.5-t)/.5,0,1)**.5
    return out*env*(vel/127)**1.4
def vib_pluck(freq,dur,vel,rng):
    n=int((dur+.5)*SR); t=np.arange(n)/SR; depth=np.clip((t-.25)/.4,0,1)*.0035; fm=freq*(1+depth*np.sin(2*np.pi*5.4*t)); ph=2*np.pi*np.cumsum(fm)/SR; out=np.zeros(n)
    for k in range(1,18):
        if freq*k>8000: break
        out+=np.sin(k*ph+rng.uniform(0,6.28))/k**1.1
    env=np.minimum(1,t/.006)*np.clip((dur+.3-t)/.3,0,1)*(1-.2*np.clip((t-dur)*8,0,1)); return out*env*(vel/127)**1.3
mf=lambda m:440*2**((m-69)/12)
def render_events(ev,seed,lead=False):
    rng=np.random.default_rng(seed); y=np.zeros(int((DUR+8)*SR))
    for e in ev:
        if lead:
            t,du,n,v,vb=e; w=vib_pluck(mf(n),du,v,rng) if vb else pluck(mf(n),du,v,False,rng,1.2)
        else:
            t,du,notes,v,mute=e; t+=rng.uniform(-.004,.004)
            ws=[pluck(mf(n)*(1+rng.uniform(-.0006,.0006)),du,v-4*j,mute,rng,1.0)*(1.0 if j==0 else .85) for j,n in enumerate(notes)]
            w=np.zeros(max(len(x) for x in ws))
            for j,x in enumerate(ws):
                o=int(j*.004*SR); w[o:]+=x[:len(w)-o]
        i=int(max(0,t)*SR); m=min(len(w),len(y)-i); y[i:i+m]+=w[:m]
    return y
def amp_chain(x,gain,lead=False):
    x=signal.sosfilt(signal.butter(2,100,'hp',fs=SR,output='sos'),x)
    x=signal.resample_poly(x,2,1); fs2=SR*2
    g=gain/ (np.abs(x).max()+1e-9)*.9
    y=np.tanh(x*g*4+.25*(x*g)**2)                                     # stage 1: asymmetric clip
    y=signal.sosfilt(signal.butter(2,[140,5200],'bp',fs=fs2,output='sos'),y)
    y=np.tanh(y*(2.2 if not lead else 1.6))                           # stage 2
    y=signal.resample_poly(y,1,2)
    # cabinet: low roll-off, a presence peak, steep top cut
    y=signal.sosfilt(signal.butter(2,90,'hp',fs=SR,output='sos'),y)
    b,a=signal.iirpeak(2400/(SR/2),3.0); y=y+.6*signal.lfilter(b,a,y)
    b,a=signal.iirpeak(180/(SR/2),2.0); y=y+.35*signal.lfilter(b,a,y)
    y=signal.sosfilt(signal.butter(4,4800,'lp',fs=SR,output='sos'),y)
    return y
rgL=amp_chain(render_events(RG,1),30); rgR=amp_chain(render_events(RG,2),30)
ldA=amp_chain(render_events(LEADN,3,lead=True),12,lead=True)

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
L=np.stack([rgL,rgL*.3],1); R=np.stack([rgR*.3,rgR],1)
def nrm(x,tg): r=float(np.sqrt((x[int(4*SR):int(DUR*SR)]**2).mean()))+1e-9; return x*tg/r
L=nrm(L,.075); R=nrm(R,.075); L=np.pad(L,((0,len(mix)-len(L)),(0,0)))[:len(mix)]; R=np.pad(R,((0,len(mix)-len(R)),(0,0)))[:len(mix)]; mix+=L+R
B=audio['bass']; B=lp(np.tanh(B*3)*.5,1800); B=norm(B,.07); mix+=B
Dr=norm(audio['drums'],.085); mix+=Dr+rv(Dr,ROOM)*.45
Og=norm(hp(lp(audio['organ'],4000),150),.02); mix+=Og+rv(Og,HALL)*.4
St=norm(hp(lp(audio['strings'],5000),150),.028); mix+=St+rv(St,HALL)*.55
Ld=np.stack([ldA,ldA],1); Ld=np.pad(Ld,((0,len(mix)-len(Ld)),(0,0)))[:len(mix)]; Ld=nrm(Ld,.055); mix+=Ld+rv(Ld,HALL)*.5+np.roll(Ld,int(.25*SR),0)*.14

if os.environ.get('DBG'):
    def pcs(x,a,b):
        N=1<<16; w=np.hanning(N); acc=np.zeros(12); seg=x[int(a*SR):int(b*SR)]
        for q in range(0,len(seg)-N,N//2):
            S=np.abs(np.fft.rfft(seg[q:q+N]*w)); f=np.fft.rfftfreq(N,1/SR); m=(f>70)&(f<1400); pc=np.round(12*np.log2(f[m]/440)+69).astype(int)%12; np.add.at(acc,pc,S[m]**2)
        return acc/acc.sum()
    nmz=['C','C#','D','D#','E','F','F#','G','G#','A','A#','B']
    for nm,x_ in (('rhythm',L[:,0]+R[:,1]),('lead',Ld[:,0]),('bass',B[:,0]),('drums',Dr[:,0]),('organ',Og[:,0]),('strings',St[:,0])):
        for a,b in ((8,24),(104,120)): c=pcs(x_,a,b); print(nm,a,' '.join(f'{n}:{v*100:3.0f}' for n,v in zip(nmz,c)))
T=np.arange(len(mix))/SR; mix*=np.minimum(1,T/.5)[:,None]; mix*=np.clip((DUR+6-T)/5.5,0,1)[:,None]
mix=hp(mix,35); mix=lp(mix,15000)
# glue: soft clip + slow gain ride to hold ~-14 LUFS
mix/=np.abs(mix).max(); mix=np.where(np.abs(mix)>.55,np.sign(mix)*(.55+.45*np.tanh((np.abs(mix)-.55)/.45)),mix); mix/=np.abs(mix).max()*1.05
wavfile.write('/tmp/tr/trailer_music.wav',SR,(mix*32000).astype('<i2'))
J=dict(bpm=BPM,len=DUR,kicks=sorted(set(round(k,3) for k in kicks)),snares=sorted(set(round(k,3) for k in snares)),crashes=sorted(set(round(k,3) for k in crashes)),sections=[[s[0],s[1]*BAR] for s in SECS])
open('/tmp/tr/trailer_beats.js','w').write('window.BEATS='+json.dumps(J)+';\n'); print('ok',DUR,len(kicks),len(snares),len(crashes))
