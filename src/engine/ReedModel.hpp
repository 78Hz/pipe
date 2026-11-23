// Pipe - physically modelled pipe organ
// Reed pipes of Pasi Opus 14: a beating reed on its resonator.
//
//   Tongue: a brass cantilever (E = 100 GPa, 8500 kg/m3) of the thickness Pasi gives, in its first
//   bending mode; free length set by the tuning wire so that the pipe sounds at pitch (the tuner's
//   job: tools/opus14_organ.cpp). Tip displacement x (+ = open), modal mass 1/4 and modal force
//   0.39 of the uniform pressure load, so  x'' = -(wr/Qr) x' - wr^2 (x - x0) - 1.56 dP / (rho_b t).
//   It beats on the shallot face (x < 0: a stiff, fully damped contact).
//   Flow: through the slit by Bernoulli, U = w x+ sqrt(2|dP|/rho) sgn(dP), plus the air the tongue
//   moves, 0.39 w L x'. dP = boot pressure - pressure at the shallot.
//   Resonator: the input impedance of the Pasi resonator (cones and cylinders, lossy, open top)
//   as a sum of modes, p = sum Z_n(s) U; radiated from the top, p(1 m) = rho/(4 pi) dU_top/dt.
//   Wind: chest -> pallet -> boot (one volume) as for the flue pipes.
// Constants not on the Pasi sheets (tongue width / resonator lower diameter, rest opening / free
// length, tongue damping, boot volume) are matched to reference recordings per family.
#pragma once
#include "Air.hpp"

#include "Opus14Data.hpp"
#include "Resonator.hpp"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>

