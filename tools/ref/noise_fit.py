import numpy as np, subprocess, os, sys
from scipy.io import wavfile
S=os.path.dirname(os.path.abspath(__file__)); B=(63,125,250,500,1000,2000,4000,8000)
def bands(p,f0,a,b):
    fs,x=wavfile.read(p); x=x.astype(float); X=x if x.ndim>1 else x[:,None]
    seg=X[int(a*fs):int(b*fs)]; w=np.hanning(len(seg))[:,None]
    P=(np.abs(np.fft.rfft(seg*w,axis=0))**2).sum(1); f=np.fft.rfftfreq(len(seg),1/fs)
    harm=np.zeros(len(f),bool)
    for k in range(1,300): harm|=np.abs(f-k*f0)<0.03*f0
    tot=P[(f>30)&(f<16000)].sum()
    return np.array([10*np.log10(P[(f>=lo)&(f<2*lo)&~harm].sum()/tot+1e-20) for lo in B])
ref={43:(98.83,),55:(197.58,),67:(395.3,)}
R={k:bands(f'{S}/ref/Montre8/0{k}-G.wav',ref[k][0],0.8,3.0) for k in ref}
F={43:97.30,55:194.39,67:387.37}
res=[]
for lv in [float(v) for v in sys.argv[1:]]:
    err=[]; rows=[]
    for k in ref:
        p=f'{S}/n_{k}_{lv}.wav'; subprocess.run([f'{S}/turb',str(k),'-1','1'],env={'DD':'1','WAV':p,'NOISE':str(lv)},capture_output=True)
        m=bands(p,F[k],1.0,2.9); err+=list(m-R[k]); rows.append((k,m))
    e=np.array(err); res.append((lv,np.sqrt(np.mean(e**2)),np.mean(e),rows))
    print('noise %.4f: rms band error %.1f dB, mean %+.1f dB'%(lv,np.sqrt(np.mean(e**2)),np.mean(e)))
best=min(res,key=lambda r:r[1]); print('best',best[0])
print('band Hz        '+''.join('%7d'%b for b in B))
for k,m in best[3]:
    print('G%d ref        '%(k//12-1)+''.join('%7.0f'%v for v in R[k])); print('G%d model      '%(k//12-1)+''.join('%7.0f'%v for v in m))
