// Builds the playable Opus 14: every flue pipe of every rank designed from the Pasi scales
// (flue::designAt), turned into the real-time description (FluePipeSpec), cut to pitch on the
// real-time model (render, measure, recut the body, until within 0.5 cent), and checked that it
// speaks on the mode it should. Pipes are cut at equal temperament, a1 = 440 Hz; the engine
// applies the temperament (a fraction of a semitone: the pipe is scaled, see FluePipeRT::setPitch).
//
//   opus14_organ <constants.txt> <reed_constants.txt> <out.bin> <out.hpp> [report.txt]
//
// constants.txt: one line per jet family "family y0b lpk gscale kd noise" (matched to references).
// out.bin: header "P14\0", u32 version, u32 count, then count FluePipeSpec records.
// out.hpp: the stop list, ranks and per-key pipe index (generated; do not edit).
#include "engine/FlueModel.hpp"
#include "engine/ReedModel.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <sstream>
using namespace pipe; using namespace pipe::flue;

// -- the stop list as published (V1 p. 253, P), divisions numbered as in the engine
enum EDiv { ePed = 0, eI = 1, eIII = 2, eII = 3 };   // I = Hauptwerk, III = Unterwerk (enclosed), II = Oberwerk
struct OrganStop { EDiv div; const char* label; const char* foot; std::vector<std::string> ranks; bool mixture; const char* mixName; float detuneCents; int lowest; };
// rank names refer to opus14::kStops entries (division-qualified below); mixture stops list the scale stop
static const std::vector<OrganStop> kList = {
    // Hauptwerk (I)
    { eI, "PRAESTANT", "16'", { "Praestant 16'" }, false, nullptr, 0, 36 },
    { eI, "OCTAVE", "8'", { "Octave 8'" }, false, nullptr, 0, 36 },
    { eI, "ROHRFLOTE", "8'", { "Rohrflote 8'" }, false, nullptr, 0, 36 },
    { eI, "SALICIONAL", "8'", { "Salicional 8'" }, false, nullptr, 0, 36 },
    { eI, "OCTAVE", "4'", { "Octave 4'" }, false, nullptr, 0, 36 },
    { eI, "SPITZFLOTE", "4'", { "Spitzflote 4'" }, false, nullptr, 0, 36 },
    { eI, "QUINTE", "2-2/3'", { "Quinte 2 2/3'" }, false, nullptr, 0, 36 },
    { eI, "SUPEROCTAVE", "2'", { "Superoctave 2'" }, false, nullptr, 0, 36 },
    { eI, "MIXTURE V", "1-1/3'", { "Mixture 1 1/3' V" }, true, "Mixture 1 1/3' V", 0, 36 },
    { eI, "MIXTURE IV", "1'", { "Mixture 1' IV" }, true, "Mixture 1' IV", 0, 36 },
    { eI, "CORNET V", "8'", { "Cornet V 8' rank", "Cornet V 4' rank", "Cornet V 2 2/3' rank", "Cornet V 2' rank", "Cornet V 1 3/5' rank" }, false, nullptr, 0, 60 },
    { eI, "TRUMPET", "16'", { "Trumpet 16'" }, false, nullptr, 0, 36 },
    { eI, "TRUMPET", "8'", { "Trumpet 8'" }, false, nullptr, 0, 36 },
    { eI, "TROMPETTE", "8'", { "Trompette 8'" }, false, nullptr, 0, 36 },
    { eI, "VOX HUMANA", "8'", { "Vox Humana 8'" }, false, nullptr, 0, 36 },
    // Oberwerk (II)
    { eII, "PRINCIPAL", "8'", { "Praestant 8'" }, false, nullptr, 0, 36 },
    { eII, "SUAVIAL", "8'", { "Suavial 8'" }, false, nullptr, 3.0f, 36 },     // undulating: tuned sharp of the Principal (ASSUMPTION 3 c)
    { eII, "GEDECKT", "8'", { "Gedeckt 8'" }, false, nullptr, 0, 36 },
    { eII, "OCTAVE", "4'", { "Octave 4'" }, false, nullptr, 0, 36 },
    { eII, "ROHRFLOTE", "4'", { "Rohrflote 4'" }, false, nullptr, 0, 36 },
    { eII, "SESQUIALTERA II", "2-2/3'", { "Sesquialtera II 2 2/3' rank", "Sesquialtera II 1 3/5' rank" }, false, nullptr, 0, 36 },
    { eII, "OCTAVE", "2'", { "Octave 2'" }, false, nullptr, 0, 36 },
    { eII, "WALDFLOTE", "2'", { "Waldflote 2'" }, false, nullptr, 0, 36 },
    { eII, "QUINTE", "1-1/3'", { "Quinte 1 1/3'" }, false, nullptr, 0, 36 },
    { eII, "MIXTURE IV", "1'", { "Mixture 1' IV" }, true, "Mixture 1' IV", 0, 36 },
    { eII, "DULZIAN", "16'", { "Dulzian 16'" }, false, nullptr, 0, 36 },
    { eII, "TRICHTERREGAL", "8'", { "Trichterregal 8'" }, false, nullptr, 0, 36 },
    // Unterwerk (III, enclosed)
    { eIII, "BOURDON", "16'", { "Bourdon 16'" }, false, nullptr, 0, 36 },
    { eIII, "PRINCIPAL", "8'", { "Principal 8'" }, false, nullptr, 0, 36 },
    { eIII, "GAMBA", "8'", { "Gamba 8'" }, false, nullptr, 0, 36 },
    { eIII, "CELESTE", "8'", { "Celeste 8'" }, false, nullptr, 5.0f, 48 },     // tuned sharp (ASSUMPTION 5 c), from c0 (ASSUMPTION)
    { eIII, "FLUTE HARMONIQUE", "8'", { "Harmonic Flute 8'" }, false, nullptr, 0, 36 },
    { eIII, "PRINCIPAL", "4'", { "Principal 4'" }, false, nullptr, 0, 36 },
    { eIII, "FLUTE HARMONIQUE", "4'", { "Harmonic Flute 4'" }, false, nullptr, 0, 36 },
    { eIII, "NAZARD", "2-2/3'", { "Nazard 2 2/3'" }, false, nullptr, 0, 36 },
    { eIII, "OCTAVIN", "2'", { "Octavin 2'" }, false, nullptr, 0, 36 },
    { eIII, "TIERCE", "1-3/5'", { "Tierce 1 3/5'" }, false, nullptr, 0, 36 },
    { eIII, "MIXTURE V", "2'", { "Mixture V 2'" }, true, "Mixture 2' V", 0, 36 },
    { eIII, "BASSOON", "16'", { "Bassoon 16'" }, false, nullptr, 0, 36 },
    { eIII, "TROMPETTE", "8'", { "Trompette 8'" }, false, nullptr, 0, 36 },
    { eIII, "OBOE", "8'", { "Oboe 8'" }, false, nullptr, 0, 36 },
    { eIII, "CLAIRON", "4'", { "Clairon 4'" }, false, nullptr, 0, 36 },
    // Pedal
    { ePed, "SUBBASS", "32'", { "Subbass 32'" }, false, nullptr, 0, 36 },
    { ePed, "PRINCIPAL", "16'", { "Praestant 16' (wood Haskell)" }, false, nullptr, 0, 36 },
    { ePed, "SUBBASS", "16'", { "Subbass 16'" }, false, nullptr, 0, 36 },
    { ePed, "OCTAVE", "8'", { "Octave 8'" }, false, nullptr, 0, 36 },
    { ePed, "BOURDON", "8'", { "Gedeckt 8'" }, false, nullptr, 0, 36 },
    { ePed, "OCTAVE", "4'", { "Octave 4'" }, false, nullptr, 0, 36 },
    { ePed, "MIXTURE V", "2-2/3'", { "Mixture V 2 2/3'" }, true, "Mixture 2 2/3' V", 0, 36 },
    { ePed, "TROMBONE", "32'", { "Trombone 32'" }, false, nullptr, 0, 36 },
    { ePed, "POSAUNE", "16'", { "Posaune 16'" }, false, nullptr, 0, 36 },
    { ePed, "TRUMPET", "8'", { "Trumpet 8'" }, false, nullptr, 0, 36 },
    { ePed, "TROMPETTE", "8'", { "Trompette 8'" }, false, nullptr, 0, 36 },
    { ePed, "CLAIRON", "4'", { "Clairon 4'" }, false, nullptr, 0, 36 },
    { ePed, "CORNET", "2'", { "Cornet 2'" }, false, nullptr, 0, 36 },
};
static opus14::Division dataDiv(EDiv d) { return d == ePed ? opus14::kPed : d == eI ? opus14::kHW : d == eII ? opus14::kOW : opus14::kUW; }
static const opus14::Stop* findStop(EDiv d, const std::string& name) {
    for (int i = 0; i < opus14::kNumStops; ++i) if (opus14::kStops[i].div == dataDiv(d) && name == opus14::kStops[i].name) return &opus14::kStops[i];
    return nullptr;
}
static const opus14::Mixture* findMix(EDiv d, const char* name) {
    for (int i = 0; i < opus14::kNumMixtures; ++i) if (opus14::kMixtures[i].div == dataDiv(d) && !std::strcmp(name, opus14::kMixtures[i].name)) return &opus14::kMixtures[i];
    return nullptr;
}

