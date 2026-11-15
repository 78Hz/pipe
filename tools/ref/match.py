# Match the unmeasured constants of the flue model to reference recordings.
# Score per note: harmonics H1..H(16 kHz) in dB re H1 (skipping those within 6 dB of the reference's
# own noise floor), inter-harmonic noise in octave bands re total power, attack 10-90 % time.
# Model: power sum of mouth and top. One set of constants for the whole stop.
import numpy as np, subprocess, os, sys, json
from scipy.io import wavfile
from scipy.optimize import minimize
EVAL, REF, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
TRAIN, HOLD = [43, 55, 67], [42, 54, 66, 79]
NAMES = {42:'042-F#',43:'043-G',54:'054-F#',55:'055-G',66:'066-F#',67:'067-G',78:'078-F#',79:'079-G'}
BANDS = (125, 250, 500, 1000, 2000, 4000, 8000)

def spectrum(X, fs):
    w = np.hanning(len(X))[:, None]; P = (np.abs(np.fft.rfft(X * w, axis=0)) ** 2).sum(1)
    return P, np.fft.rfftfreq(len(X), 1 / fs)
def f0of(P, f, guess):
    m = (f > guess * 0.9) & (f < guess * 1.1); return f[m][np.argmax(P[m])]
def features(X, fs, a, b, guess, onset):
    P, f = spectrum(X[int(a*fs):int(b*fs)], fs); f0 = f0of(P, f, guess)
    K = int(min(40, 16000 / f0)); H = []; floor = []
    for k in range(1, K + 1):
        m = np.abs(f - k * f0) < 0.02 * f0; H.append(10 * np.log10(P[m].max() + 1e-30))
        n = (np.abs(f - k * f0) > 0.3 * f0) & (np.abs(f - k * f0) < 0.5 * f0)
        floor.append(10 * np.log10(np.median(P[n]) * m.sum() + 1e-30))   # noise in the same bandwidth
    H = np.array(H); floor = np.array(floor)
    harm = np.zeros(len(f), bool)
    for k in range(1, 400): harm |= np.abs(f - k * f0) < 0.03 * f0
    tot = P[(f > 30) & (f < 16000)].sum()
    N = np.array([10 * np.log10(P[(f >= lo) & (f < 2 * lo) & ~harm].sum() / tot + 1e-30) for lo in BANDS])
    x = X.sum(1) if X.shape[1] > 1 else X[:, 0]
    e = np.sqrt(np.convolve(x * x, np.ones(int(0.005 * fs)) / int(0.005 * fs), 'same'))
    med = np.median(e[int(a*fs):int(b*fs)]); s = e[onset:]
    att = (np.argmax(s > 0.9 * med) - np.argmax(s > 0.1 * med)) / fs * 1e3
    return dict(f0=f0, H=H - H[0], Hfloor=floor - H[0], N=N, att=max(att, 1.0))

def ref_features(k):
    fs, x = wavfile.read(f'{REF}/{NAMES[k]}.wav'); x = x.astype(float); X = x[:, None] if x.ndim == 1 else x
    e = np.abs(X[:, 0]); on = np.argmax(np.convolve(e, np.ones(220) / 220, 'same') > 0.01 * e.max())
    return features(X, fs, 0.8, 3.0, 440 * 2 ** ((k - 69) / 12), on)
R = {k: ref_features(k) for k in TRAIN + HOLD}

def render(params, keys):
    subprocess.run([EVAL, OUT] + ['%.6g' % p for p in params] + [str(k) for k in keys], check=True)
    out = {}
    for k in keys:
        X = np.fromfile(f'{OUT}/{k}.f32', np.float32).astype(float).reshape(-1, 2)
        if not np.isfinite(X).all() or np.abs(X).max() < 1e-9: out[k] = None; continue
        out[k] = features(X, 48000, 1.5, 3.2, R[k]['f0'] if k in R else 440 * 2 ** ((k - 69) / 12), 0)
    return out