namespace pipe {

constexpr int kReedMaxModes = 32;

struct ReedPipeSpec {
    float target = 0;              // Hz
    float footPa = 0;              // voiced boot pressure
    float t = 0, L = 0, w = 0, x0 = 0, Qr = 30;   // tongue: thickness, free length, width, rest opening, damping
    float xs = 0;                  // closure: the curved tongue rolls onto the shallot over about this tip travel
    float bootVolume = 0;          // m^3
    float lowerR = 0, upperR = 0, length = 0;   // resonator
    float fr = 0;                  // tongue frequency as tuned (Hz)
    float trim = 1;                // final tuning: tongue and resonator scaled by this frequency ratio
    float shallotL = 0;            // shallot: acoustic length under the tongue incl. its end correction (m), 0 = none
    float channel = 0;             // reed channel: length of the gap the slit flow runs through (m), 0 = quasi-steady
    float hfFrom = 0;              // above the last mode the resonator is a horn: from this frequency (Hz)
    int nModes = 0;
    float f[kReedMaxModes], Q[kReedMaxModes], pkRe[kReedMaxModes], pkIm[kReedMaxModes], topRe[kReedMaxModes], topIm[kReedMaxModes];
    float passR = -1;              // the fitted modes' most negative Re Z (reedPassiveR), for the record; not used in sound
    // The toe hole the boot is fed through from the groove, as the voicer bored it to regulate the pipe
    // (m, diameter). 0 = open: the boot takes the groove's wind through its own bore. (Last in the record:
    // a record written before it was added is read with 0.)
    float toe = 0;
};

// The resonator's input impedance as the sum of its modes, each fitted to the complex peak of the
// bore's impedance, is not passive: away from the peaks the complex residues give Re Z < 0 (for the
// Trumpet 8' C, -1.2e5 Pa s/m^3 at 53 Hz). A real bore's impedance is positive real. Fed by the reed,
// the negative part was a source of energy: with the pallet shut the pipe went on sounding at
// -16 dB below its speech, at the frequency where Re Z was lowest. Each mode is therefore made
// passive on its own (ReedPipeRT::setPitch: the imaginary part of its peak limited to -pkRe/Q .. 0),
// so their sum is. (A single series resistance large enough to cover the worst frequency was tried
// first, 2026-11-17: for treble pipes it was 2.6e6 Pa s/m^3, hundreds of Pa of back-pressure, and 67
// reeds could no longer be voiced.) reedPassiveR: the most negative Re Z of the fitted modes, for the record.
inline double reedPassiveR(const ReedPipeSpec& s) {
    double mn = 0;
    for (double fr = 1.0; fr < 20000.0; fr *= 1.001) {
        const double w = 2 * M_PI * fr;
        double re = 0;
        for (int i = 0; i < s.nModes; ++i) {
            const double wn = 2 * M_PI * s.f[i], Q = s.Q[i], A = s.pkRe[i] * wn / Q, B = -s.pkIm[i] * wn * wn / Q;
            const double dr = wn * wn - w * w, di = w * wn / Q;
            re += (B * dr + A * w * w * wn / Q) / (dr * dr + di * di);
        }
        mn = std::min(mn, re);
    }
    return -mn;
}

class ReedPipeRT {
public:
    void init(const ReedPipeSpec& s, double sampleRate, uint32_t seed) {
        (void)seed;
        sp = s;
        os = std::max(2, (int)std::ceil(s.target * 40.0 / sampleRate));
        fs = sampleRate * os; dt = 1.0 / fs;
        wr = 2 * M_PI * (s.fr > 0 ? s.fr : 0.1615 * (s.t / ((double)s.L * s.L)) * std::sqrt(kE / kRhoB));
        // horn above the modes: pressure rho c / S_in on the reed, flow to the top sqrt(S_top/S_in), delayed L/c
        // (hfFrom = 0: no horn path. Fed back onto the tongue as rho c / S it made the reed unstable,
        // the result depending on the sample rate; the modes carry the sound instead.)
        topGain = s.hfFrom > 0 ? s.upperR / std::max(1e-4f, s.lowerR) : 0.0;
        { const double w0 = 2 * M_PI * std::max(200.f, s.hfFrom) / fs, K = std::tan(w0 / 2), q = 0.7071, n = 1.0 / (1 + K / q + K * K);
          hb0 = n; hb1 = -2 * n; hb2 = n; ha1 = 2 * (K * K - 1) * n; ha2 = (1 - K / q + K * K) * n; }
        accK = 1.56 / (kRhoB * s.t);
        Sr = 0.39 * (double)s.w * s.L;
        // shallot inertance in series with the resonator (Rucz 2015 sec. 8.4): rho l / S of the shallot bore
        applyAir();
        pitch = 0; setPitch(1.0);
        reset();
    }
    void reset() {
        inAcc = gAcc = qiLast = dqLast = 0;
        x = sp.x0; vx = 0; Us = 0; boot = 0; palletPhase = 0; palletPos = 0; palletTarget = 0; U = 0; p = 0; topPrev = 0;
        std::memset(z1, 0, sizeof z1); std::memset(z2, 0, sizeof z2); std::memset(yPrev, 0, sizeof yPrev);
        hz1 = hz2 = 0; std::memset(horn, 0, sizeof horn); hpos = 0; uhPrev = 0;
    }
    // The air (Air.hpp), relative to the air the pipe was voiced in: its density sets the flow through the
    // slit and the shallot's inertia, its speed of sound the resonator's modes and the horn's delay, rho c the
    // resonator's impedance. The tongue is brass and does not follow the air's sound speed; it is as warm as the air
    // though, and warm brass is softer (PipeAir::tongueRatio).
    void setAir(const PipeAir& a) {
        if (a == air) return;
        air = a; applyAir();
        const double p0 = pitch; pitch = 0; setPitch(p0 > 0 ? p0 : 1.0);
    }
    void applyAir() {
        rho = kRho * air.rhoRatio; const double c = 343.21 * air.cRatio; c2rho = rho * c * c;
        // horn above the modes: pressure rho c / S_in on the reed, flow to the top sqrt(S_top/S_in), delayed L/c
        Zc = sp.hfFrom > 0 ? rho * c / (M_PI * (double)sp.lowerR * sp.lowerR) : 0.0;
        hornDelay = std::min(kHorn - 2, std::max(1, (int)std::lround(sp.length / c * fs)));
        // shallot inertance in series with the resonator (Rucz 2015 sec. 8.4): rho l / S of the shallot bore
        Ls = sp.shallotL > 0 ? rho * sp.shallotL / (M_PI * (double)sp.lowerR * sp.lowerR) : 0.0;
    }
    // a frequency ratio scales the tongue and the resonator together (tuning, bends)
    void setPitch(double ratioIn) {
        if (ratioIn == pitch) return;
        pitch = ratioIn;
        const double ratio = ratioIn * (sp.trim > 0 ? sp.trim : 1.f);
        wrNow = wr * ratio * air.tongueRatio;
        hornDelayScaled = hornDelay;
        nPad = std::min(kReedMaxModes, (sp.nModes + 3) & ~3);
        for (int i = 0; i < kReedMaxModes; ++i) {
            const double wn = 2 * M_PI * sp.f[i] * ratio * air.cRatio;
            if (i >= sp.nModes || wn >= M_PI * fs * 0.98) { b0[i] = b1[i] = b2[i] = a1[i] = a2c[i] = cA[i] = cI[i] = 0; continue; }
            const double Q = sp.Q[i];
            // each mode passive on its own (reedPassiveR): (A s + B) / (s^2 + s wn/Q + wn^2) is positive real
            // when 0 <= B <= A wn / Q, i.e. -pkRe/Q <= pkIm <= 0
            const double im = std::min(0.0, std::max(-(double)sp.pkRe[i] / Q, (double)sp.pkIm[i]));
            const double A = air.zRatio() * sp.pkRe[i] * wn / Q, B = -air.zRatio() * im * wn * wn / Q;
            const double K = wn / std::tan(wn * dt / 2), a0 = K * K + K * wn / Q + wn * wn;
            b0[i] = (A * K + B) / a0; b1[i] = 2 * B / a0; b2[i] = (-A * K + B) / a0;
            a1[i] = 2 * (wn * wn - K * K) / a0; a2c[i] = (K * K - K * wn / Q + wn * wn) / a0;
            cI[i] = sp.topIm[i] * fs / wn; cA[i] = sp.topRe[i] + cI[i];
        }
    }
    void key(bool down) { palletTarget = down ? 1.0 : 0.0; }
    void setPalletTarget(double v) { palletTarget = std::max(0.0, std::min(1.0, v)); }
    void setPallet(double o, double c) { tOpen = o; tClose = c; }
    void setChestPa(double pa) { chest = pa; }
    double bootPressure() const { return boot; }