// jet families (constants matched to references; see data/opus14/jet_constants.txt)
enum JetFam { jPrincipal = 0, jString, jOpenFlute, jStopped, jChimney, jHarmonic, kNumJetFam };
static const char* kJetFamNames[kNumJetFam] = { "principal", "string", "openflute", "stopped", "chimney", "harmonic" };
static JetFam jetFamily(const opus14::Stop& s, const Design& d) {
    if (d.speakMode == 2) return jHarmonic;
    if (d.family == kString) return jString;
    if (d.shape == kStopped) return jStopped;
    if (d.shape == kChimney) return jChimney;
    if (d.family == kFlute || d.shape == kConical) return jOpenFlute;
    return jPrincipal;
}

static bool detailOboe(const std::string& n) {
    for (const char* o : { "Oboe", "Vox", "Regal", "Dulzian", "Bassoon" }) if (n.find(o) != std::string::npos) return true;
    return false;
}
static double etHz(int key, double ratio) { return 440.0 * std::pow(2.0, (key - 69) / 12.0) * ratio; }

struct Job { int rank; int key; const opus14::Stop* s; double scaleKey; double hz; };
struct Result { FluePipeSpec spec; bool ok = false; double cents0 = 0, centsEnd = 0; int fam = 0; std::string why; };

