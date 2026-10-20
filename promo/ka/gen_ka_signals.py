#!/usr/bin/env python3
"""Draws the six KA waterfall signatures from the parameters in the KA family brief and writes them as PNGs (/tmp/ka/assets). Nothing here is a recording:
every picture is the spectrogram of a signal synthesised in this script (root-raised-cosine QAM data plus the mode's sweep / chirp / staircase / lattice, plus noise).
Also writes the KAIT picture ladder: one synthetic scene blurred until its PSNR equals the PSNR the KAIT measurement file lists for the number of layers received."""
import numpy as np, os
from scipy.ndimage import gaussian_filter, maximum_filter1d
from PIL import Image
OUT='/tmp/ka/assets'; os.makedirs(OUT,exist_ok=True); rng=np.random.default_rng(7)
PAL=np.array([[0,5,10,16],[.18,12,36,84],[.4,20,96,168],[.6,70,184,200],[.82,150,222,238],[1,255,250,240]],float)
def cmap(v):
    v=np.clip(v,0,1); out=np.zeros(v.shape+(3,),float)
    for c in range(3): out[...,c]=np.interp(v,PAL[:,0],PAL[:,c+1])
    return out.astype(np.uint8)
def rrc(beta,span,sps):
    t=np.arange(-span*sps,span*sps+1)/sps; h=np.zeros_like(t)
    for i,x in enumerate(t):
        if abs(x)<1e-9: h[i]=1-beta+4*beta/np.pi
        elif abs(abs(x)-1/(4*beta))<1e-9: h[i]=beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta))+(1-2/np.pi)*np.cos(np.pi/(4*beta)))
        else: h[i]=(np.sin(np.pi*x*(1-beta))+4*beta*x*np.cos(np.pi*x*(1+beta)))/(np.pi*x*(1-(4*beta*x)**2))
    return h/np.sqrt((h**2).sum())
def qam(n,m):
    if m==2: return rng.integers(0,2,n)*2.0-1+0j
    k=int(np.sqrt(m)); a=(rng.integers(0,k,n)*2-(k-1))+1j*(rng.integers(0,k,n)*2-(k-1)); return a/np.sqrt(((np.arange(k)*2-(k-1))**2).mean()*2)
def shaped(nsym,m,sps,beta):
    s=qam(nsym,m); u=np.zeros(nsym*sps,complex); u[::sps]=s; from scipy.signal import fftconvolve; y=fftconvolve(u,rrc(beta,8,sps),'same'); return y/np.sqrt((abs(y)**2).mean())
def noise(n,p): return np.sqrt(p/2)*(rng.standard_normal(n)+1j*rng.standard_normal(n))
def spec(x,fs,nfft,hop,fmin,fmax,pool=1):
    w=np.hanning(nfft); n=(len(x)-nfft)//hop; idx=np.arange(nfft)[None,:]+hop*np.arange(n)[:,None]
    S=np.zeros((n,nfft),np.float32)
    for a in range(0,n,512): S[a:a+512]=np.abs(np.fft.fftshift(np.fft.fft(x[idx[a:a+512]]*w,axis=1),axes=1))**2
    f=np.fft.fftshift(np.fft.fftfreq(nfft,1/fs)); S=S[:,(f>=fmin)&(f<fmax)]
    if pool>1: k=S.shape[1]//pool*pool; S=S[:,:k].reshape(n,-1,pool).max(2)
    return 10*np.log10(S+1e-12)
def save(name,D,lo=None,hi=None,boost=1.0):
    lo=np.percentile(D,12) if lo is None else lo; hi=np.percentile(D,99.8) if hi is None else hi
    v=np.clip((D-lo)/(hi-lo),0,1)**boost; Image.fromarray(cmap(v)).save(f'{OUT}/{name}.png'); print(name,D.shape)
def chirp(n,f0,f1,fs,ph0=0):
    f=np.linspace(f0,f1,n); return np.exp(1j*(ph0+2*np.pi*np.cumsum(f)/fs))
