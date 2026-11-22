// Pipe - the one judge of a pipe: played through the engine, as the plugin plays it, and judged on its sound.
//
// Used by the pipe test (tests/pipes_test.cpp) and by the voicer (tools/engine_voicer.cpp), so a pipe the
// voicer accepts is a pipe the test passes. A pipe is played alone (its rank drawn, nothing else), on its
// groove in its chest, in the organ's air, at the sample rate given, and must:
//   - speak (not silent) and in the right register, the first time and every time after
//   - be in tune the first time it speaks (3 cents; the tuner has not heard it yet), and after the tuner has
//     heard it 1.5 cents (flue) / 3 cents (reed, settled)
//   - hold that pitch and level while the key is held (an organist holds a chord for many seconds): every
//     window of the hold within 3 cents, the level within 2 dB of its range
//   - speak the same when struck softly and hard (the key action's pallet opening)
//   - keep speaking its note under the tremulant
#pragma once

#include "engine/Engine.hpp"

#include <algorithm>
#include <complex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace pipe {
namespace judge {

inline double rmsDb(const float* x, int n) {
    double e = 0; for (int i = 0; i < n; ++i) e += (double)x[i] * x[i];
    return 10 * std::log10(e / std::max(1, n) + 1e-20);
}
// Level (dB) of the partial at `f` in x.
inline double partialDb(const float* x, int n, double f, double fs) {
    std::complex<double> z = 0;
    for (int i = 0; i < n; ++i) z += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / fs);
    return 20 * std::log10(2 * std::abs(z) / n + 1e-20);
}
// The same through a Hann window: what leaks from a strong partial a few bins away (a bass reed's second
// partial, 15 dB over its fundamental) falls 30 dB lower, so a partial that is not there reads as not there.
inline double partialDbHann(const float* x, int n, double f, double fs) {
    std::complex<double> z = 0;
    for (int i = 0; i < n; ++i) z += (double)x[i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / n)) * std::polar(1.0, -2 * M_PI * f * i / fs);
    return 20 * std::log10(4 * std::abs(z) / n + 1e-20);
}
// Refine a frequency estimate from the phase advance between the two halves of longer and longer
// stretches (each stretch short enough for the error left by the one before).
inline double finePitch(const float* x, int n, double f, double fs, int firstLen = 0) {
    for (int len = std::max({ 64, (int)(4 * fs / f), firstLen }); ; len *= 3) {
        len = std::min(len, n);
        std::complex<double> z1 = 0, z2 = 0; const int mid = len / 2;
        for (int i = 0; i < mid; ++i) z1 += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / fs);
        for (int i = mid; i < 2 * mid; ++i) z2 += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / fs);
        f += std::arg(z2 * std::conj(z1)) / (2 * M_PI * mid / fs);
        if (len == n) break;
    }
    return f;
}
// The pitch of a harmonic sound whose fundamental is near f, read from its strongest partial of the first
// four (divided by its number), as a tuner listens to a 32' reed whose fundamental is 15 dB under its
// second partial: the weak fundamental's phase is the noisiest thing in the sound.
inline double pitchOf(const float* x, int n, double f, double fs) {
    const int n1 = std::min(n, std::max(1024, (int)(40 * fs / f)));
    int best = 1; double bd = partialDbHann(x, n1, f, fs);
    for (int h = 2; h <= 4 && h * f < 0.4 * fs; ++h) { const double d = partialDbHann(x, n1, h * f, fs); if (d > bd + 3) { bd = d; best = h; } }
    return finePitch(x, n, best * f, fs, (int)(8 * fs / f)) / best;   // stretches of whole fundamental periods, so its neighbours do not leak in
}
// Cents between the sound and `expectHz`; 9999 when it is in the wrong register: when the strongest
// partial near the expected pitch is not the fundamental of what sounds (energy an octave or a twelfth
// below it, or the note itself all but missing under its own overtones).
inline double centsOff(const float* x, int n, double expectHz, double fs, bool verbose = false) {
    const int n1 = std::min(n, std::max(1024, (int)(40 * fs / expectHz)));
    double f = expectHz, top = -300;
    for (double c = -70; c <= 70; c += 2) {
        const double ft = expectHz * std::pow(2.0, c / 1200.0), d = partialDbHann(x, n1, ft, fs);
        if (d > top) { top = d; f = ft; }
    }
    if (verbose && top < -80) {
        double bf = 0, bd = -300;
        for (double c = -2400; c <= 2400; c += 4) { const double ft = expectHz * std::pow(2.0, c / 1200.0); if (ft > 0.48 * fs) break; const double d = partialDbHann(x, n1, ft, fs); if (d > bd) { bd = d; bf = c; } }
        std::printf("      %.1f Hz: nothing near the note; strongest partial %+.0f cents away at %.1f dB\n", expectHz, bf, bd);
    }
    if (std::fabs(1200 * std::log2(f / expectHz)) > 60) return 9999;
    for (double sub : { 0.5, 1.0 / 3.0, 2.0 / 3.0, 1.5 })
        for (double c = -70; c <= 70; c += 5)
            if (partialDbHann(x, n1, f * sub * std::pow(2.0, c / 1200.0), fs) > top - 15) {
                if (verbose) std::printf("      %.1f Hz: partial at x%.3f %+.0f ct is %.1f dB against %.1f dB\n", expectHz, sub, c, partialDbHann(x, n1, f * sub * std::pow(2.0, c / 1200.0), fs), top);
                return 9999;
            }
    if (2 * f < 0.45 * fs && partialDbHann(x, n1, 2 * f, fs) > top + 25) return 9999;
    if (3 * f < 0.45 * fs && partialDbHann(x, n1, 3 * f, fs) > top + 25) return 9999;
    return 1200 * std::log2(pitchOf(x, n, f, fs) / expectHz);
}

