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
#include "PipeJudge.hpp"
#include <atomic>
#include <thread>

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

// the judge's measures (PipeJudge.hpp), at this run's rate
static double partialDb(const float* x, int n, double f) { return judge::partialDb(x, n, f, fs); }
static double finePitch(const float* x, int n, double f) { return judge::finePitch(x, n, f, fs); }
static double centsOff(const float* x, int n, double expectHz) { return judge::centsOff(x, n, expectHz, fs, std::getenv("PIPE_TEST_VERBOSE") != nullptr); }

static int rankOf(const char* stopId) { const int s = stopById(stopId); return s >= 0 ? kStops[s].rankFirst : 0; }

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
        for (auto& t : tapped) t.clear();
    }
    void draw(int rank) {
        en->setParameter(stopForRank(rank), 1.f);
        en->setParameter(kParamZoneLoDiv, (float)kRanks[rank].division);
    }
    // Listening to single pipes inside a full organ: each tapped pipe's own sound (what its model gives,
    // before the room), taken block by block as the engine renders it, so one pipe can be judged while
    // the whole organ plays and draws on the wind. Silence where the pipe is not sounding.
    std::vector<std::pair<int, int>> taps;
    std::vector<std::vector<float>> tapped;
    void tap(int rank, int key) { taps.push_back({ rank, key }); tapped.emplace_back(); }
    void run(double seconds) {
        const int n = (int)(seconds * fs);
        const size_t o = L.size(); L.resize(o + (size_t)n); R.resize(o + (size_t)n);
        if (taps.empty()) { for (int i = 0; i < n; i += 64) en->render(&L[o + (size_t)i], &R[o + (size_t)i], std::min(64, n - i)); return; }
        for (int i = 0; i < n;) {
            // within one control block: the voices' block buffers then hold exactly these samples
            const int len = std::min(n - i, kControlBlock - (int)((o + (size_t)i) % kControlBlock));
            en->render(&L[o + (size_t)i], &R[o + (size_t)i], len);
            for (size_t t = 0; t < taps.size(); ++t) {
                const Voice* v = en->debugVoice(taps[t].first, taps[t].second);
                for (int j = 0; j < len; ++j) tapped[t].push_back(v ? v->gm[j] : 0.f);
            }
            i += len;
        }
    }
    std::vector<float> tappedTail(size_t t, double seconds) const {
        const size_t n = (size_t)(seconds * fs);
        return std::vector<float>(tapped[t].end() - (long)n, tapped[t].end());
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
// Every pipe, judged by the one judge the voicer uses (PipeJudge.hpp): alone on its groove, first strike,
// re-strike, soft and hard strikes, a long hold and the tremulant. The ranks run in parallel, one engine
// per thread.
static void everyPipe() {
    std::printf("1. every pipe of every rank: speaks promptly, right register, in tune (12-TET, a' = 440), holds, under the tremulant, dies within a semitone\n");
    std::printf("   %-26s %5s %6s %6s %7s %7s %7s %6s %6s %7s %7s %7s\n", "rank", "pipes", "fail", "first", "again", "hold c", "hold dB", "vel", "trem", "speech", "of", "let go");
    struct Row { int pipes = 0, fail = 0; double first = 0, again = 0, hold = 0, holdDb = 0, vel = 0, trem = 0, speech = 0, speechLim = 0, rel = 0; std::string bad; };
    std::vector<Row> rows((size_t)kNumRanks);
    std::atomic<int> next{0};
    const bool verbose = std::getenv("PIPE_TEST_VERBOSE") != nullptr;
    auto work = [&] {
        judge::Rig g(fs);
        for (int r; (r = next++) < kNumRanks;) {
            Row& row = rows[(size_t)r];
            const int hi = kRanks[r].division == kPedal ? kPedalHigh : kManualHigh;
            for (int key = kManualLow; key <= hi; ++key) {
                if (organ::kOrganPipe[r][key] < 0 && organ::kOrganReedPipe[r][key] < 0) continue;
                judge::Options o; o.verbose = verbose;
                const judge::Verdict v = judge::judgePipe(g, r, key, o);
                if (!v.played) continue;
                ++row.pipes;
                row.first = std::max(row.first, std::fabs(v.first)); row.again = std::max(row.again, std::fabs(v.again));
                row.hold = std::max(row.hold, v.holdWorst); row.holdDb = std::max(row.holdDb, v.holdLevel); row.vel = std::max(row.vel, v.velWorst);
                row.trem = std::max(row.trem, std::fabs(v.tremCents)); row.rel = std::max(row.rel, v.release);
                if (v.speechPeriods / v.speechLimit > row.speech / std::max(1e-9, row.speechLim)) { row.speech = v.speechPeriods; row.speechLim = v.speechLimit; }
                if (!v.pass()) { ++row.fail; char b[120]; std::snprintf(b, sizeof b, " %d:%s;", key, v.fault().c_str()); row.bad += b; }
            }
        }
    };
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    if (const char* e = std::getenv("PIPE_TEST_THREADS")) nt = (unsigned)std::max(1, std::atoi(e));
    std::vector<std::thread> t; for (unsigned i = 0; i < nt; ++i) t.emplace_back(work); for (auto& x : t) x.join();
    for (int r = 0; r < kNumRanks; ++r) {
        const Row& row = rows[(size_t)r];
        if (!row.pipes) continue;
        std::printf("   %-26s %5d %6d %6.2f %7.2f %7.2f %7.2f %6.2f %6.1f %7.0f %7.0f %7.0f\n", kRanks[r].name, row.pipes, row.fail, row.first, row.again, row.hold, row.holdDb, row.vel, row.trem, row.speech, row.speechLim, row.rel);
        if (row.fail) std::printf("      %s\n", row.bad.substr(0, 600).c_str());
        check(row.fail == 0, "%s: all %d pipes speak promptly in the right register, in tune, hold, under the tremulant, and die within a semitone", kRanks[r].name, row.pipes);
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
    const int principal = rankOf("i_octave_8");
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
    // Equal divisions, as shipped: no key-map setting is touched, so this is what a
    // player gets by choosing the tuning. Each key must be the next step, and the
    // octave must be n keys wide (not 12).
    static const struct { int sys, n; } edos[] = { { kTun19, 19 }, { kTun22, 22 }, { kTun24, 24 }, { kTun31, 31 }, { kTun41, 41 }, { kTun53, 53 },
                                                  { kTunCustomEdo, 17 }, { kTunCustomEdo, 10 }, { kTunCustomEdo, 72 } };
    for (const auto& e : edos) {
        g.reset(); g.draw(principal); g.en->setParameter(kParamTuning, (float)e.sys);
        if (e.sys == kTunCustomEdo) g.en->setParameter(kParamEdoN, (float)e.n);
        g.run(0.05);
        // middle C (key 60) plays C4; the step that serves as A (three fifths up) sits on 440 Hz
        const int fifth = (int)std::lround(e.n * std::log2(1.5));
        const int aStep = ((3 * fifth) % e.n + e.n) % e.n;
        auto expect = [&](int key, int anchor) { return 440.0 * std::pow(2.0, (double)(key - anchor - aStep) / e.n); };
        // every key of one octave for the divisions people play most, a sample for the rest
        std::vector<int> keys;
        if (e.n == 19 || e.n == 31 || e.n == 10) for (int k = 60; k <= 60 + e.n; ++k) keys.push_back(k);
        else if (60 + e.n <= 127) keys = { 48, 60, 61, 62, 60 + aStep, 60 + e.n - 1, 60 + e.n };
        else keys = { 127 - e.n, 128 - e.n, 59, 60, 61, 62, 127 };      // MIDI's 128 keys: the octave ends on key 127
        double worst = 0;
        for (int key : keys) {
            const double c = play(key, expect(key, 60));
            if (std::getenv("PIPE_TEST_VERBOSE") && std::fabs(c) > 2) std::printf("      key %d (%.1f Hz): %+.2f\n", key, expect(key, 60), c);
            worst = std::max(worst, std::fabs(c));
        }
        check(worst <= 2.0, "%d-EDO as shipped: %d keys from middle C, each the next step, octave %d keys wide (worst %.2f ct)",
              e.n, (int)keys.size(), e.n, worst);
        // the anchor moves where C4 sits
        g.en->setParameter(kParamAnchorKey, 48.f);
        const double w = std::max(std::fabs(play(48, expect(48, 48))), std::fabs(play(49, expect(49, 48))));
        check(w <= 2.0, "%d-EDO, C4 on key 48: key 48 plays C4, key 49 the step above (worst %.2f ct)", e.n, w);
        g.en->setParameter(kParamAnchorKey, 60.f);
        // the 12-key subset, when asked for: a fifth on the keyboard is the division's own fifth.
        // Divisions whose fifth cannot generate 12 notes stay sequential.
        g.en->setParameter(kParamMapLo, (float)kMapSubset);
        if (Tuning::subsetUsable(e.n)) {
            const double ws = std::max(std::fabs(play(69, 440.0)), std::fabs(play(76, 440.0 * std::pow(2.0, (double)fifth / e.n))));
            check(ws <= 2.0, "%d-EDO, 12-key subset chosen: a' = 440 and the fifth above it (worst %.2f ct)", e.n, ws);
        } else {
            const double ws = std::max(std::fabs(play(61, expect(61, 60))), std::fabs(play(62, expect(62, 60))));
            check(ws <= 2.0, "%d-EDO has no 12-note subset: subset chosen still plays every step (worst %.2f ct)", e.n, ws);
        }
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
        g.reset(); g.draw(rankOf("i_octave_8"));
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
    // a principal, a large open bass, a string and its celeste, a stopped flute, a bass reed
    for (int r : { rankOf("i_octave_8"), rankOf("p_principal_16"), rankOf("i_salicional_8"), rankOf("iii_celeste_8"), rankOf("ii_gedeckt_8"), rankOf("p_posaune_16") }) {
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
            // speaks, no louder than full, never sharp. A part-open pallet gives the pipe less wind: quieter, and a
            // flue flat. From a quarter of the travel a flue keeps within a sixth of a semitone (20 c) and a reed within
            // a few cents; below it a barely open pallet starves a large pipe (the Praestant 16' wood at 15 %: about a
            // quarter of a semitone), and a pipe on failing wind is held only to Pykett's ceiling, a semitone flat.
            const bool reed = kRanks[r].kind == kKindReed;
            const double flatLimit = pct < 25 ? -judge::kReleaseLimit : -20;
            if (lvl < full - 16 || lvl > full + 0.5 || c > 9000 || (reed ? (c > 5 || c < (pct < 25 ? flatLimit : -5)) : (c > 1.5 || c < flatLimit))) ok = false;
        }
        check(ok, "%s:%s", kRanks[r].name, line.c_str());
    }
}

// ---------------------------------------------------------------------------
static void shortNotes() {
    std::printf("5. short notes: a key let go as soon as its pipe is allowed to have spoken (the judge's speech time)\n");
    // A note held only as long as the pipe is given to speak (PipeJudge.hpp: 30 of its periods, a keen string
    // 50, the large stopped basses 50 and 80; never under 0.3 s) has come within 6 dB of its full level:
    // letting the key go in the middle of the attack does not choke it.
    Rig g;
    struct Case { int rank, key; };
    const int oct8 = rankOf("i_octave_8"), sal = rankOf("i_salicional_8"), ged = rankOf("ii_gedeckt_8"), p16 = rankOf("p_principal_16"), pos = rankOf("p_posaune_16");
    for (const Case& c : { Case{ oct8, 60 }, Case{ oct8, 48 }, Case{ oct8, 36 }, Case{ sal, 60 }, Case{ ged, 60 }, Case{ ged, 48 }, Case{ p16, 36 }, Case{ pos, 36 } }) {
        const double f = judge::pipeHz(c.rank, c.key), ms = 1000.0 * judge::speechLimitFor(c.rank, f) / f;
        g.reset(); g.draw(c.rank); g.run(0.05);
        g.send(0x90, c.key, 100); g.run(1.5); const double steady = g.tailDb(0.3);
        g.send(0x80, c.key, 64); g.run(0.6);
        const size_t t0 = g.L.size();
        g.send(0x90, c.key, 100); g.run(ms / 1000.0); g.send(0x80, c.key, 64); g.run(0.3);
        double peak = -200;
        for (size_t i = t0; i + 480 <= g.L.size(); i += 120) {
            double e = 0; for (size_t j = 0; j < 480; ++j) { const double s = 0.5 * (g.L[i + j] + g.R[i + j]); e += s * s; }
            peak = std::max(peak, 10 * std::log10(e / 480 + 1e-20));
        }
        check(peak > steady - 6, "%s key %d held %3.0f ms: reaches %.1f dB of its full level", kRanks[c.rank].name, c.key, ms, peak - steady);
    }
}

// ---------------------------------------------------------------------------
// The partial at f (searched over -150..+40 cents) in the last n samples: its offset and level.
static void strongest(const std::vector<float>& L, const std::vector<float>& R, int n, double f, double& cents, double& db) {
    double best = -1, bf = f;
    for (double c = -150; c <= 40; c += 1) {
        const double ft = f * std::pow(2.0, c / 1200.0);
        std::complex<double> z = 0;
        for (int i = 0; i < n; ++i) {
            const size_t j = L.size() - (size_t)n + (size_t)i;
            const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n);
            z += w * 0.5 * (double)(L[j] + R[j]) * std::polar(1.0, -2 * M_PI * ft * i / fs);
        }
        if (std::abs(z) > best) { best = std::abs(z); bf = ft; }
    }
    cents = 1200 * std::log2(bf / f); db = 20 * std::log10(4 * best / n + 1e-20);
}

// Known limit, guarded so it cannot get worse unnoticed: a reed in the model speaks flat
// while its tone builds from silence (about 45 cents at first; within 5 cents after about 60 periods).
// 9. Release: when a key comes up its pallet shuts and the groove under it empties through the pipes standing
// on it (Dickson's grooves: about 3/4 inch wide at the bottom, 1/4 inch at the top; the ISMA 2010 study of
// mechanical actions: the sound starts to die as the pallet closes). With the pallet shut nothing else feeds the
// groove, so it falls at rho c^2 Q / V: Q the wind its pipes draw, V its volume. Every flue rank alone, every
// sixth key: over the 2 ms after the pallet has shut the groove falls at least 0.8 of that. (A pipe that told
// the groove only its toe's conductance held the groove full: a small pipe went on speaking, 80 cents flat,
// for a second after its key was up.) And as it dies its pitch moves no more than a real pipe's, towards a
// semitone (Pykett): each flue pipe stops speaking at its own threshold (FluePipeSpec::speak), and its body rings
// down at its resonance. (Without it the jet drove the high pipes a fifth flat and into the octave: the mixtures
// "detuned when a key was lifted".) Reeds ring on their tongues when the wind has gone, their own question.
static void release() {
    std::printf("\n9. Release: the groove empties through its pipes and they stop\n");
    Rig g;
    double worst = 1e9, worstGlide = 0; int wR = -1, wK = -1, wgR = -1, wgK = -1;
    for (int r = 0; r < kNumRanks; ++r) {
        if (kRanks[r].kind == kKindReed) continue;
        const bool ped = kRanks[r].division == kPedal;
        for (int key = ped ? kPedalLow : kManualLow; key <= (ped ? kPedalHigh : kManualHigh); key += 6) {
            if (organ::kOrganPipe[r][key] < 0) continue;
            const double f = judge::pipeHz(r, key);
            g.reset(); g.taps.clear(); g.tapped.clear(); g.draw(r); g.tap(r, key);
            g.run(0.05); g.send(0x90, key, 100); g.run(std::max(1.0, 60.0 / f));
            { const Voice* v = g.en->debugVoice(r, key); if (!v || !v->groove) continue; }   // on an offset chest of its own
            const int d = kRanks[r].division;
            const size_t up = g.tapped[0].size();
            const double held = g.en->debugGroovePa(d, key);
            g.send(0x80, key, 0);
            g.run(0.025);   // the pallet shuts (20 ms)
            // every pipe of the stop on this key draws on the groove: a mixture's ranks together
            auto drawn = [&] { double q = 0; for (int rr = 0; rr < kNumRanks; ++rr) if (stopForRank(rr) == stopForRank(r)) q += g.en->debugPipeInflow(rr, key); return q; };
            const double P0 = g.en->debugGroovePa(d, key), q0 = drawn();
            g.run(0.002);
            const double P1 = g.en->debugGroovePa(d, key), q1 = drawn();
            const double expect = 1.4e5 * 0.5 * (q0 + q1) / g.en->debugGrooveVolume(d, key) * 0.002;   // Pa over the 2 ms
            const double ratio = (P0 < 0.2 * held || expect < 1e-3) ? 1.0 : (P0 - P1) / expect;   // already all but empty: it did
            if (std::getenv("PIPE_TEST_VERBOSE") && ratio < 0.8) std::printf("      %s key %d: groove %.1f -> %.1f Pa (held %.1f), drawn %.3g -> %.3g m3/s, expect %.1f Pa\n", kRanks[r].name, key, P0, P1, held, q0, q1, expect);
            if (ratio < worst) { worst = ratio; wR = r; wK = key; }
            g.run(0.3);
            const double glide = judge::releaseGlide(g.tapped[0], g.tapped[0], up, f, fs);   // the pipe's own sound
            if (glide > worstGlide) { worstGlide = glide; wgR = r; wgK = key; }
        }
    }
    check(worst >= 0.8, "with the pallet shut each groove falls as fast as its pipes draw it down (the least: %.2f of rho c^2 Q / V, %s key %d)", worst, wR >= 0 ? kRanks[wR].name : "-", wK);
    // as it dies its pitch keeps within a semitone (Pykett; PipeJudge.hpp releaseGlide), every rank on every sixth key
    check(worstGlide <= judge::kReleaseLimit, "as a pipe dies its pitch moves at most %.0f cents (%s key %d; a real pipe falls towards a semitone)", worstGlide, wgR >= 0 ? kRanks[wgR].name : "-", wgK);
}

static void reedAttack() {
    std::printf("8. reed attack from silence (known limit of the reed model)\n");
    Rig g; g.draw(rankOf("i_trumpet_8")); g.run(0.05);
    const int key = 48; const double f = et(key) * rankRatio(rankOf("i_trumpet_8"), key);
    g.send(0x90, key, 100);
    double worst = 0, settledAt = -1; const double step = 3.0 / f;
    for (double t = step; t < 2.0; t += step) {
        g.run(step);
        if (t * f < 6) continue;
        const int n = (int)(6 * fs / f);
        double c, db; strongest(g.L, g.R, n, f, c, db);
        worst = std::min(worst, c);
        if (std::fabs(c) >= 5) settledAt = -1;            // the last time it is outside 5 cents counts
        else if (settledAt < 0) settledAt = t * f;
    }
    std::printf("      Trompete C3: starts %.0f cents flat, within 5 cents from period %.0f on (6-period windows)\n", worst, settledAt);
    check(worst > -55 && settledAt > 0 && settledAt < 70, "reed attack scoop no worse than measured (start %.0f cents, settled by period %.0f)", worst, settledAt);
}

static void blowerOffOn() {
    std::printf("7. the blower stops and starts (reservoir size 1 s and blower coast-down 1 s, so the tests run quickly)\n");
    const int chord[] = { 48, 55, 60, 64 };
    const double c3 = et(48);
    struct Trace { double seconds; double flattest; double lastCents; bool silent; int jumps; double emptyAt; };
    // play the chord, stop the blower, follow C3 (the Principal's fundamental) every `step` s
    // The Principal's C3 is followed by its pipe's own sound (Rig::tap): on full organ the chord's other
    // ranks put partials of their own beside it.
    const int principal = rankOf("i_octave_8");
    auto shutdown = [&](int load, bool fullOrgan, double step, std::vector<double>* cents = nullptr) {
        Rig g; g.tap(principal, 48);
        // a short coast-down, so what is measured is the reservoir, not the blower slowing
        g.en->setParameter(kParamResLoad, (float)load); g.en->setParameter(kParamResSize, 1.f); g.en->setParameter(kParamSpinDown, 1.f);
        if (fullOrgan) { for (int st = 0; st < kNumStops; ++st) if (kStops[st].type == kStopRank || kStops[st].type == kStopMixture) g.en->setParameter(st, 1.f); }
        else g.draw(rankOf("i_octave_8"));
        g.en->setParameter(kParamZoneLoDiv, (float)kGreat);
        g.run(0.1); for (int k : chord) g.send(0x90, k, 100); g.run(1.5);
        g.en->setParameter(kParamBlowerOn, 0.f);
        Trace t{ 0, 0, 0, false, 0, -1 }; double prev = 0;
        double c, db0; strongest(g.tapped[0], g.tapped[0], 9600, c3, c, db0);
        for (int i = 0; i < 4000 && !t.silent; ++i) {
            g.run(step); t.seconds += step;
            if (t.emptyAt < 0 && g.en->outputs[kOutLid - kOutFirst] <= WindSystem::kStops) t.emptyAt = t.seconds;   // the reservoir empty: its lid down on its stops
            double db; strongest(g.tapped[0], g.tapped[0], 9600, c3, c, db);
            if (db < db0 - 50) { t.silent = true; break; }
            if (cents) cents->push_back(c);
            if (c < prev - 25) ++t.jumps;           // a sudden fall, more than the slow slide
            t.flattest = std::min(t.flattest, c); t.lastCents = c; prev = c;
        }
        return t;
    };
    std::vector<double> path;
    const Trace one = shutdown(kLoadSprings, false, 0.5, &path);
    if (std::getenv("PIPE_TEST_VERBOSE")) { std::printf("      one stop, C3 every 0.5 s (cents):"); for (double c : path) std::printf(" %.0f", c); std::printf("\n"); }
    const Trace full = shutdown(kLoadSprings, true, 0.1);
    check(one.silent && full.silent, "springs: the chord dies away on one stop (%.1f s) and on full organ (%.1f s)", one.seconds, full.seconds);
    check(one.seconds > 5 * full.seconds, "one stop lasts far longer than full organ (%.1f s against %.1f s)", one.seconds, full.seconds);
    // Springs: the pressure falls with the lid, so the pitch slides flat as the reservoir empties. One stop
    // lets it fall slowly, through the pipe's last speaking pressure; full organ empties it in a few seconds and
    // the wind collapses with the lid on its stops, the pipe still sliding.
    // How far: a real pipe sinks "towards a semitone (around 6% in frequency) before the sound ceases altogether"
    // (Pykett): a ceiling, not a minimum; the physics decides how far each goes. Both must sink (the pressure does fall).
    check(one.flattest < -3 && full.flattest < -3 && one.flattest >= -judge::kReleaseLimit && full.flattest >= -judge::kReleaseLimit,
          "on the way the pipes sink flat before they stop, no more than a semitone: one stop %.0f cents, full organ %.0f cents", one.flattest, full.flattest);
    // the slide is gradual: in the first half the pitch moves by no more than a few cents per step
    bool gradual = true; double worstStep = 0;
    for (size_t i = 1; i < path.size() / 2; ++i) { worstStep = std::max(worstStep, std::fabs(path[i] - path[i - 1])); if (std::fabs(path[i] - path[i - 1]) > 6) gradual = false; }
    check(gradual, "springs, one stop: the first half of the slide is gradual (largest move %.1f cents per half second)", worstStep);
    path.clear();
    const Trace w = shutdown(kLoadWeights, true, 0.1, &path);
    double heldUntil = 0;
    // held: against its pitch when the blower stopped (on full organ the groove's sag already sets it a little flat)
    for (size_t i = 0; i < path.size(); ++i) { if (std::fabs(path[i] - path[0]) > 3) break; heldUntil = (i + 1) * 0.1; }
    // Weights: the pressure stays while the lid falls, so the pitch holds until the reservoir is empty
    check(w.silent && w.emptyAt > 0 && heldUntil >= w.emptyAt - 0.2, "weights: pitch holds (within 3 cents for %.1f s, the reservoir empty at %.1f s), then the wind fails (silent at %.1f s)", heldUntil, w.emptyAt, w.seconds);

    // nothing played: only leakage draws on the reservoir
    {
        Rig g; g.en->setParameter(kParamResSize, 1.f); g.draw(rankOf("i_octave_8")); g.run(0.1);
        g.en->setParameter(kParamBlowerOn, 0.f); g.run(60.0);
        const float lid = g.en->outputs[kOutLid - kOutFirst];
        g.send(0x90, 60, 100); g.run(1.0);
        double c, db; strongest(g.L, g.R, 9600, et(60), c, db);
        check(lid > 0.3f && db > -40, "a minute after the blower stops with nothing played the reservoir still holds wind (lid %.2f) and a note sounds (%.1f dB, %+.1f ct)", lid, db, c);
    }

    // start-up: the blower was off when the project was saved; a chord is held and the blower started
    {
        Rig g; g.en->setParameter(kParamBlowerOn, 0.f); g.draw(rankOf("i_octave_8")); g.run(0.1);
        for (int k : chord) g.send(0x90, k, 100); g.run(1.0);
        double c, db; strongest(g.L, g.R, 9600, c3, c, db);
        check(db < -70, "blower off from the start: the held chord is silent (%.1f dB)", db);
        g.en->setParameter(kParamBlowerOn, 1.f);
        double firstHeard = -1, firstCents = 0, t = 0, inTune = -1;
        for (int i = 0; i < 60; ++i) {
            g.run(0.25); t += 0.25;
            strongest(g.L, g.R, 9600, c3, c, db);
            if (firstHeard < 0 && db > -60) { firstHeard = t; firstCents = c; }
            if (inTune < 0 && firstHeard >= 0 && std::fabs(c) < 1.5 && t > firstHeard + 0.5) inTune = t;
        }
        check(firstHeard > 0.5 && firstCents < -15, "blower started: the chord comes in after %.2f s, %.0f cents flat", firstHeard, firstCents);
        check(inTune > firstHeard && inTune < 12, "and rises into tune by %.1f s", inTune);
    }
}

static void fullChord() {
    std::printf("6. full organ, ten-note chord: every note's Principal still speaks while the wind sags\n");
    // Each note's Principal (Octave 8') is heard on its own, by its pipe's own sound, first alone and then
    // inside the full organ: every other stop drawn on every division (the reservoir feeds them all) and the
    // same ten keys held. The sound of the whole cannot be used for this: the chord's octaves and the
    // mixtures' pure fifths and thirds put other ranks' partials within a few cents of every Principal.
    const int principal = rankOf("i_octave_8");
    const int keys[] = { 36, 43, 48, 52, 55, 60, 64, 67, 72, 76 };
    const int nk = (int)(sizeof keys / sizeof keys[0]);
    double alone[10], aloneDb[10];
    for (int i = 0; i < nk; ++i) {
        Rig g; g.tap(principal, keys[i]); g.draw(principal); g.run(0.05);
        g.send(0x90, keys[i], 100); g.run(2.5);
        const auto x = g.tappedTail(0, 0.5);
        alone[i] = centsOff(x.data(), (int)x.size(), judge::pipeHz(principal, keys[i]));
        aloneDb[i] = rmsDb(x.data(), (int)x.size());
    }
    Rig g;
    for (int k : keys) g.tap(principal, k);
    for (int s = 0; s < kNumStops; ++s) if (kStops[s].type == kStopRank || kStops[s].type == kStopMixture) g.en->setParameter(s, 1.f);
    g.en->setParameter(kParamZoneLoDiv, (float)kGreat);
    g.run(0.5);
    const float chest0 = g.en->outputs[kOutChestGt - kOutFirst], res0 = g.en->outputs[kOutReservoir - kOutFirst];
    for (int k : keys) g.send(0x90, k, 100);
    g.run(2.5);
    const float chest1 = g.en->outputs[kOutChestGt - kOutFirst], res1 = g.en->outputs[kOutReservoir - kOutFirst];
    std::printf("      reservoir %.1f -> %.1f mm, Hauptwerk chest %.1f -> %.1f mm\n", res0, res1, chest0, chest1);
    double worst = 0, quieter = 0; std::string line;
    for (int i = 0; i < nk; ++i) {
        const auto x = g.tappedTail((size_t)i, 0.5);
        const double c = centsOff(x.data(), (int)x.size(), judge::pipeHz(principal, keys[i]));
        const double d = rmsDb(x.data(), (int)x.size()) - aloneDb[i];
        char b[64]; std::snprintf(b, sizeof b, " %d: %+.1f c %+.1f dB;", keys[i], c > 9000 ? 999.0 : c - alone[i], d); line += b;
        worst = std::max(worst, c > 9000 ? 999.0 : std::fabs(c - alone[i]));
        quieter = std::min(quieter, d);
    }
    std::printf("     %s\n", line.c_str());
    check(quieter > -6, "every Principal still speaks in the chord (the weakest %.1f dB against alone)", quieter);
    check(worst < 8, "the chord sags by no more than %.1f cents", worst);
    check(g.tailDb(0.5) < -3, "and it does not overload (%.1f dB RMS)", g.tailDb(0.5));
}

int main() {
    if (const char* e = std::getenv("PIPE_TEST_FS")) fs = std::max(22050, std::atoi(e));
    std::printf("sample rate %d Hz\n", fs);
    // PIPE_TEST_SECTIONS=678: only those sections (the first takes an hour on a small machine)
    const char* only = std::getenv("PIPE_TEST_SECTIONS");
    auto run = [&](char n, void (*f)()) { if (!only || std::strchr(only, n)) f(); };
    run('1', everyPipe);
    run('2', tuningSystems);
    run('3', mpeTiming);
    run('4', partTravel);
    run('5', shortNotes);
    run('6', fullChord);
    run('7', blowerOffOn);
    run('8', reedAttack);
    run('9', release);
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
