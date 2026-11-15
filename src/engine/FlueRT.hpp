// Pipe - physically modelled pipe organ
// Real-time flue pipe: the same physics as flue::FlueSim with the matched jet (turbulent jet at
// the lip, Verge's source spacing on its thickness, arrival-order delay, vortex loss, volume
// injection, turbulence noise, pallet / foot / windway wind path), written for speed:
//  - the foot pressure is solved by Newton's method from the previous sample (2 steps; the
//    reference solves by 40 bisections) and skipped when the wind is steady;
//  - coefficients that depend only on the jet speed are recomputed only when it changes;
//  - the resonator modes are a flat array the compiler can vectorise;
//  - power-of-two ring buffer for the jet delay.
// The pipe is described by PipeSpec (continuous-time data, independent of the sample rate), so the
// whole organ can be precomputed and stored. FlueSim stays the reference; tests compare the two.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace pipe {

constexpr int kRtMaxModes = 24;

// One flue pipe, as built and voiced (SI units). Modes: admittance Y_n(s) = (A s + B)/(s^2 + s wn/Q + wn^2)
// with A, B from the complex peak value; topRe/topIm = flow ratio top/mouth at the mode.
struct FluePipeSpec {
    float target = 0;          // Hz the pipe is cut to
    float h = 0, W = 0, Wm = 0;            // flue width, cut-up, mouth width
    float languid = 0.002f, radius = 0, toe = 0;   // windway length, body radius, toe-hole diameter (0 open)
    float footPa = 0;          // voiced foot pressure (for the noise centre)
    float length = 0;          // body length (m), as cut
    float shape = 0;           // 0 open, 1 stopped, 2 chimney, 3 conical (display)
    float wood = 0;            // 1 = wood (display)
    float b = 0, transit = 0, lipSpeed = 1;        // turbulent jet at the lip (see FlueSim::init)
    // matched constants of the family
    float y0b = 0.5f, lpk = 1.f, gscale = 1.f, kd = 1.f, noise = 0.f, sign = -1.f;
    float vortex = 1.f;        // vortex-shedding loss at the lip, x the sharp-edge value (contraction 0.6)
    int nModes = 0;
    float f[kRtMaxModes], Q[kRtMaxModes], pkRe[kRtMaxModes], pkIm[kRtMaxModes], topRe[kRtMaxModes], topIm[kRtMaxModes];
};

// tanh by its [9/8] continued-fraction (Lambert) approximant, limited to +-1: the approximant reaches
// 1 at |x| = 6.297, so the limit is continuous; |error| < 6.8e-6 everywhere.
inline double fastTanh(double x) {
    const double x2 = x * x;
    const double a = x * (34459425.0 + x2 * (4729725.0 + x2 * (135135.0 + x2 * (990.0 + x2))));
    const double b = 34459425.0 + x2 * (16216200.0 + x2 * (945945.0 + x2 * (13860.0 + x2 * 45.0)));
    const double t = a / b;
    return t > 1.0 ? 1.0 : t < -1.0 ? -1.0 : t;
}

