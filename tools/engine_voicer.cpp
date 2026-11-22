// Voices the organ's pipes through the engine itself: every candidate pipe is played as the plugin plays
// it (on its groove, in its chest's wind, in the organ's air, with the tuner, the key action and the
// tremulant) and judged by the pipe test's own judge (tests/PipeJudge.hpp), at 48 and 44.1 kHz. A pipe is
// accepted only when it passes at both rates.
//
//   engine_voicer survey <in.bin> <report.txt>                  judge every pipe as it is
//   engine_voicer voice  <in.bin> <out.bin> <report.txt>        cut every pipe to pitch on the engine and
//                                                               re-voice the ones that fail
//
// Environment: ONLY=<rank name part> KEYS=a-b  THREADS=n  RATES=48000,44100  CHECKPOINT=<file> (voice:
// each finished pipe is appended, so a stopped run resumes).
//
// What the voicer may change is what a voicer changes at the voicing machine. A flue pipe: the toe hole
// (its wind), a harmonic pipe's hole at half its length, the cut-up, the flue (windway) width, the languid (the jet's height against the lip: y0/b),
// the nicking and the jet's strength (the family's gain), a beard; and it is cut to pitch (the whole pipe
// scaled, the trim). A reed: the toe hole (an open boot to a bored hole), the tongue's curve (its rest
// opening), its width, the boot pressure, and the tuning (tongue and resonator, the trim). The search
// tries one change at a time, smallest first, then pairs, and keeps the first that passes: the least
// the voicer had to do.
#include "../tests/PipeJudge.hpp"
#include "engine/PipeEdit.hpp"
#include "organ_bin.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <algorithm>

using namespace pipe;

struct PipeRef { int rank, key; bool reed; int idx; };

static std::vector<int> rates() {
    std::vector<int> r;
    const char* e = std::getenv("RATES"); std::string s = e ? e : "48000,44100";
    size_t p = 0; while (p < s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); r.push_back(std::atoi(s.substr(p, q - p).c_str())); p = q + 1; }
    return r;
}

// One thread's engines, one per rate.
struct Bench {
    std::vector<std::unique_ptr<judge::Rig>> rigs;
    Bench() { for (int fs : rates()) rigs.push_back(std::make_unique<judge::Rig>(fs)); }
    void install(const PipeRef& p, const FluePipeSpec* f, const ReedPipeSpec* r) {
        for (auto& g : rigs) {
            auto set = std::make_unique<PipeEditSet>();
            if (f) set->flue[p.rank * 128 + p.key] = *f;
            if (r) set->reed[p.rank * 128 + p.key] = *r;
            g->en->setVoicingOverride(std::move(set));
        }
    }
    // every rate; with o.stopEarly the next rate is heard only when this one passes
    std::vector<judge::Verdict> judgeAll(const PipeRef& p, const judge::Options& o) {
        std::vector<judge::Verdict> v;
        for (auto& g : rigs) { v.push_back(judge::judgePipe(*g, p.rank, p.key, o)); if (o.stopEarly && !v.back().pass()) break; }
        return v;
    }
    // first-strike cents at the first rate (the tuning measurement), 9999 if wrong or silent
    double firstCents(const PipeRef& p) {
        judge::Options o; o.full = false;
        const judge::Verdict v = judge::judgePipe(*rigs[0], p.rank, p.key, o);
        if (v.silent) return 9999;
        return v.wrongFirst ? v.rawFirst : v.first;   // a pipe in the wrong register is still cut to its note; the judge then hears it
    }
};