# ---- KAAL: 8,000 sym/s 16-QAM, root-raised-cosine 0.25 (10 kHz), a sweep every 0.505 s at -10 dB, 48 kHz
fs=48000; N=fs*12; x=shaped(8000*12,16,6,.25); P=24240; sw=np.zeros(N,complex); var=['up','down','up','up','down','down','up','down','up','up','down','up']
for i in range(N//P+1):
    a=i*P; b=min(N,a+P); f0,f1=(-5000,5000) if var[i%len(var)]=='up' else (5000,-5000); sw[a:b]=chirp(b-a,f0,f1,fs)[:b-a]
x=x[:N]+0.316*sw+noise(N,.03); save('kaal',spec(x,fs,1024,240,-12000,12000))
# ---- KAPP: the start of one burst (first ~24 ms): ramp, preamble up-chirp then down-chirp, header, payload; 80,000 sym/s, 100 kHz
fs=320000; sym=80000; dat=lambda n,m:shaped(n,m,4,.25)
pre_n=254*4; up=chirp(127*4,-50000,50000,fs); dn=chirp(127*4,50000,-50000,fs); hdr=dat(688,2)[:688*4]; pay=dat(1500,16)[:1500*4]
b=np.concatenate([np.zeros(2000,complex),up,dn,hdr,pay]); b=b+noise(len(b),.03)
save('kapp',spec(b,fs,256,8,-90000,90000))
# ---- KAFT: staircase of 8 tones (12.5 ms per step) across the 1 MHz channel, then data frames, 3.2 Msps
fs=3200000; stair=np.zeros(int(.1*fs),complex); L=int(.0125*fs); fr=np.linspace(-420000,420000,8)
for k in range(8): n=np.arange(L); stair[k*L:(k+1)*L]=np.exp(2j*np.pi*fr[k]*n/fs)
data=shaped(int(.14*800000),16,4,.25); xx=np.concatenate([np.zeros(int(.02*fs),complex),.45*stair,data[:int(.14*fs)],.45*stair,data[:int(.06*fs)]]); xx=xx+noise(len(xx),.03)
save('kaft',spec(xx,fs,2048,1600,-600000,600000,pool=2))
# ---- KAVB profile A: 700,000 sym/s 16-QAM (875 kHz), V sweep (down across the band, back up) every 0.5 s at -10 dB, 2.8 Msps
fs=2800000; T=4; data=shaped(700000*T,16,4,.25); N=len(data); Pn=int(.5*fs); v=np.zeros(N,complex); half=Pn//2
for i in range(N//Pn+1):
    a=i*Pn; seg=np.concatenate([chirp(half,437000,-437000,fs),chirp(Pn-half,-437000,437000,fs)]); m=min(N-a,Pn); v[a:a+m]=seg[:m]
xx=data+.316*v+noise(N,.03); save('kavb',spec(xx,fs,4096,5600,-700000,700000,pool=4))
# ---- KAIT: up-sweep, down-sweep (40 ms each, -85..+85 kHz), the 10 s data block (158,000 sym/s, 190 kHz), up-sweep, down-sweep; 632 ksps
fs=632000; up=chirp(int(.04*fs),-85000,85000,fs); dn=chirp(int(.04*fs),85000,-85000,fs); data=shaped(158000*10,16,4,.2)
xx=np.concatenate([np.zeros(int(.12*fs),complex),up,dn,np.zeros(int(.06*fs),complex),data,np.zeros(int(.06*fs),complex),up,dn,np.zeros(int(.12*fs),complex)]); xx=xx+noise(len(xx),.03)
save('kait_all',spec(xx,fs,1024,6320,-150000,150000)); save('kait_start',spec(xx[:int(.4*fs)],fs,1024,316,-150000,150000))
# ---- KAWS: preamble lattice (8 chirps of 1.0 s over 1,600 Hz, alternating direction, one every 0.25 s) and a 7-tone comb, then SF8 chirp symbols whose glide direction follows the 0xACE1 register; 12 ksps, centre 1,500 Hz
fs=12000; Tt=11.0; N=int(Tt*fs); x=np.zeros(N,complex); t=np.arange(N)/fs
def lfsr(seed=0xACE1):
    s=seed
    while True:
        bit=((s>>0)^(s>>2)^(s>>3)^(s>>5))&1; s=(s>>1)|(bit<<15); yield bit
g=lfsr()
for k in range(8):
    a=int((0.4+k*.25)*fs); n=int(1.0*fs); c=chirp(n,-800,800,fs) if k%2==0 else chirp(n,800,-800,fs); x[a:a+n]+=.62*c[:min(n,N-a)]
for k,fq in enumerate([450,1500,2550,1150,2200,800,1850]):
    a=int((0.7+k*.4)*fs); n=int(.4*fs); x[a:a+n]+=.55*np.exp(2j*np.pi*(fq-1500)*np.arange(n)/fs)
SF=8; Ns=int(2**SF/2400*fs); pos=int(4.4*fs)
while pos+Ns<N:
    s=rng.integers(0,2**SF); d=1 if next(g) else -1; ph=(s/2**SF+np.arange(Ns)/Ns)%1; f=(-1200+2400*ph)*d; x[pos:pos+Ns]+=.6*np.exp(2j*np.pi*np.cumsum(f)/fs); pos+=Ns
x=x+noise(N,.05); save('kaws',spec(x,fs,512,48,-1500,1500))
# ---- the KAIT picture ladder: a synthetic scene, blurred until its PSNR matches the measured value for each number of received layers
H,W=540,960; yy,xx_=np.mgrid[0:H,0:W]/np.array([H,W])[:,None,None]
sky=np.stack([.10+.55*yy**1.5,.16+.5*yy**1.2,.34+.45*yy],-1); sun=np.exp(-(((xx_-.68)*1.8)**2+((yy-.52))**2)/.012)[...,None]*np.array([1,.8,.45]); img=sky+sun
def ridge(base,amp,seed,col,freqs):
    r=np.random.default_rng(seed); h=base+sum(amp/k*np.sin(2*np.pi*(k*xx_[0]*freqs+r.random())) for k in range(1,7))/2
    return h
for base,amp,seed,col in ((.62,.07,1,(.18,.24,.34)),(.72,.06,2,(.10,.16,.20)),(.82,.05,3,(.04,.09,.10))):
    r=np.random.default_rng(seed); hh=base+sum(amp/k*np.sin(2*np.pi*(k*xx_[0]*1.3+r.random())) for k in range(1,9))*.7; m=(yy>hh)[...,None]; img=np.where(m,np.array(col)+.03*r.standard_normal((H,W,1)),img)
hw=.004+.03*np.clip((yy-.3)/.44,0,1); legs=(abs(abs(xx_-.22)-hw)<.0025)&(yy>.3)&(yy<.74); bars=(abs(xx_-.22)<hw)&(np.abs(((yy-.3)/.44*7)%1-.5)>.47)&(yy>.3)&(yy<.74); spike=(abs(xx_-.22)<.0015)&(yy>.22)&(yy<.3); mast=legs|bars|spike; img=np.where(mast[...,None],np.array([.92,.93,.95]),img)
tex=gaussian_filter(rng.standard_normal((H,W,1)),(.8,.8,0))*.09+gaussian_filter(rng.standard_normal((H,W,1)),(6,6,0))*.12; img=np.clip(img+tex,0,1); img8=(img*255).astype(np.uint8); Image.fromarray(img8).save(f'{OUT}/pic_orig.png')
def psnr(a,b): return 10*np.log10(255**2/np.mean((a.astype(float)-b.astype(float))**2))
TARGETS=[('0',18.1),('1',20.7),('2',25.1),('3',28.3),('4',32.0),('5',40.5),('6',46.8)]
for name,tg in TARGETS:
    lo,hi=.0,200.
    for _ in range(30):
        mid=(lo+hi)/2; b=gaussian_filter(img8.astype(float),(mid,mid,0)); (lo,hi)=(mid,hi) if psnr(img8,np.clip(b,0,255).astype(np.uint8))>tg else (lo,mid)
    b=gaussian_filter(img8.astype(float),(mid,mid,0)); b=np.clip(b+ (rng.standard_normal(b.shape)*0.4 if tg>44 else 0),0,255).astype(np.uint8)
    Image.fromarray(b).save(f'{OUT}/pic_{name}.jpg',quality=92); print('pic',name,round(psnr(img8,b),1),'sigma',round(mid,2))
