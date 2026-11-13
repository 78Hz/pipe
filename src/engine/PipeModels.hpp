// Pipe - physically modelled pipe organ
// Pipe sound generators: jet-driven flue pipe and beating-reed pipe.
//
// Both models are digital waveguides: a delay line holds the round trip of the
// pressure wave in the resonator, a one-pole filter models wall and radiation
// losses, and a nonlinear exciter (air jet or reed) sits at the mouth/boot.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace pipe {

constexpr int kBoreSize = 8192;   // power of two, >= one period of 16' C at 192 kHz / 2
constexpr int kJetSize  = 4096;

struct Noise {
    uint32_t s = 0x12345678u;
    void seed(uint32_t v) { s = v ? v : 0x9e3779b9u; }
    // white noise in [-1, 1)
    inline float next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(int32_t)s * (1.0f / 2147483648.0f);
    }
};

// 3rd-order Lagrange fractional read, `d` samples behind the last written sample (d >= 1).
template <int N>
inline float lagrangeRead(const float* buf, int w, float d) {
    const int i = (int)d;
    const float f = d - (float)i;
    const float x0 = buf[(w - i + 1) & (N - 1)];
    const float x1 = buf[(w - i)     & (N - 1)];
    const float x2 = buf[(w - i - 1) & (N - 1)];
    const float x3 = buf[(w - i - 2) & (N - 1)];
    const float fm1 = f - 1.0f, fm2 = f - 2.0f, fp1 = f + 1.0f;
    const float c0 = -f * fm1 * fm2 * (1.0f / 6.0f);
    const float c1 =  fp1 * fm1 * fm2 * 0.5f;
    const float c2 = -fp1 * f * fm2 * 0.5f;
    const float c3 =  fp1 * f * fm1 * (1.0f / 6.0f);
    return c0 * x0 + c1 * x1 + c2 * x2 + c3 * x3;
}

inline float onePoleCoef(float fc, float fs) {
    fc = std::min(fc, 0.45f * fs);
    return 1.0f - std::exp(-2.0f * (float)M_PI * fc / fs);
}

// Model families. The loop filters scale with the pipe's own frequency, so a
// family behaves the same across the compass.
enum FluePipeFamily { kFamilyOpen = 0, kFamilyStopped = 1, kFamilyString = 2, kNumFlueFamilies = 3 };
enum ReedPipeFamily { kReedTrumpet = 0, kReedOboe = 1, kNumReedFamilies = 2 };

struct FlueFamilyConst { float boreFc, jetFc, loopGain; };
constexpr FlueFamilyConst kFlueFamily[kNumFlueFamilies] = {
    { 6.0f, 1.5f, 0.985f },   // open: principals, open flutes
    { 6.0f, 1.5f, 0.985f },   // stopped: gedackts, bourdons
    { 11.0f, 1.7f, 0.99f },   // strings: narrow scale, many harmonics
};
struct ReedFamilyConst { float position, slope, boreFc; };
// The excitation point decides which register the cone settles in: nearer the apex
// than about 0.3 the model prefers the octave except in a sliver of blowing
// pressures; at 0.36 - 0.40 it speaks its fundamental over the whole range.
constexpr ReedFamilyConst kReedFamily[kNumReedFamilies] = {
    { 0.36f, 0.34f, 8.0f },   // trumpet / trombone: full conical resonator
    { 0.40f, 0.34f, 10.0f },  // oboe: narrow resonator, nasal
};
// Operating points of the two exciters. Both models only oscillate inside a
// window of "breath" (flue: above about 0.84, more for wide cut-ups; reed:
// about 0.55 to 0.86, blown shut above that), so the voiced point sits well inside that window; Engine::controlUpdate
// maps each pipe's foot pressure onto it, down to that pipe's own threshold.
constexpr float kFlueVoicedBreath = 1.0f;   // jet drive at the voiced foot pressure
constexpr float kReedVoicedBreath = 0.74f;  // reed blowing pressure at the voiced foot pressure
constexpr float kDcFc = 0.25f;              // DC blocker cutoff, multiple of f0

// ---------------------------------------------------------------------------
// Flue pipe: air jet from the windway crosses the mouth and is deflected by the
// acoustic flow; the deflected jet drives the resonator (the pipe body).
struct FlueCore {
    // tuning / geometry (set by voice)
    float boreDelay = 100.f;  // round-trip delay in samples (half period for stopped pipes)
    float jetDelay  = 30.f;   // jet transit time across the cut-up, samples
    float sign      = 1.f;    // +1 open pipe, -1 stopped pipe
    float loopGain  = 0.985f;
    float aBore = 0.5f, aJet = 0.5f, aDc = 0.01f;
    float jetRefl = 0.5f, endRefl = 0.5f;
    float bias = 0.f;         // languid / jet offset relative to the upper lip

    // runtime inputs
    float breath = 0.f;       // normalised jet drive (from foot pressure)
    float noiseAmt = 0.02f;   // turbulence

    float bore[kBoreSize];
    float jet[kJetSize];
    int wb = 0, wj = 0;
    float lpB = 0.f, lpJ = 0.f, dcIn = 0.f, dcOut = 0.f;
    float last = 0.f;
    Noise noise;

    void reset() {
        std::memset(bore, 0, sizeof(bore));
        std::memset(jet, 0, sizeof(jet));
        wb = wj = 0; lpB = lpJ = dcIn = dcOut = last = 0.f;
    }

