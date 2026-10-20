#!/usr/bin/env python3
"""Score for the KA family presentation: an original, slow piano / strings / pad piece at 60 BPM (one bar = 4 s), written for this film.
Rendered with FluidSynth (FluidR3 GM), long hall reverb, gentle glue. Output: ka_music.wav"""
import numpy as np, subprocess, os, random
from mido import MidiFile, MidiTrack, Message, MetaMessage, bpm2tempo
from scipy.io import wavfile
from scipy import signal
SR=44100; OUT=os.environ.get('OUT','/tmp/ka/music_work'); os.makedirs(OUT,exist_ok=True); SF2='/usr/share/sounds/sf2/FluidR3_GM.sf2'
BAR=4.0; NB=58; DUR=NB*BAR; rnd=random.Random(5)
# D minor / F major colours: Dm9, Bbmaj7, Fmaj7, C6/9 ... then a lift to Gm and A for the middle chapters
CH=[dict(root=38,n=[50,57,60,64,69]), dict(root=34,n=[46,53,57,62,65]), dict(root=41,n=[53,57,60,64,69]), dict(root=36,n=[48,55,60,62,67])]
CH2=[dict(root=43,n=[55,58,62,65,69]), dict(root=45,n=[57,61,64,67,71]), dict(root=38,n=[50,57,60,64,69]), dict(root=34,n=[46,53,57,62,65])]
def chord(b):
    prog=CH2 if 13<=b<44 and (b//4)%2==1 else CH
    return prog[b%4]
def level(b):          # 0..1 intensity per bar
    if b<2: return .15
    if b<12: return .35
    if b<52: return .55+.1*np.sin(b*.5)
    if b<54: return .7
    return .25
class Stem:
    def __init__(s,prog,ch=0): s.ev=[]; s.cc=[]; s.prog=prog; s.ch=ch
    def add(s,t,d,n,v): s.ev.append((max(0,t+rnd.uniform(-.004,.004)),d,n,int(max(1,min(127,v+rnd.randint(-2,2))))))
    def save(s,p):
        m=MidiFile(ticks_per_beat=480); tr=MidiTrack(); m.tracks.append(tr); tr.append(MetaMessage('set_tempo',tempo=bpm2tempo(60))); tr.append(Message('program_change',program=s.prog,channel=s.ch,time=0))
        ms=[]
        for t,d,n,v in s.ev: ms.append((int(t*480),1,Message('note_on',note=n,velocity=v,channel=s.ch))); ms.append((int((t+d)*480),0,Message('note_off',note=n,velocity=0,channel=s.ch)))
        for t,c,v in s.cc: ms.append((int(t*480),0,Message('control_change',control=c,value=int(max(0,min(127,v))),channel=s.ch)))
        ms.sort(key=lambda x:(x[0],x[1])); last=0
        for tk,_,mm in ms: mm.time=tk-last; last=tk; tr.append(mm)
        m.save(p)
piano=Stem(0); pad=Stem(89); strings=Stem(48); cello=Stem(42); harp=Stem(46); bells=Stem(98); bass=Stem(32); horn=Stem(60)
MOT=[[74,72,69,67],[72,70,69,65],[76,74,72,69],[74,72,67,69]]
for b in range(NB):
    t0=b*BAR; c=chord(b); L=level(b); T=c['n']
    # pad and strings: a sustained chord, swelling
    for n in T[:4]: pad.add(t0,BAR+.4,n,34+int(26*L))
    pad.cc.append((t0,11,50+60*L))
    if b>=2 and b<54:
        for n in (T[0]-12,T[2],T[3]): strings.add(t0,BAR+.3,n,40+int(24*L))
        strings.cc.append((t0,11,45+55*L))
    # piano: slow arpeggio, two bars of eighth-note pairs, quieter at the start
    pat=[0,2,3,4,3,2,3,4] if b%2==0 else [1,3,4,3,2,4,3,2]
    step=.5 if b>=2 else 1.0
    if b<56:
        for k in range(int(BAR/step)):
            if b<2 and k%2: continue
            piano.add(t0+k*step,1.4,T[pat[k%len(pat)]]+(12 if (k%8==4 and L>.5) else 0),36+int(22*L)+(6 if k%4==0 else 0))
    # a melody every second bar in the middle, on bells / horn
    if 6<=b<52 and b%2==0:
        mo=MOT[(b//2)%4]
        for k,n in enumerate(mo): harp.add(t0+k*1.0,1.3,n,48+int(14*L))
    if 8<=b<52 and b%4==3: bells.add(t0+2.0,2.5,T[4]+12,40)
    if b>=3 and b<54: bass.add(t0,BAR*.95,c['root'],44+int(14*L)) if b%2==0 else bass.add(t0,BAR*.95,c['root'],40+int(10*L))
    if 12<=b<52: cello.add(t0,2.0,T[0]-12+12,48+int(14*L)); cello.add(t0+2,2.0,T[3]-12+12,46+int(14*L))
    if 52<=b<56: horn.add(t0,BAR,T[0],40)
# the last chord rings
for n in CH[0]['n']: pad.add(54*BAR,16,n,46); piano.add(54*BAR,10,n,46); strings.add(54*BAR,14,n,44)
audio={}
for name,st in dict(piano=piano,pad=pad,strings=strings,cello=cello,harp=harp,bells=bells,bass=bass,horn=horn).items():
    mid=f'{OUT}/{name}.mid'; wav=f'{OUT}/{name}.wav'; st.save(mid)
    subprocess.run(['fluidsynth','-ni','-q','-g','0.8','-r',str(SR),'-F',wav,SF2,mid],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    sr,x=wavfile.read(wav); x=x.astype(np.float32)/32768
    if x.ndim==1: x=np.stack([x,x],1)
    n=int(DUR*SR)+SR*8; y=np.zeros((n,2),np.float32); y[:min(n,len(x))]=x[:n]; audio[name]=y
TG=dict(piano=.05,pad=.034,strings=.034,cello=.02,harp=.02,bells=.012,bass=.028,horn=.014)
for k,tg in TG.items():
    seg=audio[k][4*SR:int(DUR*SR)]; r=float(np.sqrt((seg**2).mean()))+1e-9; audio[k]*=tg/r
def rev(t60,pre,damp,seed):
    r=np.random.default_rng(seed); n=int(t60*1.3*SR); t=np.arange(n)/SR; ir=r.standard_normal((n,2))*np.exp(-6.9*t/t60)[:,None]
    ir=signal.sosfilt(signal.butter(2,damp,fs=SR,output='sos'),ir,axis=0); ir[:int(pre*SR)]=0; return (ir/np.sqrt((ir**2).sum()/2)).astype(np.float32)
HALL=rev(4.2,.03,4200,1)
rv=lambda x:np.stack([signal.fftconvolve(x[:,c],HALL[:,c])[:len(x)] for c in (0,1)],1)
hp=lambda x,f:signal.sosfilt(signal.butter(2,f,'hp',fs=SR,output='sos'),x,axis=0)
lp=lambda x,f:signal.sosfilt(signal.butter(2,f,'lp',fs=SR,output='sos'),x,axis=0)
mix=np.zeros((int(DUR*SR)+SR*8,2),np.float32)
p=hp(audio['piano'],80); mix+=p*.8+rv(p)*.8
pd=hp(lp(audio['pad'],3200),100); mix+=pd*.6+rv(pd)*.5
s=hp(lp(audio['strings'],4000),130); mix+=s*.55+rv(s)*.55
ce=hp(lp(audio['cello'],2800),70); mix+=ce*.6+rv(ce)*.5
h=audio['harp']; mix+=h*.5+rv(h)*.7
bl=audio['bells']; mix+=bl*.4+rv(bl)*.8
bs=lp(audio['bass'],500); mix+=bs*.8
hn=hp(lp(audio['horn'],2600),110); mix+=hn*.5+rv(hn)*.6
T=np.arange(len(mix))/SR; mix*=np.minimum(1,T/1.5)[:,None]; mix*=np.clip((DUR+5-T)/5,0,1)[:,None]**1.4
mix=hp(mix,30); mix=lp(mix,14000); mix/=np.abs(mix).max()*1.1
wavfile.write('/tmp/ka/ka_music.wav',SR,(mix*32000).astype('<i2')); print('ok',len(mix)/SR)