class FluePipeRT {
public:
    // chestPa: wind in the chest under the pallet. pitch: frequency ratio applied to the resonator and
    // the jet together (a geometrically similar pipe), for tuning and bends.
    void init(const FluePipeSpec& s, double sampleRate, uint32_t seed) {
        sp = s; os = std::max(1, (int)std::ceil(s.target * 24.0 / sampleRate)); fs = sampleRate * os; dt = 1.0 / fs;
        rho = kRho; c2rho = rho * 343.21 * 343.21; mu = 1.807e-5;   // air at 20 C (as resonator::Air)
        toeArea = s.toe > 0 ? M_PI * 0.25 * (double)s.toe * s.toe : 0.0;
        windR = 12.0 * mu * s.languid / ((double)s.h * s.h);
        lc = s.languid;
        const double D = 2.0 * s.radius, Lf = std::max(0.150, 2.5 * D);
        volume = 0.3e-3 + M_PI / 12.0 * D * D * Lf;
        Sm = (double)s.W * s.Wm;
        deltaOut = 0.82 * std::sqrt(Sm / M_PI);
        const double hLip = s.b / 0.4;
        ddE = 4.0 / M_PI * std::sqrt(2.0 * hLip * s.W);
        driveK = -(rho * ddE * s.kd / Sm);
        Gh = s.gscale * 2.0 * s.b / 0.10;
        vortexK = 0.5 * rho / (0.6 * 0.6) * (s.vortex > 0 ? s.vortex : 1.0);    // gain * h, gain = gscale * 2 (b/h) / 0.10
        rng = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x2545F4914F6CDD1Dull); if (!rng) rng = 1;
        {
            const double fc = 0.4 * fs, K = std::tan(M_PI * fc / fs), q = 1.0 / std::sqrt(2.0);
            const double nrm = 1.0 / (1.0 + K / q + K * K);
            lp = { K * K * nrm, 2 * K * K * nrm, K * K * nrm, 2 * (K * K - 1) * nrm, (1 - K / q + K * K) * nrm, 0, 0 };
        }
        // noise band-pass (Verge St 0.05 on the jet at the lip, voiced pressure)
        {
            const double U = jetSpeed(s.footPa > 0 ? s.footPa : 775.0);
            const double fc = std::min(0.4 * fs, 0.05 * s.lipSpeed * U / hLip), Qn = 0.7, w0 = 2 * M_PI * fc / fs;
            const double al = std::sin(w0) / (2 * Qn), a0 = 1 + al;
            nb0 = al / a0; nb2 = -al / a0; na1 = -2 * std::cos(w0) / a0; na2 = (1 - al) / a0;
            nnorm = 1.0 / std::sqrt(2.0 * (M_PI / 2.0 * fc / Qn) / fs);
        }
        pitch = 0; setPitch(1.0);
        reset();
    }
    void reset() {
        footP = 0; palletPhase = 0; palletPos = 0; palletTarget = keyDown ? 1.0 : 0.0; Uslot = 0; qjPrev = 0; qinPrev = 0; vPrev = 0;
        etaLP = 0; tauPrev = 1e9; ucache = -1; qMouthPrev = 0; sTopPrev = 0; nz1 = nz2 = 0;
        lp.z1 = lp.z2 = 0; std::memset(z1, 0, sizeof z1); std::memset(z2, 0, sizeof z2); std::memset(yPrev, 0, sizeof yPrev);
        std::memset(hist, 0, sizeof hist); pos = 0; steadyCount = 0;
    }
    void setPitch(double ratio) {
        if (ratio == pitch) return;
        pitch = ratio;
        n = sp.nModes; nPad = std::min(kRtMaxModes, (n + 3) & ~3);
        for (int i = 0; i < kRtMaxModes; ++i) {
            if (i >= n) { b0[i] = b1[i] = b2[i] = a1[i] = a2[i] = cA[i] = cI[i] = 0; continue; }
            const double wn = 2 * M_PI * sp.f[i] * ratio, Q = sp.Q[i];
            if (wn >= M_PI * fs * 0.98) { b0[i] = b1[i] = b2[i] = a1[i] = a2[i] = cA[i] = cI[i] = 0; continue; }
            const double A = sp.pkRe[i] * wn / Q, B = -sp.pkIm[i] * wn * wn / Q;
            const double K = wn / std::tan(wn * dt / 2), a0 = K * K + K * wn / Q + wn * wn;
            b0[i] = (A * K + B) / a0; b1[i] = 2 * B / a0; b2[i] = (-A * K + B) / a0;
            a1[i] = 2 * (wn * wn - K * K) / a0; a2[i] = (K * K - K * wn / Q + wn * wn) / a0;
            // top = re * y + im * (dy/dt)/w  ->  (re + im fs/w) y - (im fs/w) y[n-1]
            cI[i] = sp.topIm[i] * fs / wn; cA[i] = sp.topRe[i] + cI[i];
        }
        transitS = sp.transit / ratio;   // the jet scales with the pipe
        ucache = -1;
    }
    void key(bool down) { keyDown = down; palletTarget = down ? 1.0 : 0.0; }
    // partial pallet opening (pressure-driven keys): the pallet moves to this opening 0..1
    void setPalletTarget(double x) { palletTarget = std::max(0.0, std::min(1.0, x)); keyDown = palletTarget > 0.0; }
    void setPallet(double openSeconds, double closeSeconds) { tOpen = openSeconds; tClose = closeSeconds; }
    bool idle() const { return palletTarget <= 0 && palletPhase <= 0 && footP < 1e-3 && std::fabs(vPrev) < 1e-7 && std::fabs(qMouthPrev) < 1e-12; }

    // Renders n samples of the two radiated pressures (Pa at 1 m): mouth and top.
    void render(float* mouth, float* top, int count) {
        for (int k = 0; k < count; ++k) {
            double m = 0, t = 0;
            for (int j = 0; j < os; ++j) { double a, b; tick(a, b); m += a; t += b; }
            mouth[k] = (float)(m / os); top[k] = (float)(t / os);
        }
    }

    inline void tick(double& mouthOut, double& topOut) {
        wind();
        const double Uj = Uslot;
        // volume injection
        const double qj = sp.h * sp.Wm * Uj;
        const double dpVol = rho * deltaOut / Sm * (qj - qjPrev) * fs;
        qjPrev = qj;
        hist[pos] = (float)vPrev;
        double qin = 0, t = 0;
        if (Uj > 1e-3) {
            if (Uj != ucache) {   // coefficients that follow the jet speed
                ucache = Uj;
                Vj = sp.lipSpeed * Uj;
                tauS = transitS / Uj;
                const double wc = sp.lpk * Vj / sp.b;
                lpc = 1.0 - std::exp(-wc * dt);
                ghU = sp.sign * Gh / Uj;
                invB = 1.0 / sp.b;
                const double Re = Uj * sp.h / 1.5e-5;
                reGate = std::min(1.0, std::max(0.0, (Re - 600.0) / 200.0));
                lowRe = Re < 3000.0;
                dyn = sp.noise * 0.5 * rho * Vj * Vj * nnorm;
            }
            double tau = std::min((double)kHist - 4.0, std::max(1.0, tauS * fs - 1.0));
            tau = std::min(tau, tauPrev + 1.0);
            tauPrev = tau;
            const double p = pos - tau;
            const int i0 = (int)std::floor(p);
            const double fr = p - i0;
            const double va = hist[i0 & (kHist - 1)], vb = hist[(i0 + 1) & (kHist - 1)];
            double eta = ghU * (va + (vb - va) * fr);
            eta = std::max(-(double)sp.W, std::min((double)sp.W, eta));
            etaLP += lpc * (eta - etaLP);
            t = fastTanh((etaLP - sp.y0b * sp.b) * invB);
            qin = sp.Wm * sp.b * Vj * (1.0 + t);
        }
        {
            const double y = lp.b0 * qin + lp.z1;
            lp.z1 = lp.b1 * qin - lp.a1 * y + lp.z2;
            lp.z2 = lp.b2 * qin - lp.a2 * y;
            qin = y;
        }
        const double dQin = (qin - qinPrev) * fs;
        qinPrev = qin;
        const double v = vPrev;
        double dpNoise = 0;
        if (sp.noise > 0 && Uj > 0.5) {
            double gate = reGate;
            if (lowRe) gate *= 1.0 - t * t;     // sech^2 of the jet position
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            const double xw = ((double)(rng >> 11) * (1.0 / 9007199254740992.0) - 0.5) * 3.4641016151377544;
            const double y = nb0 * xw + nz1;
            nz1 = -na1 * y + nz2;
            nz2 = nb2 * xw - na2 * y;
            dpNoise = dyn * gate * y;
        }
        const double dp = dpNoise + driveK * dQin - vortexK * v * std::fabs(v) + dpVol;
        double q = 0, sTop = 0;
        for (int i = 0; i < nPad; ++i) {
            const double y = b0[i] * dp + z1[i];
            z1[i] = b1[i] * dp - a1[i] * y + z2[i];
            z2[i] = b2[i] * dp - a2[i] * y;
            q += y;
            sTop += cA[i] * y - cI[i] * yPrev[i];
            yPrev[i] = y;
        }
        vPrev = q / Sm;
        pos = (pos + 1) & (kHist - 1);
        constexpr double kRad = kRho / (4 * M_PI);
        mouthOut = -kRad * (q - qMouthPrev) * fs;
        topOut = kRad * (sTop - sTopPrev) * fs;
        qMouthPrev = q; sTopPrev = sTop;
    }

    // live state for the engine
    double footPressure() const { return footP; }
    double jetSpeedNow() const { return Uslot; }