    void render(float* out, int count) {
        inAcc = gAcc = 0;
        for (int k = 0; k < count; ++k) {
            double acc = 0;
            for (int j = 0; j < os; ++j) acc += tick();
            out[k] = (float)(acc / os);
        }
        inAcc /= (double)(os * count); gAcc /= (double)(os * count);
    }
    // the wind drawn into the boot over the last render, and its conductance (as FluePipeRT::inflow)
    double inflow() const { return inAcc; }
    double inflowConductance() const { return gAcc; }
    // What the boot will draw over its next step with the groove at chestPa, from its state now, and how that
    // moves with chestPa (FluePipeRT::previewInflow): the groove is solved with it before the pipe takes its wind.
    double previewInflow(double chestPa, double& gOut) const {
        // over the engine's sample: the os steps of the reed's own rate, the boot carried from one to the next,
        // the tongue's flow as it is now
        double phase = palletPhase, P0 = boot, qSum = 0, gSum = 0;
        for (int j = 0; j < os; ++j) {
            if (phase != palletTarget) phase = phase < palletTarget ? std::min(palletTarget, phase + dt / tOpen) : std::max(palletTarget, phase - dt / tClose);
            const double pos = phase != palletPhase || palletPhase != palletTarget ? 0.5 - 0.5 * std::cos(M_PI * phase) : palletPos;
            const double g = 0.008 * pos;
            const double k1 = dt * c2rho / sp.bootVolume;
            if (g <= 1e-9) { P0 = std::max(0.0, P0 - k1 * U); continue; }
            const double Ap = toeD > 0 ? 0.61 * M_PI * 0.25 * toeD * toeD * pos : 0.61 * 0.150 * g;
            const double aa = rho / (2 * Ap * Ap);
            const double bb = toeD > 0 ? 12.0 * 1.807e-5 * 0.005 / (toeD * toeD * toeD * std::max(1e-3, pos)) : 12.0 * 1.807e-5 * 0.010 / (0.150 * g * g * g);
            auto inflowAt = [&](double Pb_, double& dq_) { const double dP = chestPa - Pb_, r = std::sqrt(bb * bb + 4 * aa * std::fabs(dP)); dq_ = 1.0 / r; return (dP >= 0 ? 1.0 : -1.0) * (-bb + r) / (2 * aa); };
            double P = P0, dq = 0;
            for (int it = 0; it < 3; ++it) { const double qi = inflowAt(P, dq); const double F = P - P0 - k1 * (qi - U), dF = 1.0 + k1 * dq; P = std::max(0.0, std::min(std::max(chestPa, P0), P - F / dF)); }
            const double q = inflowAt(P, dq);
            qSum += q; gSum += dq / (1.0 + k1 * dq); P0 = P;
        }
        gOut = gSum / os;
        return qSum / os;
    }
    // Stood on a groove (the slider chest): its boot is fed through a toe hole of this diameter (m); 0: its own pallet.
    void setToe(double d) { toeD = d; }