// The pitch of the strongest partial near the note, whatever else sounds (the voicer cuts a pipe to pitch with it
// before judging its register).
inline double centsRaw(const float* x, int n, double expectHz, double fs) {
    const int n1 = std::min(n, std::max(1024, (int)(40 * fs / expectHz)));
    double f = expectHz, top = -300;
    for (double c = -70; c <= 70; c += 2) { const double ft = expectHz * std::pow(2.0, c / 1200.0), d = partialDbHann(x, n1, ft, fs); if (d > top) { top = d; f = ft; } }
    if (top < -100) return 9999;
    return 1200 * std::log2(finePitch(x, n, f, fs) / expectHz);
}

inline double et(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }

// The engine as the plugin runs it, dry, equal temperament, no drift, no blower noise or limiter.
struct Rig {
    std::unique_ptr<Engine> en;
    std::vector<float> L, R;
    int fs = 48000;
    explicit Rig(int sampleRate = 48000) : fs(sampleRate) { en = std::make_unique<Engine>(); reset(); }
    void reset() {
        en->prepare(fs);
        for (int i = 0; i < kOutFirst; ++i) en->setParameter(i, paramInfo(i).def);
        for (int s = 0; s < kNumStops; ++s) en->setParameter(s, 0.f);
        en->setMatrixText("");
        en->setParameter(kParamRoom, kRoomDry); en->setParameter(kParamRoomMix, 0.f);
        en->setParameter(kParamAgeDrift, 0.f); en->setParameter(kParamTuning, kTun12);
        en->setParameter(kParamBlowerNoise, -90.f); en->setParameter(kParamLimiter, 0.f);
        en->setParameter(kParamPedalSplit, 0.f);
        en->setSoloRank(-1);
        en->allNotesOff();
        L.clear(); R.clear();
    }
    void draw(int rank) {
        en->setSoloRank(kStops[stopForRank(rank)].rankCount > 1 ? rank : -1);   // a mixture's rank on its own
        en->setParameter(stopForRank(rank), 1.f);
        en->setParameter(kParamZoneLoDiv, (float)kRanks[rank].division);
    }
    void run(double seconds) {
        const int n = (int)(seconds * fs);
        const size_t o = L.size(); L.resize(o + (size_t)n); R.resize(o + (size_t)n);
        for (int i = 0; i < n; i += 64) en->render(&L[o + (size_t)i], &R[o + (size_t)i], std::min(64, n - i));
    }
    void send(int a, int b, int c = -1) {
        const uint8_t m[3] = { (uint8_t)a, (uint8_t)b, (uint8_t)(c < 0 ? 0 : c) };
        en->midi(m, c < 0 ? 2 : 3);
    }
    // mono, from `from` seconds before the end, `seconds` long
    std::vector<float> span(double fromEnd, double seconds) const {
        const size_t n = (size_t)(seconds * fs), back = (size_t)(fromEnd * fs);
        std::vector<float> m(n);
        const size_t o = L.size() - back;
        for (size_t i = 0; i < n; ++i) m[i] = 0.5f * (L[o + i] + R[o + i]);
        return m;
    }
    std::vector<float> tail(double seconds) const { return span(seconds, seconds); }
};