static bool allPass(const std::vector<judge::Verdict>& v) { if (v.size() < rates().size()) return false; for (auto& x : v) if (!x.pass()) return false; return true; }
static double badness(const std::vector<judge::Verdict>& v) { double b = 0; for (auto& x : v) b = std::max(b, x.badness()); return b; }
static std::string describe(const std::vector<judge::Verdict>& v) {
    std::string s; const auto rs = rates();
    for (size_t i = 0; i < v.size(); ++i) {
        char b[300];
        std::snprintf(b, sizeof b, "%s%d: %s [level %.1f dB first %+.2f again %+.2f hold %.2f c %.1f dB vel %.2f trem %+.1f c %+.1f dB speech %.0f per]",
            i ? " | " : "", rs[i] / 1000, v[i].fault().c_str(), v[i].level, v[i].first, v[i].again, v[i].holdWorst, v[i].holdLevel, v[i].velWorst, v[i].tremCents, v[i].tremLevel, v[i].speechPeriods);
        s += b;
    }
    return s;
}

static std::vector<PipeRef> allPipes() {
    std::vector<PipeRef> out;
    const char* only = std::getenv("ONLY");
    int ka = 0, kb = 127; if (const char* ks = std::getenv("KEYS")) std::sscanf(ks, "%d-%d", &ka, &kb);
    for (int r = 0; r < kNumRanks; ++r) {
        if (only && !std::strstr(kRanks[r].name, only)) continue;
        const int hi = kRanks[r].division == kPedal ? kPedalHigh : kManualHigh;
        for (int k = kManualLow; k <= hi; ++k) {
            if (k < ka || k > kb) continue;
            if (kRanks[r].lowestKey > 0 && k < kRanks[r].lowestKey) continue;
            const int fi = organ::kOrganPipe[r][k], ri = organ::kOrganReedPipe[r][k];
            if (fi >= 0) out.push_back({ r, k, false, fi });
            else if (ri >= 0) out.push_back({ r, k, true, ri });
        }
    }
    return out;
}

static int threads() { const char* e = std::getenv("THREADS"); return e ? std::max(1, std::atoi(e)) : std::max(1u, std::thread::hardware_concurrency()); }

// ---------------------------------------------------------------- tuning on the engine
// The whole pipe scaled (its trim) until its first strike, heard through the engine, is within 0.4 cents.
// The pitch does not move exactly with the trim (the jet's speed, the boot are not scaled), so the step
// learns the slope (secant). false: it does not speak on its note at some trim tried.
template <class Spec>
static bool tune(Bench& b, const PipeRef& p, Spec& sp, std::function<void(const Spec&)> put) {
    put(sp);
    double c0 = b.firstCents(p);
    if (c0 > 9000) return false;
    double t0 = sp.trim, slope = 1.0;
    for (int it = 0; it < 6 && std::fabs(c0) > 0.4; ++it) {
        const double t1 = t0 * std::pow(2.0, -c0 / 1200.0 / slope);
        Spec s1 = sp; s1.trim = (float)t1; put(s1);
        const double c1 = b.firstCents(p);
        if (c1 > 9000) return false;
        const double dc = c1 - c0, dt = 1200.0 * std::log2(t1 / t0);
        if (std::fabs(dt) > 1e-3 && std::fabs(dc) > 1e-3) slope = std::max(0.3, std::min(2.0, dc / dt));
        sp = s1; t0 = t1; c0 = c1;
    }
    return std::fabs(c0) <= 1.0 && std::fabs(sp.trim - 1.f) < 0.08f;   // a pipe whose reading wanders by more is left to the judge
}

// ---------------------------------------------------------------- candidates
struct Change { const char* what; std::vector<std::pair<int, float>> edits; float y0b = 0, gainX = 1, beard = -1; };