    inline double tick() {
        // pallet and boot (inflow implicit, outflow = reed flow of the last step)
        if (palletPhase != palletTarget) {
            palletPhase = palletPhase < palletTarget ? std::min(palletTarget, palletPhase + dt / tOpen)
                                                     : std::max(palletTarget, palletPhase - dt / tClose);
            palletPos = 0.5 - 0.5 * std::cos(M_PI * palletPhase);
        }
        // The boot's outflow is this sample's reed flow, so the boot is solved with the flow below
        // (as the resonator is): boot = Pb - kb U. Pb is the boot with the pallet's inflow and none
        // out, kb its compliance relieved by the pallet's conductance. Updated from the flow of the
        // last sample, the closed boot and the tongue formed a stiff loop one sample late, which fed
        // energy in: shut off, a reed went on sounding at -16 dB.
        double Pb, kb;
        {
            const double g = 0.008 * palletPos;
            const double k1 = dt * c2rho / sp.bootVolume, P0 = boot;
            double P = P0, dq = 0;
            if (g > 1e-9) {
                // on a groove the boot is fed through its toe hole (a hole of toeD, 5 mm deep); on its own chest through
                // its own pallet (150 x 8 mm)
                const double Ap = toeD > 0 ? 0.61 * M_PI * 0.25 * toeD * toeD * palletPos : 0.61 * 0.150 * g;
                const double aa = rho / (2 * Ap * Ap);
                const double bb = toeD > 0 ? 12.0 * 1.807e-5 * 0.005 / (toeD * toeD * toeD * std::max(1e-3, palletPos)) : 12.0 * 1.807e-5 * 0.010 / (0.150 * g * g * g);
                // both ways: a boot left above a falling groove blows back through its toe
                auto inflowAt = [&](double Pb_, double& dq_) { const double dP = chest - Pb_, r = std::sqrt(bb * bb + 4 * aa * std::fabs(dP)); dq_ = 1.0 / r; return (dP >= 0 ? 1.0 : -1.0) * (-bb + r) / (2 * aa); };
                for (int it = 0; it < 3; ++it) {
                    const double qi = inflowAt(P, dq);
                    const double F = P - P0 - k1 * (qi - U), dF = 1.0 + k1 * dq;
                    P = std::max(0.0, std::min(std::max(chest, P0), P - F / dF));
                }
                qiLast = inflowAt(P, dq); dqLast = dq / (1.0 + k1 * dq);   // the toe in series with the boot, as the groove feels it (FlueRT)
            } else { P = P0 - k1 * U; qiLast = 0; dqLast = 0; }
            inAcc += qiLast; gAcc += dqLast;
            kb = k1 / (1.0 + k1 * dq);
            Pb = P + kb * U;   // the outflow taken back out; it is put in again at this sample's flow
        }
        // tongue (velocity implicit in the stiff terms)
        const double dP = boot - p;
        const double force = -accK * dP + wrNow * wrNow * sp.x0;
        // on the shallot face (x < 0) a stiff, damped contact. The step is split at the instant the tongue
        // meets or leaves the face, so the impact is timed within the sample: switched at whole samples
        // the period of a treble reed (40 - 60 samples) locked to whole samples, its pitch in steps of
        // 20 - 40 c, and it jumped between regimes for small changes of tuning.
        auto step = [&](double h, bool contact, double& xx, double& vv) {
            double K = wrNow * wrNow, C = wrNow / sp.Qr;
            if (contact) { const double wc = 20.0 * wrNow; K += wc * wc; C += 2.0 * wc; }
            // trapezoidal (implicit midpoint): no numerical damping. The implicit-velocity step used before
            // damped the tongue by about omega dt / 2 (a Q of 7 at 1.1 kHz at 48 kHz, far below the tongue's
            // own 61): above about 600 Hz the reeds could not sound at all (2026-11-17: Trumpet 8' c#3 is
            // silent at 48 and 96 kHz and speaks in tune at 384 kHz with the old step).
            const double a = h * h * K / 4 + h * C / 2;
            const double v1 = (vv * (1.0 - a) + h * (force - K * xx)) / (1.0 + a);
            xx += 0.5 * h * (vv + v1);
            vv = v1;
        };
        {
            const double x0s = x, v0s = vx;
            const bool c0 = x0s < 0;
            step(dt, c0, x, vx);
            if ((x < 0) != c0 && x0s != x) {
                const double th = std::max(0.0, std::min(1.0, x0s / (x0s - x)));
                double xx = x0s, vv = v0s;
                step(th * dt, c0, xx, vv);
                step((1.0 - th) * dt, !c0, xx, vv);
                x = xx; vx = vv;
            }
        }
        // Flow and pressure solved together (Guillemain et al. 2005): the resonator answers the flow
        // of this very sample through its instantaneous part, p = Pz + Zd U, so with Bernoulli in the
        // slit U - Ur = A sgn(D) sqrt|D|, D = boot - p, the flow has a closed form.
        // the curved tongue rolls onto the shallot face from its root: the slit closes progressively
        // over a tip travel of about xs (softplus), not at a corner
        double open;
        if (sp.xs > 0) { const double z = x / sp.xs; open = z > 30 ? x : sp.xs * std::log1p(std::exp(z)); }
        else open = x > 0 ? x : 0.0;
        double Pz = Zc * hz1, Zd = Zc * hb0;
        // the shallot: p at the tongue = p in the resonator + Ls dU/dt (backward difference, part of the solve)
        Pz -= Ls * fs * U; Zd += Ls * fs;
        for (int i = 0; i < nPad; ++i) { Pz += z1[i]; Zd += b0[i]; }
        const double Ur = -Sr * vx;   // the tongue closing (vx < 0) pushes air from the boot into the shallot
        const double A = sp.w * open * std::sqrt(2.0 / rho);
        Zd += kb;   // the boot gives way to the flow as the resonator does
        const double D0 = Pb - Pz - Zd * Ur;
        const double a2 = A * A, q = a2 * Zd;
        double X;
        if (sp.channel > 0) {
            // with the inertia of the air in the reed channel (length l under the tongue):
            // rho l / A dUs/dt = D - rho/2 Us|Us| / A^2, implicit in Us (A = w x, the slit)
            const double Aop = sp.w * open;
            if (Aop < 1e-12) X = 0.0;
            else {
                const double g = Aop / (rho * sp.channel);
                X = (Us + dt * g * D0) / (1.0 + dt * g * Zd + dt * 0.5 * std::fabs(Us) / (sp.channel * Aop));
            }
            Us = X;
        } else
            X = D0 >= 0 ? 0.5 * (-q + std::sqrt(q * q + 4 * a2 * D0)) : -0.5 * (-q + std::sqrt(q * q - 4 * a2 * D0));
        const double Uold = U;
        U = Ur + X;
        boot = Pb - kb * U;
        // resonator
        double pp = 0, top = 0;
        for (int i = 0; i < nPad; ++i) {
            const double y = b0[i] * U + z1[i];
            z1[i] = b1[i] * U - a1[i] * y + z2[i];
            z2[i] = b2[i] * U - a2c[i] * y;
            pp += y;
            top += cA[i] * y - cI[i] * yPrev[i];
            yPrev[i] = y;
        }
        // horn path above the modes (2nd-order high-pass of the flow)
        const double uh = hb0 * U + hz1;
        hz1 = hb1 * U - ha1 * uh + hz2;
        hz2 = hb2 * U - ha2 * uh;
        pp += Zc * uh;
        horn[hpos] = uh;
        top += topGain * horn[(hpos - hornDelay) & (kHorn - 1)];
        hpos = (hpos + 1) & (kHorn - 1);
        p = pp + Ls * fs * (U - Uold);   // pressure under the tongue (resonator + shallot)
        const double out = kRho / (4 * M_PI) * (top - topPrev) * fs;
        topPrev = top;
        return out;
    }

private:
    static constexpr double kRho = 1.2929 * 273.15 / 293.15, kE = 1.0e11, kRhoB = 8500.0;
    ReedPipeSpec sp;
    int os = 2, nPad = 0, hornDelayScaled = 1;
    double fs = 96000, dt = 1.0 / 96000, rho = 1.2, c2rho = 1.4e5;
    PipeAir air;
    double inAcc = 0, gAcc = 0, qiLast = 0, dqLast = 0, toeD = 0;
    double wr = 0, wrNow = 0, accK = 0, Sr = 0, pitch = 0, Ls = 0, Us = 0;
    double x = 0, vx = 0, boot = 0, U = 0, p = 0, topPrev = 0;
    static constexpr int kHorn = 8192;
    double Zc = 0, topGain = 1, hb0 = 0, hb1 = 0, hb2 = 0, ha1 = 0, ha2 = 0, hz1 = 0, hz2 = 0, uhPrev = 0;
    int hornDelay = 1, hpos = 0;
    float horn[kHorn];
    double chest = 775, palletPhase = 0, palletPos = 0, palletTarget = 0, tOpen = 0.012, tClose = 0.012;
    alignas(32) double b0[kReedMaxModes], b1[kReedMaxModes], b2[kReedMaxModes], a1[kReedMaxModes], a2c[kReedMaxModes];
    alignas(32) double z1[kReedMaxModes], z2[kReedMaxModes], cA[kReedMaxModes], cI[kReedMaxModes], yPrev[kReedMaxModes];
};

// ------------------------------------------------------------------ design
namespace reed {
using namespace resonator;

// gamma: the voicer's curve of the tongue, as the ratio of the wind pressure to the pressure that
// would press the tongue shut, p / p_M with p_M = k_tongue x0 / force per pascal (Kergomard's gamma).
// A voicer curves each tongue to speak well at the wind it gets, so this ratio, not the rest opening
// over the free length, is what holds across the compass: with a fixed x0/L the bass tongues were
// pressed shut (p/p_M up to 2.9) and the treble ones could not start (down to 0.06).
struct ReedConstants { double widthRatio = 0.6, gamma = 0.55, Qr = 30, bootLitres8 = 0.5, closure = 0.2, shallot = 2.0, channel = 0.0; bool shallotTube = false; double shallotR = 1.0; double zetaMin = 0.0; };

inline const opus14::Row* row(const opus14::Stop& s, const char* label) {
    for (int i = 0; i < s.nRows; ++i) if (!std::strcmp(s.rows[i].label, label)) return &s.rows[i];
    return nullptr;
}
// Wooden reed resonators are printed "width x depth": the second value is the depth.
inline bool woodReed(const opus14::Stop& s) {
    return s.material && (std::strstr(s.material, "Poplar") || std::strstr(s.material, "Pine") || std::strstr(s.material, "Fir") || std::strstr(s.material, "ood"));
}
// A printed row at a key, interpolated geometrically between the printed keys.
// On a metal stop a second value at a key is where a new series begins (Bassoon and Dulzian: the
// full-length resonators from c0 on, half length below; Oboe: the belled stem from c1 on): from that key
// up the second value is the point at that key, below it the first. On a wooden stop alt = the depth.
inline double at(const opus14::Stop& s, const char* label, double key, bool alt = false) {
    const opus14::Row* r = row(s, label);
    if (!r) return NAN;
    const bool series = !woodReed(s) && !alt;
    double vals[7];
    for (int i = 0; i < s.nCols; ++i) {
        vals[i] = alt ? r->alt[i] : r->v[i];
        if (series && !std::isnan(r->alt[i]) && key >= s.key[i]) vals[i] = r->alt[i];
    }
    if (!alt && !woodReed(s)) {   // the second series starts at its key: no interpolation across the break
        for (int i = 0; i < s.nCols; ++i) if (!std::isnan(r->alt[i]) && key < s.key[i]) {
            // below the break: points from the break up are the new series, so they are not used
            for (int j = i + 1; j < s.nCols; ++j) vals[j] = NAN;
            break;
        }
    }
    int idx[7], n = 0;
    for (int i = 0; i < s.nCols; ++i) if (!std::isnan(vals[i]) && vals[i] > 0) idx[n++] = i;
    if (n == 0) return NAN;
    if (n == 1) return vals[idx[0]];
    int j = 0; while (j + 2 < n && s.key[idx[j + 1]] < key) ++j;
    const int a = idx[j], b = idx[j + 1];
    const double t = (key - s.key[a]) / (double)(s.key[b] - s.key[a]);
    return vals[a] * std::pow(vals[b] / vals[a], t);
}
// radius from a printed diameter (round), or a wooden square "a x b" (alt = depth)
inline double radiusAt(const opus14::Stop& s, const char* label, double key) {
    const double a = at(s, label, key);
    if (std::isnan(a)) return NAN;
    const double b = woodReed(s) ? at(s, label, key, true) : NAN;
    return std::isnan(b) ? 0.5e-3 * a : eqRadius(a * b * 1e-6);
}

// The resonator from the Pasi rows (from the shallot upwards).
inline std::vector<Segment> resonatorOf(const opus14::Stop& s, double key) {
    std::vector<Segment> g;
    if (row(s, "Diameter Bottom")) {   // Vox Humana: a double cone
        const double rb = radiusAt(s, "Diameter Bottom", key), rm = radiusAt(s, "Diameter Middle", key), rt = radiusAt(s, "Diameter Top", key);
        g.push_back({ rb, rm, 1e-3 * at(s, "Length Lower", key) });
        g.push_back({ rm, rt, 1e-3 * at(s, "Length Upper", key) });
    } else if (row(s, "Cylinder Length")) {   // Dulzian: a cone then a cylinder
        const double rl = radiusAt(s, "Lower Diameter", key), ru = radiusAt(s, "Upper Diameter", key);
        g.push_back({ rl, ru, 1e-3 * at(s, "Cone Length", key) });
        g.push_back({ ru, ru, 1e-3 * at(s, "Cylinder Length", key) });
    } else if (row(s, "Stem Length")) {   // Trichterregal (stem + funnel), Oboe (stem + bell)
        const double rl = radiusAt(s, "Stem Lower Diameter", key), ru = radiusAt(s, "Stem Upper Diameter", key);
        g.push_back({ rl, ru, 1e-3 * at(s, "Stem Length", key) });
        const char* cu = row(s, "Cone Upper Diameter") ? "Cone Upper Diameter" : "Bell Upper Diameter";
        const char* cl = row(s, "Cone Length") ? "Cone Length" : "Bell Cone Length";
        const double rc = radiusAt(s, cu, key), lc = at(s, cl, key);
        // the bell from the first key it is printed at (the Oboe's from c1, with the belled stem)
        bool printed = false; const opus14::Row* r = row(s, cl);
        for (int i = 0; r && i < s.nCols; ++i) if (s.key[i] <= key && !std::isnan(r->v[i])) printed = true;
        if (printed && !std::isnan(rc) && !std::isnan(lc)) g.push_back({ ru, rc, 1e-3 * lc });
    } else {   // trumpets, posaunes, clairons, cornet: one cone
        g.push_back({ radiusAt(s, "Lower Diameter", key), radiusAt(s, "Upper Diameter", key), 1e-3 * at(s, "Length", key) });
    }
    return g;
}

inline cd inputImpedance(double w, const std::vector<Segment>& g, const Air& air) {
    return bodyImpedance(w, g, Top::Open, 0, 0, {}, air);
}
// flow at the top over flow into the resonator
inline cd topFlowRatio(double w, const std::vector<Segment>& g, const Air& air) {
    const cd ZL = openEnd(w, g.back().r2, air);
    Mat m{ 1, 0, 0, 1 };
    for (const auto& s : g) m = m * (std::fabs(s.r2 - s.r1) < 1e-9 ? cylinder(w, s.r1, s.L, air) : cone(w, s.r1, s.r2, s.L, air));
    return 1.0 / (m.c * ZL + m.d);
}

// Tongue free length for a reed frequency fr (cantilever, first mode).
inline double tongueLength(double t, double fr) { return std::sqrt(0.1615 * t * std::sqrt(1.0e11 / 8500.0) / fr); }

// rest opening for a curve gamma = p_wind / p_M of a tongue at frequency fr (modal force 1.56 dP / (rho_b t))
inline double restOpening(const ReedPipeSpec& r, double fr, double gamma) {
    const double wr = 2 * M_PI * fr;
    return (double)r.footPa * 1.56 / (8500.0 * (double)r.t) / (gamma * wr * wr);
}

// zeta = Zc w x0 sqrt(2 / (rho p_M)) (Kergomard): the slit against the resonator's throat, the reed's
// strength over its pipe. With the tongue width a fixed fraction of the resonator's lower diameter it fell
// a hundredfold up the compass (0.07 at the trumpets' c4), and the treble could not speak at its pitch,
// though the real pipes do. The tongue width is not on the Pasi sheets: where zeta would fall below
// zetaMin (the lowest value at the keys the constants were matched on) the tongue is made that much wider.
inline void applyZeta(ReedPipeSpec& r, const ReedConstants& k, double gamma) {
    r.w = (float)(k.widthRatio * 2.0 * r.lowerR);
    if (!(k.zetaMin > 0)) return;
    const double rho = 1.2929 * 273.15 / 293.15, Zc = rho * 343.21 / (M_PI * (double)r.lowerR * r.lowerR);
    const double pM = (double)r.footPa / gamma, zeta = Zc * r.w * r.x0 * std::sqrt(2.0 / (rho * pM));
    if (zeta < k.zetaMin) r.w = (float)(r.w * k.zetaMin / zeta);
}
inline double zetaOf(const ReedPipeSpec& r, double gamma) {
    const double rho = 1.2929 * 273.15 / 293.15, Zc = rho * 343.21 / (M_PI * (double)r.lowerR * r.lowerR);
    return Zc * r.w * r.x0 * std::sqrt(2.0 / (rho * (double)r.footPa / gamma));
}

inline ReedPipeSpec design(const opus14::Stop& s, double key, double targetHz, const ReedConstants& k, double fs = 48000.0) {
    ReedPipeSpec r;
    Air air; air.tempC = 20.0;
    r.target = (float)targetHz;
    r.footPa = (float)(79.0 * 9.80665);
    std::vector<Segment> g = resonatorOf(s, key);
    r.lowerR = (float)g.front().r1; r.upperR = (float)g.back().r2;
    double len = 0; for (auto& x : g) len += x.L; r.length = (float)len;
    double t = at(s, "Tongue Thickness", key);
    if (std::isnan(t)) {   // the treble is not printed: continue the last octave's ratio
        const double t1 = at(s, "Tongue Thickness", key - 12), t2 = at(s, "Tongue Thickness", key - 24);
        t = (!std::isnan(t1) && !std::isnan(t2)) ? t1 * t1 / t2 : 0.1;
    }
    r.t = (float)(1e-3 * t);
    r.L = (float)tongueLength(r.t, targetHz);
    r.w = (float)(k.widthRatio * 2.0 * g.front().r1);
    r.shallotL = (float)(k.shallot * r.L);
    if (k.shallotTube) {
        // the shallot as a tube of the resonator's lower bore between the tongue and the resonator: its
        // air is both mass and volume (the lumped inertance kept only the mass)
        g.insert(g.begin(), Segment{ k.shallotR * g.front().r1, k.shallotR * g.front().r1, (double)r.shallotL });
        r.shallotL = 0.f;
    }
    r.channel = (float)k.channel;
    r.Qr = (float)k.Qr;
    r.bootVolume = (float)(1e-3 * k.bootLitres8 * std::sqrt(std::max(0.05, 8.0 * 65.4 / targetHz / 8.0)));
    const double fTop = std::min(8000.0, 0.45 * fs);
    auto modes = findModes([&](double w) { return inputImpedance(w, g, air); }, std::max(20.0, targetHz * 0.3), std::max(fTop, 3.0 * targetHz), kReedMaxModes, 6000);
    size_t n = 0; while (n < modes.size() && modes[n].Q >= 2.0) ++n;
    r.nModes = (int)std::min<size_t>(n, kReedMaxModes);
    r.hfFrom = 0.f;   // no horn path (see ReedPipeRT::init)
    r.fr = (float)(1.05 * targetHz);
    r.x0 = (float)restOpening(r, r.fr, k.gamma);
    r.lowerR = (float)g.front().r1;
    applyZeta(r, k, k.gamma);
    r.xs = (float)(k.closure * r.x0);
    for (int i = 0; i < r.nModes; ++i) {
        r.f[i] = (float)modes[i].f; r.Q[i] = (float)modes[i].Q;
        r.pkRe[i] = (float)modes[i].peak.real(); r.pkIm[i] = (float)modes[i].peak.imag();
        // top flow per unit pressure at the shallot, at the mode: (U_top/U_in) / Z_in
        const cd T = topFlowRatio(2 * M_PI * modes[i].f, g, air) / modes[i].peak;
        r.topRe[i] = (float)T.real(); r.topIm[i] = (float)T.imag();
    }
    r.passR = (float)reedPassiveR(r);
    return r;
}
} // namespace reed
} // namespace pipe

