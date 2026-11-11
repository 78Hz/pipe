// Pipe - does every pipe speak, and at the right pitch?
//
// Everything here is judged on the sound the engine produces, never on the
// internal state it reports: a note counts as sounding only if there is audio,
// and as in tune only if the audio's fundamental is where the tuning says.
//
//   1. every pipe of every rank speaks, in the right register, in tune
//   2. every tuning system puts the notes where the literature says
//   3. MPE: a note sounds whatever the gap between note-on and first pressure
//   4. a key pressed part of the way still speaks
//   5. short notes speak
//   6. every note of a full chord speaks while the wind sags
#include "engine/Engine.hpp"

#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace pipe;

static int fs = 48000;
static int failures = 0;

static void check(bool ok, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", buf);
    if (!ok) ++failures;
}

static double rmsDb(const float* x, int n) {
    double e = 0; for (int i = 0; i < n; ++i) e += (double)x[i] * x[i];
    return 10 * std::log10(e / std::max(1, n) + 1e-20);
}

// Level (dB) of the partial at `f` in x.
static double partialDb(const float* x, int n, double f) {
    std::complex<double> z = 0;
    for (int i = 0; i < n; ++i) z += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / fs);
    return 20 * std::log10(2 * std::abs(z) / n + 1e-20);
}

// Refine a frequency estimate from the phase advance between the two halves of
// longer and longer stretches (each stretch short enough for the error left by
// the one before).
static double finePitch(const float* x, int n, double f) {
    for (int len = std::max(64, (int)(4 * fs / f)); ; len *= 3) {
        len = std::min(len, n);
        std::complex<double> z1 = 0, z2 = 0; const int mid = len / 2;
        for (int i = 0; i < mid; ++i) z1 += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / fs);
        for (int i = mid; i < 2 * mid; ++i) z2 += (double)x[i] * std::polar(1.0, -2 * M_PI * f * i / fs);
        f += std::arg(z2 * std::conj(z1)) / (2 * M_PI * mid / (double)fs);
        if (len == n) break;
    }
    return f;
}

// Cents between the sound and `expectHz`; 9999 when it is in the wrong register:
// when the strongest partial near the expected pitch is not the fundamental of
// what sounds (energy an octave or a twelfth below it, or the note itself all but
// missing under its own overtones).
static double centsOff(const float* x, int n, double expectHz) {
    const int n1 = std::min(n, std::max(1024, (int)(40 * fs / expectHz)));
    double f = expectHz, top = -300;
    for (double c = -70; c <= 70; c += 2) {
        const double ft = expectHz * std::pow(2.0, c / 1200.0), d = partialDb(x, n1, ft);
        if (d > top) { top = d; f = ft; }
    }
    if (std::getenv("PIPE_TEST_VERBOSE") && top < -80) {
        double bf = 0, bd = -300;
        for (double c = -2400; c <= 2400; c += 4) { const double ft = expectHz * std::pow(2.0, c / 1200.0); if (ft > 0.48 * fs) break; const double d = partialDb(x, n1, ft); if (d > bd) { bd = d; bf = c; } }
        std::printf("      %.1f Hz: nothing near the note; strongest partial %+.0f cents away at %.1f dB\n", expectHz, bf, bd);
    }
    if (std::fabs(1200 * std::log2(f / expectHz)) > 60) return 9999;
    for (double sub : { 0.5, 1.0 / 3.0, 2.0 / 3.0, 1.5 })
        for (double c = -70; c <= 70; c += 5)
            if (partialDb(x, n1, f * sub * std::pow(2.0, c / 1200.0)) > top - 15) {
                if (std::getenv("PIPE_TEST_VERBOSE")) std::printf("      %.1f Hz: partial at x%.3f %+.0f ct is %.1f dB against %.1f dB\n", expectHz, sub, c, partialDb(x, n1, f * sub * std::pow(2.0, c / 1200.0)), top);
                return 9999;
            }
    if (2 * f < 0.45 * fs && partialDb(x, n1, 2 * f) > top + 25) return 9999;
    if (3 * f < 0.45 * fs && partialDb(x, n1, 3 * f) > top + 25) return 9999;
    return 1200 * std::log2(finePitch(x, n, f) / expectHz);
}