static std::vector<Change> flueChanges(int rank, int key) {
    const bool string = kRanks[rank].kind == kKindString;
    std::vector<Change> one;
    // a harmonic pipe: its hole at half the length pierced wider (it damps the octave below and the 1.5x
    // resonance the doubled pipe also has), first of all
    if (const opus14::Stop* st = edit::stopFor(rank); st && key >= flue::overblowFrom(*st))
        for (float hk : { 1.2f, 1.4f, 0.85f, 1.6f, 1.8f, 0.6f }) one.push_back({ "harmonic hole", { { kEdHole, hk } } });   // an all but closed hole is first bored at a quarter of the diameter
    for (float t : { 0.9f, 1.12f, 0.8f, 1.25f, 0.7f, 1.4f }) one.push_back({ "toe", { { kEdToe, t } } });
    for (float c : { 0.95f, 1.05f, 0.9f, 1.1f, 0.85f, 1.15f }) one.push_back({ "cut-up", { { kEdCutup, c } } });
    for (float h : { 0.9f, 1.1f, 0.8f, 1.2f }) one.push_back({ "flue", { { kEdFlue, h } } });
    for (float y : { -0.1f, 0.1f, -0.2f, 0.2f }) { Change c{ "languid", {} }; c.y0b = y; one.push_back(c); }
    for (float g : { 0.8f, 1.25f, 0.63f, 1.6f }) { Change c{ "jet", {} }; c.gainX = g; one.push_back(c); }
    for (float bd : string ? std::vector<float>{ 1.0f, 0.7f, 0.5f } : std::vector<float>{ 1.0f, 0.7f }) { Change c{ "beard", {} }; c.beard = bd; one.push_back(c); }
    if (std::getenv("PASS2")) {   // the second pass, for the pipes the first could not voice: wider, and the mouth
        for (float m : { 0.92f, 1.08f, 0.85f, 1.15f }) one.push_back({ "mouth", { { kEdMouthWidth, m } } });
        for (float t : { 0.6f, 1.6f }) one.push_back({ "toe", { { kEdToe, t } } });
        for (float c : { 0.8f, 1.2f }) one.push_back({ "cut-up", { { kEdCutup, c } } });
        for (float g : { 0.5f, 2.0f }) { Change c{ "jet", {} }; c.gainX = g; one.push_back(c); }
        for (float y : { -0.3f, 0.3f }) { Change c{ "languid", {} }; c.y0b = y; one.push_back(c); }
    }
    if (std::getenv("PASS3")) {
        for (float g : { 0.4f, 2.5f, 3.2f }) { Change c{ "jet", {} }; c.gainX = g; one.push_back(c); }
        for (float h : { 0.7f, 1.3f }) one.push_back({ "flue", { { kEdFlue, h } } });
        for (float t : { 0.5f, 2.0f }) one.push_back({ "toe", { { kEdToe, t } } });
    }
    return one;
}
static std::vector<Change> reedChanges() {
    std::vector<Change> one;
    for (float t : { 1.0f, 0.8f, 0.65f, 0.5f }) one.push_back({ "toe", { { kEdToe, t } } });   // from the open boot: a bored hole, then narrower
    for (float c : { 1.08f, 0.93f, 1.16f, 0.86f, 1.25f }) one.push_back({ "curve", { { kEdTongueCurve, c } } });
    for (float w : { 0.9f, 1.1f, 0.8f }) one.push_back({ "width", { { kEdTongueWidth, w } } });
    for (float pr : { 0.92f, 1.08f }) one.push_back({ "boot", { { kEdBootPressure, pr } } });
    if (std::getenv("PASS2")) {   // the tongue against its resonator (the trim then cuts the pipe back to pitch), the scroll
        for (float l : { 0.97f, 1.03f, 0.94f, 1.06f }) one.push_back({ "tongue length", { { kEdTongueLength, l } } });
        for (float t : { 0.95f, 1.05f }) one.push_back({ "tongue thickness", { { kEdTongueThick, t } } });
        for (float r : { 0.98f, 1.02f, 0.96f, 1.04f }) one.push_back({ "resonator", { { kEdResLength, r } } });
        for (float c : { 1.35f, 0.8f }) one.push_back({ "curve", { { kEdTongueCurve, c } } });
        one.push_back({ "toe", { { kEdToe, 0.4f } } });
        for (float pr : { 0.85f, 1.15f }) one.push_back({ "boot", { { kEdBootPressure, pr } } });
    }
    if (std::getenv("PASS3")) {   // the third: the tongue brought well up towards its note (a bass reed sounding far below its tongue)
        for (float l : { 0.9f, 0.85f, 0.8f, 1.1f }) one.push_back({ "tongue length", { { kEdTongueLength, l } } });
        for (float t : { 1.1f, 1.2f, 0.9f }) one.push_back({ "tongue thickness", { { kEdTongueThick, t } } });
        one.push_back({ "toe", { { kEdToe, 0.3f } } });
    }
    if (std::getenv("PASS4")) {   // the fourth, for the top reeds: the tongue opened much further (a tongue curved so little it
                                  // barely lets wind by), more wind in the boot, the resonator shorter or longer against the tongue
        for (float c : { 1.5f, 1.7f, 2.0f }) one.push_back({ "curve", { { kEdTongueCurve, c } } });
        for (float pr : { 1.3f, 1.5f }) one.push_back({ "boot", { { kEdBootPressure, pr } } });
        for (float r : { 0.94f, 1.06f, 0.9f, 1.1f }) one.push_back({ "resonator", { { kEdResLength, r } } });
        for (float w : { 1.2f, 1.35f }) one.push_back({ "width", { { kEdTongueWidth, w } } });
    }
    return one;
}
static std::string label(const std::vector<const Change*>& cs) {
    std::string s;
    for (auto* c : cs) {
        char b[80];
        if (!c->edits.empty()) std::snprintf(b, sizeof b, "%s x%.2f", c->what, (double)c->edits[0].second);
        else if (c->beard >= 0) std::snprintf(b, sizeof b, "beard %.2f", (double)c->beard);
        else if (c->y0b != 0) std::snprintf(b, sizeof b, "languid %+.2f", (double)c->y0b);
        else std::snprintf(b, sizeof b, "jet x%.2f", (double)c->gainX);
        if (!s.empty()) s += ", ";
        s += b;
    }
    return s.empty() ? "cut to pitch" : s;
}

