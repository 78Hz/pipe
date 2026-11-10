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
// family behaves the same across the compass; the tuning calibration tables in
// CalibrationTables.hpp were generated with these exact constants
// (tools/calibrate.cpp) and must be regenerated if they change.
enum FluePipeFamily { kFamilyOpen = 0, kFamilyStopped = 1, kFamilyString = 2, kNumFlueFamilies = 3 };
enum ReedPipeFamily { kReedTrumpet = 0, kReedOboe = 1, kNumReedFamilies = 2 };

struct FlueFamilyConst { float boreFc, jetFc, loopGain; };
constexpr FlueFamilyConst kFlueFamily[kNumFlueFamilies] = {
    { 6.0f, 3.0f, 0.985f },   // open: principals, open flutes
    { 6.0f, 3.0f, 0.985f },   // stopped: gedackts, bourdons
    { 11.0f, 3.2f, 0.99f },   // strings: narrow scale, many harmonics
};
struct ReedFamilyConst { float position, slope, boreFc; };
constexpr ReedFamilyConst kReedFamily[kNumReedFamilies] = {
    { 0.35f, 0.30f, 6.0f },   // trumpet / trombone: full conical resonator
    { 0.30f, 0.32f, 9.0f },   // oboe: narrow resonator, nasal
};
constexpr float kFlueVoicedBreath = 0.9f;   // jet drive at the voiced foot pressure
constexpr float kReedVoicedBreath = 0.72f;  // reed blowing pressure at the voiced foot pressure
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