def note_error(m, r):
    if m is None: return 1e4, {}
    K = min(len(m['H']), len(r['H'])); ok = r['H'][:K] > r['Hfloor'][:K] + 6
    w = np.where(np.arange(1, K + 1) <= 10, 1.0, 0.5) * ok
    dh = (m['H'][:K] - r['H'][:K]); eh = np.sum(w * dh ** 2) / max(1, w.sum())
    nb = np.array([lo >= r['f0'] / 1.5 for lo in BANDS]); dn = (m['N'] - r['N'])[nb]; en = np.mean(dn ** 2)
    ea = (10 * np.log10(m['att'] / r['att'])) ** 2
    pitch = 1200 * np.log2(m['f0'] / r['f0'])
    return eh + 0.5 * en + 0.5 * ea, dict(harm_rms=np.sqrt(eh), noise_rms=np.sqrt(en), att_m=m['att'], att_r=r['att'], pitch_c=pitch)

def speaks_fundamental(params):
    subprocess.run([EVAL, OUT] + ['%.6g' % p for p in params] + ['36'], check=True)
    X = np.fromfile(f'{OUT}/36.f32', np.float32).astype(float).reshape(-1, 2)
    P, f = spectrum(X[int(1.5*48000):], 48000); f1 = 65.4
    lvl = [P[np.abs(f - k * f1) < 0.06 * f1].max() for k in (1, 2, 3, 4)]
    return lvl[0] >= max(lvl[1:])

LO = np.array([-1.5, np.log(0.1), np.log(0.3), np.log(0.3), np.log(0.003)])
HI = np.array([1.5, np.log(10), np.log(3), np.log(3), np.log(0.3)])
def params_of(z):
    z = np.clip(z, LO, HI); return [z[0], np.exp(z[1]), np.exp(z[2]), np.exp(z[3]), np.exp(z[4])]
log = open(f'{OUT}/log.txt', 'a')
def cost(z):
    p = params_of(z); M = render(p, TRAIN); c = 0; det = {}
    for k in TRAIN: e, d = note_error(M[k], R[k]); c += e; det[k] = d
    c /= len(TRAIN)
    if not speaks_fundamental(p): c += 200
    log.write(json.dumps(dict(p=p, cost=c)) + '\n'); log.flush()
    return c

if __name__ == '__main__':
    starts = [[0.5, 0, 0, 0, np.log(0.04)], [0.0, np.log(0.5), 0, 0, np.log(0.04)], [1.0, np.log(2), np.log(0.7), 0, np.log(0.04)]]
    best = None
    for z0 in starts:
        z0 = np.array(z0, float); step = np.array([0.4, 0.7, 0.5, 0.5, 0.7])
        simplex = np.vstack([z0] + [z0 + np.eye(5)[i] * step[i] for i in range(5)])
        r = minimize(cost, z0, method='Nelder-Mead', options=dict(maxfev=200, xatol=1e-2, fatol=0.05, initial_simplex=simplex))
        print('start', z0, '->', params_of(r.x), 'cost %.1f' % r.fun, flush=True)
        if best is None or r.fun < best.fun: best = r
    p = params_of(best.x); json.dump(dict(params=p, cost=best.fun), open(f'{OUT}/best.json', 'w'))
    print('BEST y0b %.3f lpk %.3f gscale %.3f kd %.3f noise %.4f cost %.1f' % tuple(p + [best.fun]))
    M = render(p, TRAIN + HOLD)
    for k in TRAIN + HOLD:
        e, d = note_error(M[k], R[k])
        print('%s %-7s score %.1f  harmonics rms %.1f dB  noise rms %.1f dB  attack %.0f / ref %.0f ms  pitch %+.0f c' % ('train' if k in TRAIN else 'HOLD ', NAMES[k], e, d['harm_rms'], d['noise_rms'], d['att_m'], d['att_r'], d['pitch_c']))
    print('bottom C speaks on fundamental:', speaks_fundamental(p))