// sounding frequency of the RT pipe: peak of mouth + top near the target
static double soundFreq(const FluePipeSpec& sp, double& h1, double& hHalf, double& h2) {
    FluePipeRT p; p.init(sp, 48000, 7); p.setChestPa(kChestPa); p.key(true);
    const double T = std::max(1.2, 0.25 + 120.0 / sp.target);
    const int N = (int)(T * 48000), L = std::min(N / 2, 48000);
    std::vector<double> x(N); std::vector<float> m(1), t(1);
    for (int i = 0; i < N; ++i) { p.render(m.data(), t.data(), 1); x[i] = m[0] + t[0]; }
    const double f = peakFreq(x, N - L, L, sp.target, 0.06, 48000);
    h1 = toneAt(x, N - L, L, f, 48000); hHalf = toneAt(x, N - L, L, f / 2, 48000); h2 = toneAt(x, N - L, L, 2 * f, 48000);
    return f;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: opus14_organ constants.txt reed_constants.txt out.bin out.hpp [report]\n"); return 1; }
    // reed constants: "family widthRatio openRatio Qr bootLitres8"
    reed::ReedConstants reedK[2];
    { std::ifstream in(argv[2]); std::string line;
      while (std::getline(in, line)) { if (line.empty() || line[0] == '#') continue; std::istringstream ss(line); std::string n; double a, b, c, d, e = 0.2;
        if (!(ss >> n >> a >> b >> c >> d)) continue;
        ss >> e;
        const int i = n == "oboe" ? 1 : 0; reedK[i].widthRatio = a; reedK[i].openRatio = b; reedK[i].Qr = c; reedK[i].bootLitres8 = d; reedK[i].closure = e; } }
    char** outArgs = argv + 1;   // outArgs[2] = bin, [3] = hpp, [4] = report
    // constants
    JetParams fam[kNumJetFam];
    for (int i = 0; i < kNumJetFam; ++i) fam[i] = matchedPrincipal();
    { std::ifstream in(argv[1]); std::string line;
      while (std::getline(in, line)) { if (line.empty() || line[0] == '#') continue; std::istringstream ss(line); std::string n; double a, b, c, d, e, vx = 1.0;
        if (!(ss >> n >> a >> b >> c >> d >> e)) continue;
        ss >> vx;
        for (int i = 0; i < kNumJetFam; ++i) if (n == kJetFamNames[i]) { fam[i].y0b = a; fam[i].lpk = b; fam[i].gain = -c; fam[i].kd = d; fam[i].noise = e; fam[i].vortex = vx; } } }

    // ranks
    struct RankOut { int stop; std::string name; int div; bool reed; int mixRow; float detune; int lowest; const opus14::Stop* s; const opus14::Mixture* mix; };
    std::vector<RankOut> ranks;
    for (size_t si = 0; si < kList.size(); ++si) {
        const auto& os = kList[si];
        if (os.mixture) {
            const opus14::Mixture* m = findMix(os.div, os.mixName);
            const opus14::Stop* s = findStop(os.div, os.ranks[0]);
            if (!m || !s) { fprintf(stderr, "missing mixture %s\n", os.mixName); return 1; }
            int rows = 0; for (int b = 0; b < m->nBreaks; ++b) rows = std::max(rows, m->breaks[b].nRanks);
            for (int r = 0; r < rows; ++r) ranks.push_back({ (int)si, std::string(os.label) + " " + std::to_string(r + 1), os.div, false, r, os.detuneCents, os.lowest, s, m });
        } else for (const auto& rn : os.ranks) {
            const opus14::Stop* s = findStop(os.div, rn);
            if (!s) { fprintf(stderr, "missing stop %s\n", rn.c_str()); return 1; }
            // a rank's first pipe: the stop's lowest key, or its first printed column if later (cornet)
            ranks.push_back({ (int)si, rn, os.div, s->kind == opus14::kReed, -1, os.detuneCents, std::max(os.lowest, s->key[0]), s, nullptr });
        }
    }
    const int kLo = 36;
    auto compassHigh = [&](int div) { return div == ePed ? 65 : 93; };
    // per rank per key: pipe ratio to 8' and job
    std::vector<Job> jobs;
    std::vector<std::vector<int>> pipeIdx(ranks.size(), std::vector<int>(128, -1));
    std::vector<std::vector<float>> ratio(ranks.size(), std::vector<float>(128, 0.f));
    std::vector<std::vector<int>> reedIdx(ranks.size(), std::vector<int>(128, -1));
    std::vector<Job> reedJobs;
    for (size_t r = 0; r < ranks.size(); ++r) {
        const auto& R = ranks[r];
        for (int k = R.lowest; k <= compassHigh(R.div); ++k) {
            double feet = R.s->feet, scaleKey = k;
            if (R.mix) {
                int b = 0; while (b + 1 < R.mix->nBreaks && R.mix->breaks[b + 1].key <= k) ++b;
                if (R.mixRow >= R.mix->breaks[b].nRanks) continue;
                feet = R.mix->breaks[b].feet[R.mixRow];
                // scales printed for the base stop (2' or 2 2/3'): the place on that scale with this pipe's pitch
                scaleKey = k + 12.0 * std::log2(R.s->feet == 2.6666666666666665f ? 2.6666666666666665 / feet : 2.0 / feet);
                if (std::strstr(R.s->note, "2 2/3")) scaleKey = k + 12.0 * std::log2(2.6666666666666665 / feet);
                else scaleKey = k + 12.0 * std::log2(2.0 / feet);
            }
            ratio[r][k] = (float)(8.0 / feet);
            if (R.reed) { reedIdx[r][k] = (int)reedJobs.size(); reedJobs.push_back({ (int)r, k, R.s, scaleKey, etHz(k, 8.0 / feet) }); continue; }
            pipeIdx[r][k] = (int)jobs.size();
            jobs.push_back({ (int)r, k, R.s, scaleKey, etHz(k, 8.0 / feet) });
        }
    }
    fprintf(stderr, "%zu ranks, %zu flue pipes\n", ranks.size(), jobs.size());

    std::vector<Result> res(jobs.size());
    std::atomic<size_t> next{0}; std::mutex io;
    if (const char* reuse = getenv("REUSE_FLUES")) {
        FILE* f = fopen(reuse, "rb"); char magic[4]; uint32_t ver = 0, n = 0, sz = 0;
        if (f && fread(magic, 1, 4, f) == 4 && fread(&ver, 4, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && fread(&sz, 4, 1, f) == 1
            && ver == 2 && n == jobs.size() && sz == sizeof(FluePipeSpec)) {
            for (auto& r : res) { if (fread(&r.spec, sizeof(FluePipeSpec), 1, f) != 1) break; r.ok = true; r.fam = 0; }
            next = jobs.size();
            fprintf(stderr, "flue pipes reused from %s\n", reuse);
        } else fprintf(stderr, "cannot reuse %s: computing the flue pipes\n", reuse);
        if (f) fclose(f);
    }
    auto worker = [&] {
        for (;;) {
            const size_t j = next++;
            if (j >= jobs.size()) return;
            const Job& jb = jobs[j]; Result& out = res[j];
            Voicing v; v.ising = familyOf(*jb.s) == kString ? 2.8 : 2.0;
            Design d = designAt(*jb.s, jb.scaleKey, jb.hz, v);
            const JetFam jf = jetFamily(*jb.s, d); out.fam = jf;
            for (int it = 0; it < 10; ++it) {
                out.spec = specOf(d, fam[jf]);
                double h1, hh, h2; const double f = soundFreq(out.spec, h1, hh, h2);
                const double c = 1200 * std::log2(f / jb.hz);
                if (it == 0) out.cents0 = c;
                out.centsEnd = c;
                out.ok = h1 > 0 && h1 > hh && std::fabs(c) < 50;
                if (!out.ok) { out.why = "f " + std::to_string(f) + " target " + std::to_string(jb.hz); if (std::fabs(c) > 300) break; }
                if (std::fabs(c) < 0.5) break;
                recut(d, std::pow(f / jb.hz, 1.5));   // the jet pulls the pitch: a longer step converges faster
            }
            out.spec.target = (float)jb.hz;
            if ((j + 1) % 200 == 0) { std::lock_guard<std::mutex> lk(io); fprintf(stderr, "  %zu / %zu\n", j + 1, jobs.size()); }
        }
    };
    std::vector<std::thread> th; const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < nt; ++i) th.emplace_back(worker);
    for (auto& t : th) t.join();
    // reeds: designed from the Pasi resonators and tongues, tuned by wire and scroll
    struct ReedResult { ReedPipeSpec spec; double cents = 0; bool ok = false; };
    std::vector<ReedResult> reedRes(reedJobs.size());
    std::atomic<size_t> nextR{0};
    auto reedHz = [](const ReedPipeSpec& sp) {
        ReedPipeRT p; p.init(sp, 48000, 3); p.setChestPa(kChestPa); p.key(true);
        const int N = (int)(48000 * std::max(0.8, 0.3 + 60.0 / sp.target)), L = N / 2;
        std::vector<double> x(N); float o;
        for (int i = 0; i < N; ++i) { p.render(&o, 1); x[i] = o; }
        double e = 0; for (int i = N - L; i < N; ++i) e += x[i] * x[i];
        if (!(e / L > 1e-8)) return 0.0;
        const double f = peakFreq(x, N - L, L, sp.target, 0.15, 48000);
        return toneAt(x, N - L, L, f, 48000) > toneAt(x, N - L, L, f / 2, 48000) ? f : 0.0;
    };
    auto reedWorker = [&] {
        for (;;) {
            const size_t j = nextR++;
            if (j >= reedJobs.size()) return;
            const Job& jb = reedJobs[j];
            const bool oboe = detailOboe(ranks[(size_t)jb.rank].name);
            ReedResult& out = reedRes[j];
            out.spec = reed::design(*jb.s, jb.scaleKey, jb.hz, reedK[oboe ? 1 : 0]);
            out.cents = reed::tune(out.spec, reedHz);
            out.ok = std::fabs(out.cents) < 5;
            if ((j + 1) % 100 == 0) { std::lock_guard<std::mutex> lk(io); fprintf(stderr, "  reeds %zu / %zu\n", j + 1, reedJobs.size()); }
        }
    };
    th.clear();
    for (unsigned i = 0; i < nt; ++i) th.emplace_back(reedWorker);
    for (auto& t : th) t.join();

    // blob
    { FILE* f = fopen(outArgs[2], "wb"); const uint32_t ver = 2, n = (uint32_t)res.size(), sz = sizeof(FluePipeSpec);
      fwrite("P14", 1, 4, f); fwrite(&ver, 4, 1, f); fwrite(&n, 4, 1, f); fwrite(&sz, 4, 1, f);
      for (auto& r : res) fwrite(&r.spec, sizeof(FluePipeSpec), 1, f);
      // reed section: count, record size, then the records (16-byte aligned)
      const uint32_t nr = (uint32_t)reedRes.size(), szr = sizeof(ReedPipeSpec);
      long pos = ftell(f); while (pos % 16 != 8) { fputc(0, f); ++pos; }
      fwrite(&nr, 4, 1, f); fwrite(&szr, 4, 1, f);
      for (auto& r : reedRes) fwrite(&r.spec, sizeof(ReedPipeSpec), 1, f);
      fclose(f); }
    // header
    { FILE* f = fopen(outArgs[3], "w");
      fprintf(f, "// Generated by tools/opus14_organ.cpp from the Pasi Opus 14 data - do not edit.\n#pragma once\nnamespace pipe { namespace organ {\n");
      fprintf(f, "enum { kNumOrganStops = %zu, kNumOrganRanks = %zu, kNumFluePipes = %zu, kNumReedPipes = %zu };\n", kList.size(), ranks.size(), res.size(), reedRes.size());
      fprintf(f, "struct OrganStopDef { const char* id; const char* label; const char* foot; int division; int rankFirst, rankCount; bool reed; };\n");
      fprintf(f, "static const OrganStopDef kOrganStops[kNumOrganStops] = {\n");
      for (size_t si = 0; si < kList.size(); ++si) {
          int first = -1, cnt = 0; bool reed = false;
          for (size_t r = 0; r < ranks.size(); ++r) if (ranks[r].stop == (int)si) { if (first < 0) first = (int)r; ++cnt; reed = ranks[r].reed; }
          std::string id = std::string(kList[si].div == ePed ? "p_" : kList[si].div == eI ? "i_" : kList[si].div == eII ? "ii_" : "iii_");
          for (const char* c = kList[si].label; *c; ++c) id += (*c == ' ') ? '_' : (char)std::tolower(*c);
          id += "_"; for (const char* c = kList[si].foot; *c; ++c) if (std::isalnum((unsigned char)*c)) id += *c;
          fprintf(f, "    { \"%s\", \"%s\", \"%s\", %d, %d, %d, %s },\n", id.c_str(), kList[si].label, kList[si].foot, (int)kList[si].div, first, cnt, reed ? "true" : "false");
      }
      fprintf(f, "};\nstruct OrganRankDef { const char* name; int division; int stop; bool reed; int jetFamily; float detuneCents; int lowestKey; const char* dataStop; };\n");
      fprintf(f, "static const OrganRankDef kOrganRanks[kNumOrganRanks] = {\n");
      for (size_t r = 0; r < ranks.size(); ++r) {
          int jf = -1; for (int k = 0; k < 128; ++k) if (pipeIdx[r][k] >= 0) { jf = res[(size_t)pipeIdx[r][k]].fam; break; }
          fprintf(f, "    { \"%s\", %d, %d, %s, %d, %.2ff, %d, \"%s\" },\n", ranks[r].name.c_str(), ranks[r].div, ranks[r].stop, ranks[r].reed ? "true" : "false", jf, ranks[r].detune, ranks[r].lowest, ranks[r].s->name);
      }
      fprintf(f, "};\n// pipe index into the flue blob per rank and key (-1: no flue pipe), and the pipe's frequency ratio to 8' (0: no pipe)\n");
      fprintf(f, "static const short kOrganPipe[kNumOrganRanks][128] = {\n");
      for (size_t r = 0; r < ranks.size(); ++r) { fprintf(f, "    {"); for (int k = 0; k < 128; ++k) fprintf(f, "%d%s", pipeIdx[r][k], k < 127 ? "," : ""); fprintf(f, "},\n"); }
      fprintf(f, "};\n// reed pipe index into the reed section per rank and key (-1: none)\nstatic const short kOrganReedPipe[kNumOrganRanks][128] = {\n");
      for (size_t r = 0; r < ranks.size(); ++r) { fprintf(f, "    {"); for (int k = 0; k < 128; ++k) fprintf(f, "%d%s", reedIdx[r][k], k < 127 ? "," : ""); fprintf(f, "},\n"); }
      fprintf(f, "};\nstatic const float kOrganRatio[kNumOrganRanks][128] = {\n");
      for (size_t r = 0; r < ranks.size(); ++r) { fprintf(f, "    {"); for (int k = 0; k < 128; ++k) { char num[32]; snprintf(num, sizeof num, "%.7g", ratio[r][k]); if (!std::strpbrk(num, ".e")) std::strcat(num, ".0"); fprintf(f, "%sf%s", num, k < 127 ? "," : ""); } fprintf(f, "},\n"); }
      fprintf(f, "};\n}} // namespace pipe::organ\n"); fclose(f); }
    // report
    int bad = 0;
    FILE* rep = argc > 5 ? fopen(outArgs[4], "w") : stderr;
    for (size_t j = 0; j < jobs.size(); ++j) {
        const auto& r = res[j];
        if (!r.ok) ++bad;
        fprintf(rep, "%-32s key %3d %8.2f Hz fam %-9s first %+7.1f c  final %+6.2f c %s %s\n", ranks[(size_t)jobs[j].rank].name.c_str(), jobs[j].key, jobs[j].hz, kJetFamNames[r.fam], r.cents0, r.centsEnd, r.ok ? "ok" : "FAIL", r.why.c_str());
    }
    fprintf(stderr, "%d of %zu flue pipes failed to speak on their mode\n", bad, jobs.size());
    int badR = 0;
    for (size_t j = 0; j < reedJobs.size(); ++j) {
        const auto& r = reedRes[j]; if (!r.ok) ++badR;
        fprintf(rep, "%-32s key %3d %8.2f Hz reed tongue %.2f mm L %.1f mm fr %.1f Hz  final %+7.2f c %s\n", ranks[(size_t)reedJobs[j].rank].name.c_str(), reedJobs[j].key, reedJobs[j].hz, r.spec.t * 1e3, r.spec.L * 1e3, r.spec.fr, r.cents, r.ok ? "ok" : "FAIL");
    }
    if (rep != stderr) fclose(rep);
    fprintf(stderr, "%d of %zu reed pipes not tuned within 5 cents\n", badR, reedJobs.size());
    return 0;
}