// The pitch the rank plays on a key (12-TET, its foot, its detune).
inline double pipeHz(int rank, int key) { return et(key) * rankRatio(rank, key) * std::pow(2.0, kRanks[rank].detuneCents / 1200.0); }

struct Verdict {
    bool played = false;          // the rank has a pipe here (in the compass, within the model's range)
    bool silent = false, wrongFirst = false, wrongAgain = false, wrongHold = false, wrongVel = false, wrongTrem = false;
    double first = 0, again = 0;  // cents
    double holdWorst = 0;         // worst |cents| over the long hold's windows (first strike)
    double holdLevel = 0;         // the level's range over the hold (dB)
    double velWorst = 0;          // worst |cents| of the soft and hard strikes
    double tremCents = 0;         // mean pitch under the tremulant (cents)
    double tremLevel = 0;         // its level against the pipe's own (dB)
    double level = -300;          // the pipe's level (dB RMS)
    double rawFirst = 9999;       // first strike: the strongest partial near the note (cents), register aside
    double speechPeriods = 0;
    bool reed = false, tiny = false;
    std::string why;
    bool registerOk() const { return !silent && !wrongFirst && !wrongAgain && !wrongHold && !wrongVel && !wrongTrem; }
    bool firstOk() const { return std::fabs(first) <= 3.0; }
    bool againOk() const { return std::fabs(again) <= (reed ? 3.0 : 1.5); }
    bool holdOk() const { return holdWorst <= 3.0 && holdLevel <= 2.0; }
    bool velOk() const { return velWorst <= (reed ? 3.0 : 1.5); }
    double speechLimit = 1e9;     // periods
    bool speechOk() const { return speechPeriods <= speechLimit; }
    bool tremOk() const { return std::fabs(tremCents) <= 10.0 && tremLevel > -6.0; }
    bool pass() const { return played && registerOk() && firstOk() && againOk() && speechOk() && holdOk() && velOk() && tremOk(); }
    // how far from passing (0 = passes): register faults count most, then each margin exceeded
    double badness() const {
        if (!played) return 0;
        if (silent) return 1000;
        if (wrongFirst || wrongAgain) return 500;
        if (wrongHold || wrongVel || wrongTrem) return 300;
        auto over = [](double x, double lim) { return std::max(0.0, x / lim - 1.0); };
        return over(std::fabs(first), 3.0) + over(std::fabs(again), reed ? 3.0 : 1.5) + over(holdWorst, 3.0) + over(holdLevel, 2.0)
             + over(speechPeriods, speechLimit) + over(velWorst, reed ? 3.0 : 1.5) + over(std::fabs(tremCents), 10.0) + over(-tremLevel, 6.0);
    }
    // the first thing wrong, for a report
    std::string fault() const {
        char b[160];
        if (!played) return "not played";
        if (silent) return "silent";
        if (wrongFirst) return "wrong register (first strike)";
        if (wrongAgain) return "wrong register (re-strike)";
        if (wrongVel) return "wrong register (soft or hard strike)";
        if (wrongHold) return "leaves its register while held";
        if (wrongTrem) return "wrong register under the tremulant";
        if (!firstOk()) { std::snprintf(b, sizeof b, "first strike %+.1f c", first); return b; }
        if (!againOk()) { std::snprintf(b, sizeof b, "re-strike %+.1f c", again); return b; }
        if (!speechOk()) { std::snprintf(b, sizeof b, "slow to speak: %.0f periods (%.0f allowed)", speechPeriods, speechLimit); return b; }
        if (!holdOk()) { std::snprintf(b, sizeof b, "while held: pitch %.1f c, level %.1f dB", holdWorst, holdLevel); return b; }
        if (!velOk()) { std::snprintf(b, sizeof b, "soft/hard strike %.1f c", velWorst); return b; }
        if (!tremOk()) { std::snprintf(b, sizeof b, "under the tremulant %+.1f c, %+.1f dB", tremCents, tremLevel); return b; }
        return "ok";
    }
};