namespace pipe { namespace reed {
// Tunes a reed pipe as a voicer does: the tuning wire (tongue frequency) first; if the tongue cannot
// bring it to pitch within 2 cents, the resonator is lengthened or shortened (tuning scroll).
// measure(spec) returns the sounding frequency (0 = does not speak). Returns the remaining cents.
template <class Measure>
inline double tune(ReedPipeSpec& r, Measure measure, int maxIt = 14) {
    double cents = 1e9;
    for (int it = 0; it < maxIt; ++it) {
        const double f = measure(r);
        if (f <= 0) { r.fr *= 1.08f; continue; }        // not speaking: tighten the tongue
        cents = 1200 * std::log2(f / r.target);
        if (std::fabs(cents) < 0.5) break;
        const double want = std::pow(r.target / f, 2.0);
        const double fr = std::max(0.6 * r.target, std::min(1.6 * r.target, r.fr * want));
        if (std::fabs(fr - r.fr * want) > 1e-6 * r.target || (it >= 8 && std::fabs(cents) > 2)) {
            // the wire is at its end (or the tongue does not move the pitch enough): the scroll
            const double s = f / r.target;
            for (int i = 0; i < r.nModes; ++i) r.f[i] = (float)(r.f[i] / s);
            r.length = (float)(r.length * s); r.hfFrom = (float)(r.hfFrom / s);
        }
        r.fr = (float)fr;
    }
    return cents;
}
}} // namespace pipe::reed