struct Outcome { bool ok = false; std::string how, verdict; };

// Try the pipe as it is (cut to pitch), then each single change, then pairs of the six single changes
// that came nearest to passing (of different kinds). The first that passes is kept.
template <class Spec>
static Outcome search(Bench& b, const PipeRef& p, Spec& spec, const std::vector<Change>& ch,
                      std::function<bool(const std::vector<const Change*>&, Spec&)> build) {
    auto put = [&](const Spec& s) { if constexpr (std::is_same<Spec, FluePipeSpec>::value) b.install(p, &s, nullptr); else b.install(p, nullptr, &s); };
    judge::Options quick; quick.stopEarly = true;
    Outcome res;
    double best = 1e18; std::vector<const Change*> bestCs;
    auto note = [&](const std::vector<const Change*>& cs, double bad, const std::string& what) {
        if (bad < best) { best = bad; bestCs = cs; res.how = "nearest: " + label(cs); res.verdict = what; }
    };
    auto attempt = [&](const std::vector<const Change*>& cs, double& bad) {
        Spec s = spec;
        if (!build(cs, s)) { bad = 2000; note(cs, bad, "could not be rebuilt"); return false; }
        if (!tune<Spec>(b, p, s, put)) { bad = 1500; note(cs, bad, "could not be cut to pitch"); return false; }
        put(s);
        const auto v = b.judgeAll(p, quick);
        bad = badness(v);
        if (!allPass(v)) { note(cs, bad, describe(v)); return false; }
        // (a pipe that passes is heard in full: the quick judge stops only at a fault)
        spec = s; res.ok = true; res.how = label(cs); res.verdict = describe(v);
        return true;
    };
    double bad;
    if (attempt({}, bad)) return res;
    std::vector<std::pair<double, size_t>> near;
    for (size_t i = 0; i < ch.size(); ++i) { if (attempt({ &ch[i] }, bad)) return res; near.push_back({ bad, i }); }
    std::sort(near.begin(), near.end());
    const size_t K = std::min<size_t>(6, near.size());
    for (size_t a = 0; a < K; ++a) for (size_t c = 0; c < ch.size(); ++c) {
        const size_t i = near[a].second;
        if (c == i || std::strcmp(ch[i].what, ch[c].what) == 0) continue;
        if (attempt({ &ch[i], &ch[c] }, bad)) return res;
    }
    // then the nearest of all with a third change of another kind (from the nearest singles)
    if (bestCs.size() == 2) {
        const auto base = bestCs;
        for (size_t a = 0; a < K; ++a) {
            const Change* x = &ch[near[a].second];
            bool same = false; for (auto* y : base) same |= std::strcmp(x->what, y->what) == 0;
            if (same) continue;
            if (attempt({ base[0], base[1], x }, bad)) return res;
        }
    }
    return res;
}

