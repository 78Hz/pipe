# Harmonic-to-noise: power in +-3% bands around each harmonic vs all the rest, 30 Hz-16 kHz,
# steady part. Model: power sum of the two channels (mouth, top).
import sys, numpy as np
from scipy.io import wavfile
def hnr(p, f0, a, b):
    fs,x=wavfile.read(p); x=x.astype(float)
    X=x if x.ndim>1 else x[:,None]
    seg=X[int(a*fs):int(b*fs)]; w=np.hanning(len(seg))[:,None]
    P=(np.abs(np.fft.rfft(seg*w,axis=0))**2).sum(1); f=np.fft.rfftfreq(len(seg),1/fs)
    band=(f>30)&(f<16000); harm=np.zeros_like(band)
    for k in range(1,int(16000/f0)+1): harm|=np.abs(f-k*f0)<0.03*f0
    hp=P[band&harm].sum(); npow=P[band&~harm].sum()
    # noise share above 2 kHz
    hb=(f>2000)&band
    return 10*np.log10(hp/npow), 10*np.log10(P[hb&harm].sum()/P[hb&~harm].sum())
for p,f0,a,b in [(a,float(b),float(c),float(d)) for a,b,c,d in zip(*[iter(sys.argv[1:])]*4)]:
    t,hf=hnr(p,f0,a,b); print('%-28s harmonic/noise %+5.1f dB overall, %+5.1f dB above 2 kHz'%(p.split('/')[-1],t,hf))
