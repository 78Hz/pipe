# Reference / model sound analysis: pitch, harmonic levels, attack, release.
import sys, wave, numpy as np
def load(p):
    w=wave.open(p); n=w.getnframes(); fs=w.getframerate(); ch=w.getnchannels()
    x=np.frombuffer(w.readframes(n),dtype=np.int16).astype(float)
    if ch>1: x=x.reshape(-1,ch)[:,0]
    return x/32768.0, fs
def env(x,fs,ms=5):
    n=max(1,int(fs*ms/1000)); return np.sqrt(np.convolve(x*x,np.ones(n)/n,'same'))
def analyse(p, f0guess=None):
    x,fs=load(p); e=env(x,fs)
    peak=e.max(); on=np.argmax(e>peak*0.01)
    # steady: the 0.5 s window of most constant level after the attack
    st=e[int(on+0.3*fs):]; 
    # release: last time envelope is above 50% of the steady median
    med=np.median(e[on+int(0.5*fs): on+int(0.5*fs)+int(1.0*fs)]) if len(x)>on+1.6*fs else np.median(st)
    above=np.where(e>0.5*med)[0]; rel=above[-1] if len(above) else len(x)-1
    a=on+int(0.6*fs); b=min(rel-int(0.1*fs), a+int(1.0*fs))
    if b-a<int(0.2*fs): a=on+int(0.25*fs); b=rel-int(0.05*fs)
    seg=x[a:b]*np.hanning(b-a); N=1<<int(np.ceil(np.log2(len(seg)*8)))
    S=np.abs(np.fft.rfft(seg,N)); f=np.fft.rfftfreq(N,1/fs)
    if f0guess is None:
        lo=np.searchsorted(f,25); k=lo+np.argmax(S[lo:np.searchsorted(f,2500)]); f0=f[k]
        # prefer the lowest strong partial (avoid octave error)
        for d in (2,3):
            j=np.argmin(abs(f-f0/d)); 
            if S[j]>0.1*S[k] and f0/d>25: f0=f0/d
    else: f0=f0guess
    def at(fr):
        j=np.searchsorted(f,fr); w=max(3,int(0.02*fr/(f[1]-f[0]))); i=max(1,j-w); jj=j+w; return S[i:jj].max()
    h1=at(f0); H=[20*np.log10(at(k*f0)/h1+1e-12) if k*f0<fs/2*0.95 else None for k in range(1,25)]
    # attack: from 10% to 90% of steady (envelope)
    t10=np.argmax(e>0.1*med); t90=np.argmax(e>0.9*med)
    # release: time from release start (envelope drops below 90% for good) to -40 dB
    t_rel=rel; down=np.where(e[rel:]<med*0.01)[0]; t40=(down[0]/fs) if len(down) else None
    # level jump at release: max envelope in 50 ms after release start vs steady
    r0=np.where(e[:rel]>0.9*med)[0]; r0=r0[-1] if len(r0) else rel
    jump=20*np.log10(e[r0:r0+int(0.05*fs)].max()/med)
    return dict(f0=f0,H=H,attack_ms=(t90-t10)/fs*1e3,attack_periods=(t90-t10)/fs*f0,release_to_m40_s=t40,release_jump_db=jump,steady=(a/fs,b/fs))
if __name__=='__main__':
    for p in sys.argv[1:]:
        r=analyse(p)
        Hs=' '.join(f'{h:+4.0f}' for h in r['H'][1:12] if h is not None)
        print(f"{p:28s} f0 {r['f0']:7.2f}  attack {r['attack_ms']:5.0f} ms = {r['attack_periods']:4.0f} periods  release->-40dB {r['release_to_m40_s'] if r['release_to_m40_s'] is None else round(r['release_to_m40_s'],2)} s  release jump {r['release_jump_db']:+.1f} dB | H2..H12 {Hs}")