static Outcome voiceFlue(Bench& b, const PipeRef& p, FluePipeSpec& spec) {
    const FluePipeSpec orig = spec;
    return search<FluePipeSpec>(b, p, spec, flueChanges(p.rank, p.key), [&](const std::vector<const Change*>& cs, FluePipeSpec& out) {
        FluePipeSpec base = orig;
        PipeEdit e; for (auto* c : cs) for (auto& ed : c->edits) e.v[ed.first] *= ed.second;
        float beard = -1; for (auto* c : cs) { base.y0b += c->y0b; base.gscale *= c->gainX; if (c->beard >= 0) beard = c->beard; }
        if (beard >= 0) e.v[kEdBeard] = beard;
        out = base; std::string why;
        return !e.any() || edit::editFlue(p.rank, p.key, base, e, out, why);
    });
}
static Outcome voiceReed(Bench& b, const PipeRef& p, ReedPipeSpec& spec) {
    const ReedPipeSpec orig = spec;
    return search<ReedPipeSpec>(b, p, spec, reedChanges(), [&](const std::vector<const Change*>& cs, ReedPipeSpec& out) {
        PipeEdit e; for (auto* c : cs) for (auto& ed : c->edits) e.v[ed.first] *= ed.second;
        out = orig; std::string why;
        return !e.any() || edit::editReed(p.rank, p.key, orig, e, out, why);
    });
}