    inline float tick() {
        const float b = lagrangeRead<kBoreSize>(bore, wb, boreDelay);
        lpB += aBore * (b - lpB);
        float t = sign * loopGain * lpB;
        // DC blocker (cutoff scaled to the pipe's pitch)
        dcOut = t - dcIn + (1.0f - aDc) * dcOut;
        dcIn = t;
        t = dcOut;

        const float pd = breath * (1.0f + noiseAmt * noise.next()) - jetRefl * t + bias;
        wj = (wj + 1) & (kJetSize - 1);
        jet[wj] = pd;
        const float pj = lagrangeRead<kJetSize>(jet, wj, jetDelay);
        lpJ += aJet * (pj - lpJ);
        float jt = lpJ * (lpJ * lpJ - 1.0f);
        jt = std::max(-1.0f, std::min(1.0f, jt));

        const float in = jt + endRefl * t;
        wb = (wb + 1) & (kBoreSize - 1);
        bore[wb] = in + 1e-20f;
        last = in;
        return in;
    }
};

// Bore delay that makes a flue loop sound at exactly `f` (first-harmonic
// balance): the loop must return the fundamental with gain 1 and no phase
// error. The effective jet gain G at the limit cycle is whatever makes the
// magnitude 1; the delay then follows from the phase. `nominal` is the plain
// delay (a period, or half of one for a stopped pipe) used to pick the branch.
inline double solveFlueBore(const FlueCore& c, double f, double fs, double jetDelay, double nominal) {
    const double w = 2.0 * M_PI * f / fs;
    const double cr = std::cos(w), ci = -std::sin(w);                 // z^-1
    auto onePole = [&](double a, double& re, double& im) {             // a / (1 - (1-a) z^-1)
        const double dr = 1.0 - (1.0 - a) * cr, di = -(1.0 - a) * ci, n = dr * dr + di * di;
        re = a * dr / n; im = -a * di / n;
    };
    double br, bi, jr, ji; onePole(c.aBore, br, bi); onePole(c.aJet, jr, ji);
    // DC blocker (1 - z^-1) / (1 - (1-aDc) z^-1)
    const double nr = 1.0 - cr, ni = -ci, dr = 1.0 - (1.0 - c.aDc) * cr, di = -(1.0 - c.aDc) * ci, dn = dr * dr + di * di;
    const double hr = (nr * dr + ni * di) / dn, hi = (ni * dr - nr * di) / dn;
    const double g0 = (double)c.sign * c.loopGain;
    const double ar = g0 * (br * hr - bi * hi), ai = g0 * (br * hi + bi * hr);     // A = sign * loopGain * HB * HDC
    const double er = std::cos(w * jetDelay), ei = -std::sin(w * jetDelay);
    const double ur = -(jr * er - ji * ei), ui = -(jr * ei + ji * er);             // u = -HJ * e^{-j w Tj}
    const double e = c.endRefl, k = c.jetRefl, R = 1.0 / std::max(1e-12, ar * ar + ai * ai);
    const double qa = k * k * (ur * ur + ui * ui), qb = 2.0 * e * k * ur, qc = e * e - R;
    const double G = (-qb + std::sqrt(std::max(0.0, qb * qb - 4.0 * qa * qc))) / (2.0 * qa);
    const double xr = e + k * G * ur, xi = k * G * ui;
    const double th = std::atan2(ar * xi + ai * xr, ar * xr - ai * xi);
    double best = nominal, bd = 1e30;
    for (int n = -2; n <= 3; ++n) {
        const double Tb = (th + 2.0 * M_PI * n) / w;
        if (std::fabs(Tb - nominal) < bd) { bd = std::fabs(Tb - nominal); best = Tb; }
    }
    return std::max(4.0, std::min((double)kBoreSize - 8.0, best));
}

// ---------------------------------------------------------------------------
// Reed pipe: a brass tongue beats against the shallot; the conical resonator
// above it (trumpet / oboe / trombone bell) shapes the tone. The cone is
// modelled as two waveguide sections either side of the excitation point, which
// gives the full harmonic series of a conical bore (after Cook's "Saxofony").
struct ReedCore {
    float delayA = 80.f;      // upper section (excitation -> bell), samples
    float delayB = 20.f;      // lower section (excitation -> apex), samples
    float loopGain = 0.95f;
    float aBore = 0.5f, aDc = 0.01f;
    float reedOffset = 0.7f, reedSlope = -0.3f;
    float breath = 0.f;
    float noiseAmt = 0.005f;

    float a[kBoreSize];
    float b[kJetSize];
    int wa = 0, wbb = 0;
    float lpB = 0.f, dcIn = 0.f, dcOut = 0.f, last = 0.f;
    Noise noise;

    void reset() {
        std::memset(a, 0, sizeof(a));
        std::memset(b, 0, sizeof(b));
        wa = wbb = 0; lpB = dcIn = dcOut = last = 0.f;
    }

    inline float tick() {
        const float outA = lagrangeRead<kBoreSize>(a, wa, delayA);
        const float outB = lagrangeRead<kJetSize>(b, wbb, delayB);
        lpB += aBore * (outA - lpB);
        const float temp = -loopGain * lpB;
        const float y = temp - outB;                    // pressure at the reed
        const float br = breath * (1.0f + noiseAmt * noise.next());
        const float pdiff = br - y;
        float r = reedOffset + reedSlope * pdiff;
        r = std::max(-1.0f, std::min(1.0f, r));
        wbb = (wbb + 1) & (kJetSize - 1);
        b[wbb] = temp + 1e-20f;
        wa = (wa + 1) & (kBoreSize - 1);
        a[wa] = br - pdiff * r - temp + 1e-20f;
        dcOut = y - dcIn + (1.0f - aDc) * dcOut;
        dcIn = y;
        last = dcOut;
        return dcOut;
    }
};

} // namespace pipe