private:
    static constexpr int kHist = 4096;
    static constexpr double kRho = 1.2929 * 273.15 / 293.15;
    int os = 1;
    FluePipeSpec sp;
    double fs = 48000, dt = 1.0 / 48000, rho = 1.2, c2rho = 1.4e5, mu = 1.81e-5;
    double vortexK = 0, toeArea = 0, windR = 0, lc = 0.002, volume = 1e-4, Sm = 1e-4, deltaOut = 0, ddE = 0, driveK = 0, Gh = 0;
    double pitch = 0, transitS = 0;
    // wind path
    bool keyDown = false;
    double chest = 775.0, tOpen = 0.012, tClose = 0.012, palletTarget = 0;
    double footP = 0, palletPhase = 0, palletPos = 0, Uslot = 0;
    int steadyCount = 0;
    static constexpr double kW = 0.150, kGMax = 0.008, kSeal = 0.010, kCdd = 0.61;
    // jet
    double qjPrev = 0, qinPrev = 0, vPrev = 0, etaLP = 0, tauPrev = 1e9;
    double ucache = -1, invB = 1, Vj = 0, tauS = 0, lpc = 0, ghU = 0, reGate = 0, dyn = 0; bool lowRe = false;
    struct LP { double b0, b1, b2, a1, a2, z1, z2; } lp{};
    double nb0 = 0, nb2 = 0, na1 = 0, na2 = 0, nnorm = 0, nz1 = 0, nz2 = 0;
    uint64_t rng = 1;
    float hist[kHist];
    int pos = 0, n = 0, nPad = 0;
    alignas(32) double b0[kRtMaxModes], b1[kRtMaxModes], b2[kRtMaxModes], a1[kRtMaxModes], a2[kRtMaxModes];
    alignas(32) double z1[kRtMaxModes], z2[kRtMaxModes], cA[kRtMaxModes], cI[kRtMaxModes], yPrev[kRtMaxModes];
    double qMouthPrev = 0, sTopPrev = 0;

    double jetSpeed(double P) const { return P <= 0 ? 0.0 : (std::sqrt(windR * windR + 2.0 * rho * P) - windR) / rho; }

    // Chest -> pallet -> foot volume -> flue, implicit in the foot pressure (as FlueSim::tickKey).
    inline void wind() {
        const bool moving = palletPhase != palletTarget;
        if (moving) {
            palletPhase = palletPhase < palletTarget ? std::min(palletTarget, palletPhase + dt / tOpen)
                                                     : std::max(palletTarget, palletPhase - dt / tClose);
            palletPos = 0.5 - 0.5 * std::cos(M_PI * palletPhase);
            steadyCount = 0;
        }
        const double g = kGMax * palletPos;
        // inflow(dP) = (-bb + sqrt(bb^2 + 4 aa dP)) / (2 aa); outflow(P) = h Wm (sqrt(R^2 + 2 rho P) - R)/rho
        double aa = 0, bb = 0; const bool open = g > 1e-9;
        if (open) {
            const double Ap = kCdd * kW * g;
            aa = rho / (2 * Ap * Ap);
            if (toeArea > 0) aa += rho / (2 * (kCdd * toeArea) * (kCdd * toeArea));
            bb = 12.0 * mu * kSeal / (kW * g * g * g);
        }
        const double k1 = dt * c2rho / volume, P0 = footP, hw = (double)sp.h * sp.Wm;
        auto F = [&](double P, double& dF) {
            double qi = 0, dqi = 0;
            const double dP = chest - P;
            if (open && dP > 0) { const double r = std::sqrt(bb * bb + 4 * aa * dP); qi = (-bb + r) / (2 * aa); dqi = 1.0 / r; }
            double qo = 0, dqo = 0;
            if (P > 0) { const double r = std::sqrt(windR * windR + 2.0 * rho * P); qo = hw * (r - windR) / rho; dqo = hw / r; }
            dF = 1.0 + k1 * (dqi + dqo);
            return P - P0 - k1 * (qi - qo);
        };
        if (steadyCount < 64) {
            double P = P0, dF;
            double r = F(P, dF);
            const double tol = 1e-9 * (chest + 1.0);
            if (std::fabs(r) > tol) {
                for (int it = 0; it < 3; ++it) {
                    P = std::max(0.0, std::min(std::max(chest, P0), P - r / dF));
                    r = F(P, dF);
                    if (std::fabs(r) < tol) break;
                }
                steadyCount = 0;
            } else ++steadyCount;
            footP = P;
            // windway inertia: rho lc dU/dt + rho U^2/2 + R U = P
            const double B = rho * lc * fs + windR, Cq = footP + rho * lc * fs * Uslot;
            const double U = std::max(0.0, (-B + std::sqrt(B * B + 2.0 * rho * std::max(0.0, Cq))) / rho);
            if (std::fabs(U - Uslot) > 1e-12 * (1.0 + U)) steadyCount = 0;
            Uslot = U;
        }
    }
public:
    // the engine changes the chest pressure (wind system): wake the solver
    void windChanged() { steadyCount = 0; }
    void setChestPa(double pa) { if (pa != chest) { chest = pa; steadyCount = 0; } }
};

} // namespace pipe