int main(int argc, char** argv) {
    setenv("OPUSXIV_THREADS", "1", 1);   // one engine per thread, each on its own core
    if (argc < 4) { std::fprintf(stderr, "usage: engine_voicer survey in.bin report | voice in.bin out.bin report\n"); return 1; }
    const std::string mode = argv[1];
    OrganBin bin; std::string why;
    if (!bin.read(argv[2], why)) { std::fprintf(stderr, "%s\n", why.c_str()); return 1; }
    const auto pipes = allPipes();
    std::fprintf(stderr, "%zu pipes, %d threads, rates", pipes.size(), threads()); for (int r : rates()) std::fprintf(stderr, " %d", r); std::fprintf(stderr, "\n");
    std::atomic<size_t> next{0}, done{0}, bad{0};
    std::mutex io;

    if (mode == "survey") {
        FILE* rep = std::fopen(argv[3], "w");
        auto work = [&] {
            Bench b;
            for (size_t i; (i = next++) < pipes.size();) {
                const PipeRef& p = pipes[i];
                if (p.reed) b.install(p, nullptr, &bin.reeds[(size_t)p.idx]); else b.install(p, &bin.flues[(size_t)p.idx], nullptr);
                judge::Options o; const auto v = b.judgeAll(p, o);
                const bool ok = allPass(v); if (!ok) ++bad;
                std::lock_guard<std::mutex> lk(io);
                std::fprintf(rep, "%s\t%d\t%s\t%s\n", kRanks[p.rank].name, p.key, ok ? "ok" : "FAIL", describe(v).c_str()); std::fflush(rep);
                const size_t d = ++done; if (d % 50 == 0) std::fprintf(stderr, "  %zu / %zu judged, %zu fail\n", d, pipes.size(), bad.load());
            }
        };
        std::vector<std::thread> t; for (int i = 0; i < threads(); ++i) t.emplace_back(work); for (auto& x : t) x.join();
        std::fclose(rep);
        std::fprintf(stderr, "survey: %zu of %zu pipes fail\n", bad.load(), pipes.size());
        return 0;
    }
    if (mode == "voice" && argc >= 5) {
        // checkpoint: "rank key ok how" lines and the spec bytes in a side file
        std::map<std::pair<int, int>, bool> doneSet;
        const char* ck = std::getenv("CHECKPOINT");
        if (ck) {
            if (FILE* f = std::fopen(ck, "rb")) {
                for (;;) {
                    int r, k, ok, reed; if (std::fread(&r, 4, 1, f) != 1 || std::fread(&k, 4, 1, f) != 1 || std::fread(&ok, 4, 1, f) != 1 || std::fread(&reed, 4, 1, f) != 1) break;
                    if (reed) { ReedPipeSpec s; if (std::fread(&s, sizeof s, 1, f) != 1) break; const int i = organ::kOrganReedPipe[r][k]; if (i >= 0 && ok) bin.reeds[(size_t)i] = s; }
                    else { FluePipeSpec s; if (std::fread(&s, sizeof s, 1, f) != 1) break; const int i = organ::kOrganPipe[r][k]; if (i >= 0 && ok) bin.flues[(size_t)i] = s; }
                    if (ok || !std::getenv("REDO_FAILED")) doneSet[{ r, k }] = ok != 0; else doneSet.erase({ r, k });
                }
                std::fclose(f);
                std::fprintf(stderr, "checkpoint: %zu pipes already voiced\n", doneSet.size());
            }
        }
        FILE* ckF = ck ? std::fopen(ck, "ab") : nullptr;
        FILE* rep = std::fopen(argv[4], "a");
        auto work = [&] {
            Bench b;
            for (size_t i; (i = next++) < pipes.size();) {
                const PipeRef& p = pipes[i];
                if (doneSet.count({ p.rank, p.key })) continue;
                Outcome o;
                FluePipeSpec fs; ReedPipeSpec rs;
                if (p.reed) { rs = bin.reeds[(size_t)p.idx]; o = voiceReed(b, p, rs); }
                else { fs = bin.flues[(size_t)p.idx]; o = voiceFlue(b, p, fs); }
                std::lock_guard<std::mutex> lk(io);
                if (o.ok) { if (p.reed) bin.reeds[(size_t)p.idx] = rs; else bin.flues[(size_t)p.idx] = fs; } else ++bad;
                if (ckF) { const int r = p.rank, k = p.key, ok = o.ok, reed = p.reed; std::fwrite(&r, 4, 1, ckF); std::fwrite(&k, 4, 1, ckF); std::fwrite(&ok, 4, 1, ckF); std::fwrite(&reed, 4, 1, ckF);
                    if (p.reed) std::fwrite(&rs, sizeof rs, 1, ckF); else std::fwrite(&fs, sizeof fs, 1, ckF); std::fflush(ckF); }
                std::fprintf(rep, "%s\t%d\t%s\t%s\t%s\n", kRanks[p.rank].name, p.key, o.ok ? "voiced" : "FAILED", o.how.c_str(), o.verdict.c_str()); std::fflush(rep);
                const size_t d = ++done; if (d % 20 == 0) std::fprintf(stderr, "  %zu / %zu voiced, %zu could not be\n", d, pipes.size(), bad.load());
            }
        };
        std::vector<std::thread> t; for (int i = 0; i < threads(); ++i) t.emplace_back(work); for (auto& x : t) x.join();
        if (ckF) std::fclose(ckF);
        std::fclose(rep);
        if (!bin.write(argv[3])) { std::fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        std::fprintf(stderr, "voiced: %zu of %zu pipes could not be voiced\n", bad.load(), pipes.size());
        return 0;
    }
    std::fprintf(stderr, "unknown mode\n");
    return 1;
}