struct Options {
    bool full = true;      // false: only what the pipe test's first part has always judged (first strike, re-strike)
    bool verbose = false;
    bool stopEarly = false;   // stop at the first fault found (the voicer's quick reject)
};

// How soon a pipe must speak, in its own periods (within 6 dB of its settled level): 30 periods, a keen
// string 50 (Pykett, "The tonal structure of organ strings"), the large stopped basses (Bourdon 16',
// Subbass) 50 and below 50 Hz 80 (the owner: "it is normal for sub basses to take longer to sound"); never
// less than 0.3 s (the ear does not hear a pipe's speech as late before that).
inline double speechLimitFor(int r, double f) {
    const bool bass = std::strstr(kRanks[r].name, "Subbass") || std::strcmp(kRanks[r].name, "Bourdon 16'") == 0;
    const double n = bass ? (f < 50.0 ? 80.0 : 50.0) : kRanks[r].kind == kKindString ? 50.0 : 30.0;
    return std::max(n, 0.3 * f);
}
// How long a pipe is held for its first strike (the lowest pipes must settle) and for the long hold.
inline double holdFor(double f) { return std::max(1.0, 90.0 / f); }
inline double longHoldFor(double f) { return std::max(6.0, 3.0 * holdFor(f)); }

inline Verdict judgePipe(Rig& g, int r, int key, const Options& o = Options()) {
    Verdict v;
    v.reed = kRanks[r].kind == kKindReed;
    const double fs = g.fs;
    const double f = pipeHz(r, key);
    if (f > 0.42 * fs) return v;
    if (kRanks[r].lowestKey > 0 && key < kRanks[r].lowestKey) return v;
    v.played = true;
    v.speechLimit = speechLimitFor(r, f);
    // The model stops being a pipe when a period is only a handful of samples; those few top pipes are
    // only required to speak.
    v.tiny = f > 0.145 * fs;
    auto cents = [&](const std::vector<float>& x) { return v.tiny ? 0.0 : centsOff(x.data(), (int)x.size(), f, fs, o.verbose); };

    g.reset(); g.draw(r);
    g.run(0.05);
    g.send(0x90, key, 100);
    const double hold = holdFor(f);
    g.run(hold);
    const size_t t0 = g.L.size() - (size_t)(hold * fs);
    // the first reading: 0.4 s, or 20 periods for the lowest pipes (6 periods of a 32' tell nothing)
    const double firstWin = std::max(std::min(0.4, hold / 3), std::min(20.0 / f, hold / 2));
    const auto first = g.tail(firstWin);
    v.level = rmsDb(first.data(), (int)first.size());
    if (v.level < -75) { v.silent = true; return v; }
    v.rawFirst = v.tiny ? 0.0 : centsRaw(first.data(), (int)first.size(), f, fs);
    v.first = cents(first);
    if (v.first > 9000) { v.wrongFirst = true; return v; }
    if (o.stopEarly && !v.firstOk()) return v;
    // time to speech: the first 10 ms stretch within 6 dB of the settled level
    for (int i = 0; i + 480 < (int)(hold * fs); i += 96) {
        double e = 0; for (int j = 0; j < 480; ++j) { const double s = 0.5 * (g.L[t0 + (size_t)(i + j)] + g.R[t0 + (size_t)(i + j)]); e += s * s; }
        if (10 * std::log10(e / 480 + 1e-20) > v.level - 6) { v.speechPeriods = (double)i / fs * f; break; }
    }
    if (o.stopEarly && !v.speechOk()) return v;
    if (o.full) {
        // held on: every window of the hold from the first reading on, in tune and at one level
        const double lh = longHoldFor(f), win = std::max(0.4, 20.0 / f);
        g.run(lh - hold);
        double lo = 1e9, hi = -1e9;
        const int nw = (int)((lh - hold + firstWin) / win);
        for (int w = 0; w < nw; ++w) {
            const auto x = g.span((nw - w) * win, win);
            const double c = cents(x), d = rmsDb(x.data(), (int)x.size());
            if (c > 9000) { v.wrongHold = true; return v; }
            v.holdWorst = std::max(v.holdWorst, std::fabs(c));
            lo = std::min(lo, d); hi = std::max(hi, d);
        }
        v.holdLevel = nw > 0 ? hi - lo : 0;
        if (o.stopEarly && !v.holdOk()) return v;
    }
    // let go, strike again: the pipe has to be in tune from the start now
    auto restrike = [&](int vel, double& c) {
        g.send(0x80, key, 64); g.run(0.4);
        g.send(0x90, key, vel);
        // A reed struck from silence starts flat and takes about 40 periods to settle; its settled pitch
        // is what is compared.
        const double again = v.reed ? holdFor(f) : std::max(0.6, 50.0 / f);
        g.run(again);
        const auto x = g.tail(v.reed ? std::max(std::min(0.4, again / 3), std::min(20.0 / f, again / 2)) : again * 0.4);
        c = cents(x);
        return c < 9000;
    };
    if (!restrike(100, v.again)) { v.wrongAgain = true; return v; }
    if (!o.full || (o.stopEarly && !v.againOk())) return v;
    for (int vel : { 20, 127 }) {
        double c; if (!restrike(vel, c)) { v.wrongVel = true; return v; }
        v.velWorst = std::max(v.velWorst, std::fabs(c));
    }
    if (o.stopEarly && !v.velOk()) return v;
    // the tremulant on its division (the default kind, speed and depth): the note heard over whole beats
    {
        g.send(0x80, key, 64); g.run(0.4);
        g.en->setParameter(kNumStops - 1, 1.f);
        g.en->setParameter(kParamTremTarget, (float)kRanks[r].division);
        g.send(0x90, key, 100);
        const double rate = std::max(1.0, (double)g.en->getParameter(kParamTremSpeed));
        const double beats = std::ceil(std::max(4.0, 20.0 / f * rate)) / rate;   // whole beats, at least 20 of the pipe's periods
        g.run(holdFor(f) + beats);
        const auto x = g.tail(beats);
        const double c = cents(x);
        g.en->setParameter(kNumStops - 1, 0.f);
        if (c > 9000) { v.wrongTrem = true; return v; }
        v.tremCents = c; v.tremLevel = rmsDb(x.data(), (int)x.size()) - v.level;
    }
    return v;
}

} // namespace judge
} // namespace pipe