struct Rig {
    std::unique_ptr<Engine> en;
    std::vector<float> L, R;
    Rig() { en = std::make_unique<Engine>(); reset(); }
    void reset() {
        en->prepare(fs);
        for (int i = 0; i < kOutFirst; ++i) en->setParameter(i, paramInfo(i).def);
        for (int s = 0; s < kNumStops; ++s) en->setParameter(s, 0.f);
        en->setMatrixText("");
        en->setParameter(kParamRoom, kRoomDry); en->setParameter(kParamRoomMix, 0.f);
        en->setParameter(kParamAgeDrift, 0.f); en->setParameter(kParamTuning, kTun12);
        en->setParameter(kParamBlowerNoise, -90.f); en->setParameter(kParamLimiter, 0.f);
        en->setParameter(kParamPedalSplit, 0.f);
        en->allNotesOff();
        L.clear(); R.clear();
    }
    void draw(int rank) {
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
    // the last `seconds` of audio, mono
    std::vector<float> tail(double seconds) const {
        const size_t n = (size_t)(seconds * fs);
        std::vector<float> m(n);
        for (size_t i = 0; i < n; ++i) m[i] = 0.5f * (L[L.size() - n + i] + R[R.size() - n + i]);
        return m;
    }
    double tailDb(double seconds) const { const auto m = tail(seconds); return rmsDb(m.data(), (int)m.size()); }
};

static double et(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }

// ---------------------------------------------------------------------------
static void everyPipe() {
    std::printf("1. every pipe of every rank: speaks, right register, in tune (12-TET, a' = 440)\n");
    std::printf("   %-16s %5s %6s %7s %10s %10s %9s\n", "rank", "pipes", "silent", "wrong", "first |ct|", "again |ct|", "speech ms");
    Rig g;
    for (int r = 0; r < kNumRanks; ++r) {
        const int stop = stopForRank(r), div = kRanks[r].division;
        if (kStops[stop].rankCount > 1) continue;        // mixture ranks cannot be drawn alone; their pipes are the same models
        int pipes = 0, silent = 0, wrong = 0; double worstFirst = 0, worstAgain = 0, slowest = 0; std::string bad;
        const int hi = div == kPedal ? kPedalHigh : kManualHigh;
        for (int key = kManualLow; key <= hi; ++key) {
            if (kRanks[r].lowestKey > 0 && key < kRanks[r].lowestKey) continue;
            const double f = et(key) * rankRatio(r, key) * std::pow(2.0, kRanks[r].detuneCents / 1200.0);
            if (f > 0.42 * fs) continue;
            ++pipes;
            g.reset(); g.draw(r);
            g.run(0.05);
            g.send(0x90, key, 100);
            const double hold = std::max(1.0, 90.0 / f);       // long enough for the lowest pipes to settle
            g.run(hold);
            const size_t t0 = g.L.size() - (size_t)(hold * fs);
            const auto first = g.tail(std::min(0.4, hold / 3));
            const double lvl = rmsDb(first.data(), (int)first.size());
            char buf[64];
            if (lvl < -75) { ++silent; std::snprintf(buf, sizeof buf, " %d:silent", key); bad += buf; continue; }
            // The model stops being a pipe when a period is only a handful of samples; those
            // few top pipes are only required to speak.
            const bool tiny = f > 0.145 * fs;
            const double c1 = tiny ? 0 : centsOff(first.data(), (int)first.size(), f);
            // time to speech: the first 10 ms stretch within 6 dB of the settled level
            for (int i = 0; i + 480 < (int)(hold * fs); i += 96) {
                double e = 0; for (int j = 0; j < 480; ++j) { const double s = 0.5 * (g.L[t0 + (size_t)(i + j)] + g.R[t0 + (size_t)(i + j)]); e += s * s; }
                if (10 * std::log10(e / 480 + 1e-20) > lvl - 6) { slowest = std::max(slowest, (double)i / fs * f); break; }
            }
            // let go, strike again: the pipe has to be in tune from the start now
            g.send(0x80, key, 64); g.run(0.4);
            g.send(0x90, key, 100);
            const double again = std::max(0.6, 50.0 / f);
            g.run(again);
            const auto second = g.tail(again * 0.4);
            const double c2 = tiny ? 0 : centsOff(second.data(), (int)second.size(), f);
            if (c1 > 9000 || c2 > 9000) { ++wrong; std::snprintf(buf, sizeof buf, " %d:register(%s)", key, c1 > 9000 ? "first" : "again"); bad += buf; continue; }
            worstFirst = std::max(worstFirst, std::fabs(c1)); worstAgain = std::max(worstAgain, std::fabs(c2));
            if (std::getenv("PIPE_TEST_VERBOSE") && (std::fabs(c1) > 2 || std::fabs(c2) > 1.5)) std::printf("      %s key %d (%.1f Hz): first %+.2f, again %+.2f\n", kRanks[r].name, key, f, c1, c2);
        }
        std::printf("   %-16s %5d %6d %7d %10.2f %10.2f %7.0f per %s\n", kRanks[r].name, pipes, silent, wrong, worstFirst, worstAgain, slowest, bad.substr(0, 80).c_str());
        check(silent == 0 && wrong == 0, "%s: all %d pipes speak in the right register", kRanks[r].name, pipes);
        // A reed's pitch is still settling while its tone builds up, so it gets a little more room.
        if (kRanks[r].kind == kKindReed)
            check(worstFirst <= 3.0 && worstAgain <= 4.0, "%s: within 3 cents once settled, 4 cents in the half second after a re-strike", kRanks[r].name);
        else
            check(worstFirst <= 3.0 && worstAgain <= 1.5, "%s: within 3 cents the first time a pipe speaks, 1.5 cents after that", kRanks[r].name);
    }
}

// ---------------------------------------------------------------------------
static void tuningSystems() {
    std::printf("2. tuning systems, measured on the Principal 8' (cents above C, from the literature)\n");
    struct Ref { int sys; const char* name; double cents[12]; };
    static const Ref refs[] = {
        { kTun12, "12-TET", { 0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100 } },
        { kTunWerck, "Werckmeister III", { 0, 90.225, 192.18, 294.135, 390.225, 498.045, 588.27, 696.09, 792.18, 888.27, 996.09, 1092.18 } },
        { kTunMeantone, "1/4-comma meantone", { 0, 76.049, 193.157, 310.265, 386.314, 503.422, 579.471, 696.578, 772.627, 889.735, 1006.843, 1082.892 } },
        { kTunKirn, "Kirnberger III", { 0, 90.225, 193.157, 294.135, 386.314, 498.045, 590.224, 696.578, 792.18, 889.735, 996.09, 1088.269 } },
        { kTunVallotti, "Vallotti", { 0, 94.135, 196.09, 298.045, 392.18, 501.955, 592.18, 698.045, 796.09, 894.135, 1000.0, 1090.225 } },
    };
    const int principal = 7;
    Rig g;
    auto play = [&](int key, double expect) {
        g.send(0x90, key, 100); g.run(0.8);
        const auto m = g.tail(0.3);
        const double c = centsOff(m.data(), (int)m.size(), expect);
        g.send(0x80, key, 64); g.run(0.3);
        return c;
    };
    for (const Ref& ref : refs) {
        g.reset(); g.draw(principal); g.en->setParameter(kParamTuning, (float)ref.sys);
        g.run(0.05);
        double worst = 0;
        for (int pc = 0; pc < 12; ++pc) {
            // a' = 440 in every system
            const double hz = 440.0 * std::pow(2.0, (ref.cents[pc] - ref.cents[9]) / 1200.0) * (pc > 9 ? 1.0 : 1.0);
            const int key = 60 + pc;
            const double expect = hz * std::pow(2.0, (key - 69 - (pc - 9)) / 12.0);
            worst = std::max(worst, std::fabs(play(key, expect)));
        }
        check(worst <= 2.0, "%s: all twelve notes within 2 cents (worst %.2f)", ref.name, worst);
    }
    // equal divisions: the keys run through the steps one after another on the upper zone map
    static const struct { int sys, n; } edos[] = { { kTun19, 19 }, { kTun22, 22 }, { kTun24, 24 }, { kTun31, 31 }, { kTun41, 41 }, { kTun53, 53 } };
    for (const auto& e : edos) {
        g.reset(); g.draw(principal); g.en->setParameter(kParamTuning, (float)e.sys);
        g.en->setParameter(kParamMapLo, (float)kMapSequential);
        g.run(0.05);
        // key 60 is C; the step that serves as A (three fifths up) sits on 440 Hz
        const int fifth = (int)std::lround(e.n * std::log2(1.5));
        const int aStep = ((3 * fifth) % e.n + e.n) % e.n;
        double worst = 0;
        for (int key : { 48, 60, 61, 62, 60 + aStep, 60 + e.n > kManualHigh ? 72 : 60 + e.n, 84 }) {
            const double expect = 440.0 * std::pow(2.0, (double)(key - 60 - aStep) / e.n);
            worst = std::max(worst, std::fabs(play(key, expect)));
        }
        check(worst <= 2.0, "%d-EDO, keys in step order: within 2 cents (worst %.2f)", e.n, worst);
        // on the 12-key subset map a fifth on the keyboard is the division's own fifth
        g.en->setParameter(kParamMapLo, (float)kMapSubset);
        const double a = 440.0, eAbove = 440.0 * std::pow(2.0, (double)fifth / e.n);
        const double w = std::max(std::fabs(play(69, a)), std::fabs(play(76, eAbove)));
        check(w <= 2.0, "%d-EDO, 12-key subset: a' = 440 and the fifth above it (worst %.2f)", e.n, w);
    }
    // a' reference, one pipe's own offset, and the tuning changing under a held key
    g.reset(); g.draw(principal); g.en->setParameter(kParamARef, 415.f); g.run(0.05);
    check(std::fabs(play(69, 415.0)) <= 2.0, "a' = 415 Hz");
    g.reset(); g.draw(principal); g.run(0.05);
    g.send(0x90, 69, 100); g.run(0.6);
    g.en->setParameter(kParamTuning, (float)kTunMeantone); g.en->setParameter(kParamARef, 430.f);
    g.send(0x80, 69, 64); g.run(0.02);        // still sounding when it is struck again
    g.send(0x90, 69, 100); g.run(0.8);
    { const auto m = g.tail(0.3); check(std::fabs(centsOff(m.data(), (int)m.size(), 430.0)) <= 2.0, "a pipe re-struck while it is still sounding takes the new tuning"); }
}

// ---------------------------------------------------------------------------
static void mpeTiming() {
    std::printf("3. MPE: note-on, then the first pressure message after a gap (Principal 8', member channel 2)\n");
    Rig g;
    auto setup = [&] {
        g.reset(); g.draw(7);
        g.send(0xB0, 101, 0); g.send(0xB0, 100, 6); g.send(0xB0, 6, 15);     // MCM: lower zone, 15 member channels
        g.run(0.05);
    };
    setup(); g.send(0x90, 60, 100); g.run(1.0);
    const double struck = g.tailDb(0.3);
    for (int gapMs : { 0, 1, 3, 6, 8, 12, 20, 50, 200, 1000 }) {
        setup();
        g.send(0xD1, 0);                       // key resting: pressure 0 before the note
        g.send(0x91, 60, 100);
        g.run(gapMs / 1000.0);
        for (int p = 8; p <= 127; p += 7) { g.send(0xD1, p); g.run(0.002); }
        g.send(0xD1, 127); g.run(1.0);
        const double lvl = g.tailDb(0.3);
        check(std::fabs(lvl - struck) < 1.5, "gap %4d ms: sounds at %.1f dB (a struck note: %.1f dB)", gapMs, lvl, struck);
    }
    setup(); g.send(0xD1, 0); g.send(0x91, 60, 100);
    g.send(0xD1, 127); g.run(0.6); const double a = g.tailDb(0.2);
    g.send(0xD1, 0); g.run(1.5); const double quiet = g.tailDb(0.2);
    g.send(0xD1, 127); g.run(0.6); const double b = g.tailDb(0.2);
    check(quiet < a - 40 && std::fabs(a - b) < 1.5, "pressure to zero and back with the key held: %.1f dB, silence (%.1f dB), %.1f dB", a, quiet, b);
}

// ---------------------------------------------------------------------------
static void partTravel() {
    std::printf("4. a key pressed part of the way (MPE pressure opens the pallet)\n");
    Rig g;
    for (int r : { 7, 0, 18, 19, 17, 5 }) {
        g.reset(); g.draw(r);
        g.send(0xB0, 101, 0); g.send(0xB0, 100, 6); g.send(0xB0, 6, 15);
        g.run(0.05);
        const int key = 48;
        const double f = et(key) * rankRatio(r, key) * std::pow(2.0, kRanks[r].detuneCents / 1200.0);
        g.send(0xD1, 127); g.send(0x91, key, 100); g.run(1.5);
        const double full = g.tailDb(0.3);
        std::string line; bool ok = true;
        for (int pct : { 15, 25, 40, 60, 80 }) {
            g.send(0xD1, pct * 127 / 100); g.run(1.2);
            const auto m = g.tail(0.4);
            const double lvl = rmsDb(m.data(), (int)m.size()), c = centsOff(m.data(), (int)m.size(), f);
            char b[64]; std::snprintf(b, sizeof b, " %d%%: %+.1f dB %+.1f ct;", pct, lvl - full, c > 9000 ? 999.0 : c); line += b;
            // speaks, no louder than full, and never further than an eighth-tone from the note
            if (lvl < full - 16 || lvl > full + 0.5 || std::fabs(c) > 13) ok = false;
        }
        check(ok, "%s:%s", kRanks[r].name, line.c_str());
    }
}

// ---------------------------------------------------------------------------
static void shortNotes() {
    std::printf("5. short notes\n");
    Rig g;
    struct Case { int rank, key; double ms; };
    for (const Case& c : { Case{ 7, 60, 60 }, Case{ 7, 48, 90 }, Case{ 7, 36, 230 }, Case{ 18, 60, 60 }, Case{ 17, 60, 80 }, Case{ 17, 48, 140 }, Case{ 0, 36, 420 }, Case{ 5, 36, 450 } }) {
        g.reset(); g.draw(c.rank); g.run(0.05);
        g.send(0x90, c.key, 100); g.run(1.5); const double steady = g.tailDb(0.3);
        g.send(0x80, c.key, 64); g.run(0.6);
        const size_t t0 = g.L.size();
        g.send(0x90, c.key, 100); g.run(c.ms / 1000.0); g.send(0x80, c.key, 64); g.run(0.3);
        double peak = -200;
        for (size_t i = t0; i + 480 <= g.L.size(); i += 120) {
            double e = 0; for (size_t j = 0; j < 480; ++j) { const double s = 0.5 * (g.L[i + j] + g.R[i + j]); e += s * s; }
            peak = std::max(peak, 10 * std::log10(e / 480 + 1e-20));
        }
        check(peak > steady - 6, "%s key %d held %3.0f ms: reaches %.1f dB of its full level", kRanks[c.rank].name, c.key, c.ms, peak - steady);
    }
}

// ---------------------------------------------------------------------------
static void fullChord() {
    std::printf("6. full organ, ten-note chord: every note's Principal still speaks while the wind sags\n");
    Rig g;
    g.reset();
    for (int s = 0; s < kNumStops; ++s) if (kStops[s].type == kStopRank || kStops[s].type == kStopMixture) g.en->setParameter(s, 1.f);
    g.en->setParameter(kParamZoneLoDiv, (float)kGreat);
    g.run(0.05);
    const int keys[] = { 36, 43, 48, 52, 55, 60, 64, 67, 72, 76 };
    for (int k : keys) g.send(0x90, k, 100);
    g.run(2.5);
    const auto m = g.tail(0.5);
    double weakest = 0, worst = 0; bool first = true;
    for (int k : keys) {
        const double f0 = et(k);
        // strongest partial within 15 cents of where this note's Principal belongs
        double f = f0, db = -200;
        for (double c = -15; c <= 15; c += 0.5) {
            const double ft = f0 * std::pow(2.0, c / 1200.0);
            std::complex<double> z = 0;
            for (size_t i = 0; i < m.size(); ++i) z += (double)m[i] * std::polar(1.0, -2 * M_PI * ft * (double)i / fs);
            const double d = 20 * std::log10(2 * std::abs(z) / (double)m.size() + 1e-20);
            if (d > db) { db = d; f = ft; }
        }
        if (first || db < weakest) weakest = db;
        first = false;
        worst = std::max(worst, std::fabs(1200 * std::log2(f / f0)));
    }
    check(weakest > -60, "weakest fundamental in the chord: %.1f dB", weakest);
    check(worst < 8, "the chord sags by no more than %.1f cents", worst);
    check(g.tailDb(0.5) < -3, "and it does not overload (%.1f dB RMS)", g.tailDb(0.5));
}

int main() {
    if (const char* e = std::getenv("PIPE_TEST_FS")) fs = std::max(22050, std::atoi(e));
    std::printf("sample rate %d Hz\n", fs);
    everyPipe();
    tuningSystems();
    mpeTiming();
    partTravel();
    shortNotes();
    fullChord();
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
