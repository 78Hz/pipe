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
#include <functional>
#include <algorithm>
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

// Checkpoint (CHECKPOINT=<prefix>): every finished pipe is appended to <prefix>.flue / <prefix>.reed as it
// completes, so a run that is stopped resumes where it was instead of starting again.
template <class Spec>
static void ckWrite(FILE* f, std::mutex& m, uint32_t j, bool ok, double a, double b, int fam, const Spec& sp, const std::string& why) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(m);
    const uint8_t o = ok; const uint32_t len = (uint32_t)why.size(), sz = sizeof(Spec);
    fwrite(&sz, 4, 1, f); fwrite(&j, 4, 1, f); fwrite(&o, 1, 1, f); fwrite(&a, 8, 1, f); fwrite(&b, 8, 1, f); fwrite(&fam, 4, 1, f);
    fwrite(&sp, sizeof(Spec), 1, f); fwrite(&len, 4, 1, f); fwrite(why.data(), 1, len, f); fflush(f);
}
template <class Spec, class Fn>
static size_t ckRead(const std::string& path, Fn&& use) {
    FILE* f = fopen(path.c_str(), "rb"); if (!f) return 0;
    size_t n = 0;
    for (;;) {
        uint32_t sz, j, len; uint8_t o; double a, b; int fam; Spec sp;
        // a record written before fields were added at the end of Spec is shorter: those fields keep their defaults
        if (fread(&sz, 4, 1, f) != 1 || sz > sizeof(Spec) || sz < sizeof(Spec) - 64 || fread(&j, 4, 1, f) != 1 || fread(&o, 1, 1, f) != 1 || fread(&a, 8, 1, f) != 1
            || fread(&b, 8, 1, f) != 1 || fread(&fam, 4, 1, f) != 1 || fread(&sp, sz, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > 100000) break;
        std::string why(len, ' '); if (len && fread(&why[0], 1, len, f) != len) break;
        use(j, o != 0, a, b, fam, sp, why); ++n;
    }
    fclose(f); return n;
}

// What a pipe does when its key is pressed, judged from its sound (mouth + top / radiated):
//   f        the pitch it speaks: the strongest component within 0.5 - 2 x the target, taken down to the
//            repetition period (the note heard), refined
//   speaks   it is sounding at the end, audibly (rms >= 0.02, ~28 dB below a principal) and settled
//            (its level at the end within 1 dB of its level at 60 % of the render: not still growing,
//            as a pipe only just above its threshold does - such a pipe is a faint sine, not speaking),
//            and the component at f/2 is weaker than at f
//   tSpeech  time until it is within 3 dB of its final level (10 ms frames)
//   spread   pitch spread over eight frames of the last 40 % (cents): does it hold one pitch
// over: it sounds (loud enough, settled) but on an overtone or with the octave below: overdriven, not faint
struct Heard { double f = 0, rms = 0, tSpeech = 1e9, spread = 1e9, growth = 0; bool speaks = false, over = false; };
// fr > 0: judged over the last four frames of fr samples each (whole tremulant periods): the growth is
// the last frame against the first and the spread is over the four
static Heard analyse(const std::vector<double>& x, double target, double T, int fr4 = 0) {
    Heard h; const int N = (int)x.size(), L = fr4 > 0 ? 4 * fr4 : (int)(0.4 * N);
    double e = 0; for (int i = N - L; i < N; ++i) e += x[i] * x[i];
    h.rms = std::sqrt(e / L);
    if (!(h.rms > 1e-6)) return h;
    double best = 0, bf = 0;
    // the coarse search on a window short enough that its main lobe (+-2 bins) spans the 10 c grid even at
    // twice the target: on the whole segment a treble pipe's lobe is a few Hz wide, a pitch between two
    // grid points was missed and a weak overtone read instead (2026-11-16: trebles at 0.92 wind read
    // +1100 c while speaking 5 c flat)
    const int Lc = std::min(L, std::max(256, (int)(2.0 * 48000 / (2 * target * (std::pow(2.0, 10 / 1200.0) - 1)))));
    for (double c = -1200; c <= 1200; c += 10) { const double ft = target * std::pow(2.0, c / 1200.0); const double a = toneAt(x, N - Lc, Lc, ft, 48000); if (a > best) { best = a; bf = ft; } }
    double f = peakFreq(x, N - L, L, bf, 0.02, 48000);
    // the pitch heard is the repetition period: f/2 is the note when the sound has the odd harmonics of
    // f/2 (f/2 itself or 3f/2); a sound of f's harmonics only repeats at 1/f. (A half-length resonator
    // gives a weak fundamental: the strongest component is then the octave, yet the pipe speaks its note.)
    while (f / 2 > target * 0.45) {
        const double a = toneAt(x, N - L, L, f, 48000);
        const double odd = std::max(toneAt(x, N - L, L, f / 2, 48000), toneAt(x, N - L, L, 1.5 * f, 48000));
        if (!(odd > 0.3 * a)) break;
        f = toneAt(x, N - L, L, f / 2, 48000) > 0.05 * a ? peakFreq(x, N - L, L, f / 2, 0.02, 48000) : peakFreq(x, N - L, L, 1.5 * f, 0.02, 48000) / 3.0;
    }
    h.f = f;
    {
        auto rmsOf = [&](int a, int b) { double q = 0; for (int i = a; i < b; ++i) q += x[i] * x[i]; return std::sqrt(q / std::max(1, b - a)); };
        const double r60 = fr4 > 0 ? rmsOf(N - 4 * fr4, N - 3 * fr4) : rmsOf((int)(0.6 * N), (int)(0.7 * N)), rEnd = fr4 > 0 ? rmsOf(N - fr4, N) : rmsOf((int)(0.9 * N), N);
        h.growth = 20 * std::log10((rEnd + 1e-12) / (r60 + 1e-12));
    }
    // a sound that has collapsed onto one overtone (harmonic k more than 6 dB above the fundamental and
    // the other harmonics that make the note's period - those not multiples of k - 20 dB below it) is
    // heard at k f: the pipe is not speaking its note
    bool onOvertone = false;
    {
        const double a1 = toneAt(x, N - L, L, f, 48000);
        for (int k = 2; k <= 8 && k * f < 20000 && !onOvertone; ++k) {
            const double ak = toneAt(x, N - L, L, k * f, 48000);
            if (!(ak > 2.0 * a1)) continue;
            double others = 0;   // the other harmonics (a weak fundamental under one overtone is not the note)
            for (int j = 2; j <= 16 && j * f < 20000; ++j) if (j % k) others = std::max(others, toneAt(x, N - L, L, j * f, 48000));
            if (others < 0.1 * ak) onOvertone = true;
        }
    }
    const bool subOct = !(toneAt(x, N - L, L, f, 48000) > toneAt(x, N - L, L, f / 2, 48000));
    h.speaks = !onOvertone && !subOct && h.rms >= 0.02 && std::fabs(h.growth) <= 1.0;
    h.over = (onOvertone || subOct) && h.rms >= 0.02 && std::fabs(h.growth) <= 1.0;
    const int fr = 480; const double fin = e / L; h.tSpeech = T;
    for (int i = 0; i + fr < N; i += fr) { double ef = 0; for (int k = 0; k < fr; ++k) ef += x[i + k] * x[i + k]; if (ef / fr > 0.5 * fin) { h.tSpeech = (double)i / 48000; break; } }
    // Frames of at least 20 periods each (up to eight over the segment): a fixed 0.25 s frame is 1000 periods
    // of a treble pipe but 8 of a 32 Hz bass, where the reading then follows the period-to-period jitter of a
    // turbulent jet (+-2 c in 0.25 s, within 1 c over 1 s, on the Subbass 16' C, 2026-11-16), and every
    // pipe is judged over the same number of its own periods.
    const int nF = fr4 > 0 ? 4 : std::max(2, std::min(8, (int)((double)L / 48000 * target / 20.0))), Lf = L / nF; double mn = 1e9, mx = -1e9;
    // followed on the strongest of the first four harmonics (the same cents, a cleaner reading)
    int hk = 1; { double ha = 0; for (int k = 1; k <= 4; ++k) { const double v = toneAt(x, N - L, L, k * f, 48000); if (k * f < 20000 && v > ha) { ha = v; hk = k; } } }
    std::vector<double> cf((size_t)nF);
    for (int k = 0; k < nF; ++k) { const double ff = peakFreq(x, N - L + k * Lf, Lf, hk * f, 0.03, 48000); cf[(size_t)k] = 1200 * std::log2(ff / (hk * f)); mn = std::min(mn, ff); mx = std::max(mx, ff); }
    // Holding one pitch: a drift (the straight line through the frame readings, its change over the segment)
    // is held to 3 c. The rest is the frame-to-frame jitter of a turbulent jet, random, and is held to what
    // can be heard at the pitch: the frequency difference limen, Wier, Jesteadt & Green 1977 (JASA 61,
    // 178), log10 dF = 0.026 sqrt(F) - 0.533 at 40 dB SL (5.9 c at 200 Hz, 3.4 c at 1 kHz, 5.6 c at
    // 4 kHz; their data start at 200 Hz and lower pipes are held to the 200 Hz value, though the ear is
    // coarser there), and not less than 3 c. h.spread is reported on the 3 c scale: max(drift, jitter x 3 / limen), so <= 3 passes
    // either way. (2026-11-16: the Subbass 16' F, F#, D# failed at 3.2 - 3.6 c of random jitter alone; the
    // owner listened and found them steady.)
    {
        double sk = 0, sc = 0, skk = 0, skc = 0; for (int k = 0; k < nF; ++k) { sk += k; sc += cf[(size_t)k]; skk += (double)k * k; skc += k * cf[(size_t)k]; }
        const double sxx = skk - sk * sk / nF, sl = (nF * skc - sk * sc) / std::max(1e-9, nF * skk - sk * sk);
        double rmn = 1e9, rmx = -1e9, ssr = 0; for (int k = 0; k < nF; ++k) { const double r = cf[(size_t)k] - (sc / nF + sl * (k - sk / nF)); rmn = std::min(rmn, r); rmx = std::max(rmx, r); ssr += r * r; }
        // a drift is what the frames show beyond their jitter: the slope less two of its standard errors (on
        // three or four frames a line through random readings has a slope of its own; 2026-11-16 the Subbass
        // 16' D# read 3.4 c of "drift" on three frames, and over 8 s holds -1 +- 2 c with no trend)
        const double se = nF > 2 ? std::sqrt(ssr / (nF - 2) / std::max(1e-9, sxx)) : 0.0;
        const double drift = std::max(0.0, std::fabs(sl) - 2.0 * se) * (nF - 1);
        const double fl = std::max(200.0, f);   // below their 200 Hz the limen is held at its 200 Hz value (5.9 c): not extrapolated
        const double limen = std::max(3.0, 1200 * std::log2(1.0 + std::pow(10.0, 0.026 * std::sqrt(fl) - 0.533) / fl));
        h.spread = std::max(drift, 1200 * std::log2(mx / mn) * 3.0 / limen);   // all the variation counts as jitter (rmx - rmn: diagnostic)
        (void)rmn; (void)rmx;
    }
    return h;
}
static double renderTime(double target) { return std::max(1.5, 0.4 + 150.0 / target); }
// trem > 0: the chest pulses +-11 % at trem Hz (the tremulant at its default depth), judged over the last four frames of whole periods (tremPeriods)
// after the pipe has spoken
static constexpr double kTremDepth = 0.11;
// each of the four frames is a whole number of tremulant periods and at least 20 periods of the pipe (as the
// steady spread, analyse): at 3 Hz one tremulant period is 14 periods of a 44 Hz bass, whose reading then
// follows its jet's period-to-period jitter (Subbass 16' F: 3.1 - 3.4 c, 2026-11-16)
static int tremPeriods(double f, double trem) { return trem > 0 ? std::max(1, (int)std::ceil(20.0 * trem / f)) : 1; }
static thread_local uint32_t gSeed = 7;   // the jet-noise seed the voicer hears a flue pipe with
static Heard hearFlue(const FluePipeSpec& sp, double pallet, double chest = 1.0, double trem = 0) {
    FluePipeRT p; p.init(sp, 48000, gSeed); p.setChestPa(kChestPa * chest); p.setPallet(pallet, pallet); p.key(true);
    const int kt = tremPeriods(sp.target, trem); const double T = renderTime(sp.target) + (trem > 0 ? 4.0 * kt / trem : 0.0); const int N = (int)(T * 48000);
    std::vector<double> x(N); float m, t;
    for (int i = 0; i < N; ++i) {
        if (trem > 0 && i % 16 == 0) p.setChestPa(kChestPa * chest * (1.0 + kTremDepth * std::sin(2 * M_PI * trem * i / 48000.0)));
        p.render(&m, &t, 1); x[i] = m + t;
    }
    return analyse(x, sp.target, T, trem > 0 ? (int)std::lround(kt * 48000 / trem) : 0);
}
static thread_local uint32_t gReedSeed = 3;
static Heard hearReed(const ReedPipeSpec& sp, double pallet, double chest = 1.0, double trem = 0) {
    ReedPipeRT p; p.init(sp, 48000, gReedSeed); p.setChestPa(kChestPa * chest); p.setPallet(pallet, pallet); p.key(true);
    const int kt = tremPeriods(sp.target, trem); const double T = renderTime(sp.target) + (trem > 0 ? 4.0 * kt / trem : 0.0); const int N = (int)(T * 48000);
    std::vector<double> x(N); float o;
    for (int i = 0; i < N; ++i) {
        if (trem > 0 && i % 16 == 0) p.setChestPa(kChestPa * chest * (1.0 + kTremDepth * std::sin(2 * M_PI * trem * i / 48000.0)));
        p.render(&o, 1); x[i] = o;
    }
    return analyse(x, sp.target, T, trem > 0 ? (int)std::lround(kt * 48000 / trem) : 0);
}
static double speechLimit(double hz) { return std::max(0.3, 30.0 / hz); }
// keen strings take about 50 cycles of the fundamental to speak (Pykett, "The tonal structure of organ
// strings"); other pipes 30
// a Subbass (large stopped wood) builds up slowly: 50 cycles, as the strings (the owner, 2026-11-16: "it is
// normal for sub basses to take longer to sound"; at 30 cycles the Subbass 32' below A had no voicing that
// spoke in time and held its note with every noise seed)
static double limitFor(const opus14::Stop& s, double hz) { return (familyOf(s) == kString || std::strstr(s.name, "Subbass")) ? std::max(0.3, 50.0 / hz) : speechLimit(hz); }
// The wind in the instrument is not constant, and a pipe has to stay on its note in the wind it will get
// (Wind.hpp): full chords sag the chest by up to ~8 % (default flexibility; the lid's bounce back
// overshoots by well under 1 %), and the tremulant pulses it +-11 % (default depth 0.22) at 3 - 9 Hz.
// So: at 0.92 x the chest it speaks, settles and holds one pitch within 50 c; under the tremulant at
// its slowest (3 Hz, the longest dwell in the low wind) it keeps speaking its note: the pitch averaged
// over each tremulant period holds within 3 c and the mean is within 50 c. (Before 2026-11-16 the test
// was a static 0.89 and 1.11: that holds the low point of the tremulant for seconds, which the
// tremulant never does, and it failed bass pipes that warble only after ~1 s in the low wind.)
template <class Spec, class Hear>
static bool seedRobust(const Spec& sp, Hear hear, std::string& note) {
    // The voicer hears every pipe with one noise seed; the engine seeds each pipe its own way. A pipe that has
    // two regimes - a large stopped pipe can settle quietly on its note or 15 - 20 dB louder, even on its 5th
    // harmonic, and the jet noise of the attack picks which (2026-11-16: Bourdon 16' D, E; Gedeckt 8' C;
    // Praestant 16' C) - is not voiced: with three more seeds it speaks its note within 5 c of the voiced
    // pitch, steadily, and within 2 dB of the voiced level.
    {
        const uint32_t f0 = gSeed, r0 = gReedSeed;
        const Heard ref = hear(sp, 1.0, 0.0);
        for (uint32_t sd : { 101u, 202u, 303u }) {
            gSeed = sd; gReedSeed = sd;
            const Heard h = hear(sp, 1.0, 0.0);
            gSeed = f0; gReedSeed = r0;
            const double dc = (h.speaks && ref.speaks) ? 1200 * std::log2(h.f / ref.f) : 1e9;
            const double dl = (h.rms > 0 && ref.rms > 0) ? 20 * std::log10(h.rms / ref.rms) : 1e9;
            if (!h.speaks || std::fabs(dc) > 5 || h.spread > 3 || std::fabs(dl) > 2) {
                char buf[160]; snprintf(buf, sizeof buf, "noise seed %u: %s %+.0fc %+.1fdB spread %.1f", sd, h.speaks ? "speaks" : "NOT SPEAKING", h.speaks ? dc : 0.0, dl, h.spread);
                note = buf; return false;
            }
        }
    }
    return true;
}
template <class Spec, class Hear>
static bool windRobust(const Spec& sp, double hz, Hear hear, std::string& note) {
    struct C { double chest, trem; const char* what; };
    for (const C& w : { C{ 0.92, 0.0, "wind x0.92" }, C{ 1.0, 3.0, "tremulant 3 Hz +-11%" } }) {
        const Heard h = hear(sp, w.chest, w.trem);
        const double c = h.speaks ? 1200 * std::log2(h.f / hz) : 1e9;
        if (!h.speaks || std::fabs(c) > 50 || h.spread > 3) {
            char buf[140]; snprintf(buf, sizeof buf, "%s: %s %+.0fc spread %.1f", w.what, h.speaks ? "speaks" : "NOT SPEAKING", h.speaks ? c : 0.0, h.spread);
            note = buf; return false;
        }
    }
    if (!seedRobust(sp, hear, note)) return false;
    note = "wind x0.92, tremulant and 3 noise seeds: holds"; return true;
}
static auto flueAt = [](const FluePipeSpec& q, double x, double tr) { return hearFlue(q, 0.012, x, tr); };
static auto reedAt = [](const ReedPipeSpec& q, double x, double tr) { return hearReed(q, 0.012, x, tr); };
// Tunes by scaling the whole pipe (trim), by the secant method: the pitch does not move exactly in
// proportion (the boot, the shallot and the jet speed are not scaled), so the step learns the slope.
template <class Spec, class Hear>
static double tuneTrim(Spec& sp, double hz, Hear hear, int maxIt = 20) {
    // Like a tuning scroll or slot: small steps (at most 30 c at a time), never past 110 c in all (a pipe
    // that needs more fails the 6 % check and is reported), and the best setting heard is the one kept.
    Spec best = sp; double bestC = 1e9;
    double cPrev = 0, tPrev = 0; bool have = false;
    for (int it = 0; it < maxIt; ++it) {
        const Heard h = hear(sp);
        const double tc = 1200 * std::log2((double)sp.trim);   // trim in cents
        if (!h.speaks) {
            if (bestC > 1e8) return 1e9;                        // never spoke
            // back halfway towards the best setting
            const double tb = 1200 * std::log2((double)best.trim);
            if (std::fabs(tc - tb) < 0.05) break;
            sp.trim = (float)std::pow(2.0, 0.5 * (tc + tb) / 1200.0); have = false; continue;
        }
        const double c = 1200 * std::log2(h.f / hz);
        if (std::fabs(c) < std::fabs(bestC)) { bestC = c; best = sp; }
        if (std::fabs(c) < 0.2) break;
        double slope = 1.0;
        if (have && std::fabs(tc - tPrev) > 1e-6) slope = std::max(0.2, std::min(3.0, (c - cPrev) / (tc - tPrev)));
        cPrev = c; tPrev = tc; have = true;
        const double step = std::max(-30.0, std::min(30.0, -c / slope));
        const double tn = std::max(-110.0, std::min(110.0, tc + step));
        if (std::fabs(tn - tc) < 1e-4) break;                   // at the end of the scroll
        sp.trim = (float)std::pow(2.0, tn / 1200.0);
    }
    sp = best;
    return bestC;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: opus14_organ constants.txt reed_constants.txt out.bin out.hpp [report]\n"); return 1; }
    // reed constants: "family widthRatio gamma Qr bootLitres8 closure shallot zetaMin"
    reed::ReedConstants reedK[2];
    { std::ifstream in(argv[2]); std::string line;
      while (std::getline(in, line)) { if (line.empty() || line[0] == '#') continue; std::istringstream ss(line); std::string n; double a, b, c, d, e = 0.2;
        if (!(ss >> n >> a >> b >> c >> d)) continue;
        ss >> e; double shf = 2.0, zm = 0.0; ss >> shf >> zm;
        const int i = n == "oboe" ? 1 : 0; reedK[i].widthRatio = a; reedK[i].gamma = b; reedK[i].Qr = c; reedK[i].bootLitres8 = d; reedK[i].closure = e; reedK[i].shallot = shf; reedK[i].zetaMin = zm; } }
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
    struct RankOut { int stop; std::string name; int div; bool reed; int mixRow; float detune; int lowest; const opus14::Stop* s; const opus14::Mixture* mix; int first = 0; };
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
            // the first key with a printed diameter (or resonator length): the Suavial (discant) starts at c1
            int first = s->key[0];
            {
                const opus14::Row* r0 = nullptr;
                for (const char* lab : { "Diameter", "Lower Diameter", "Upper Diameter", "Resonator Length", "Stem Length", "Length" })
                    for (int q = 0; q < s->nRows && !r0; ++q) if (std::string(s->rows[q].label) == lab) r0 = &s->rows[q];
                if (r0) for (int i = 0; i < s->nCols; ++i) if (!std::isnan(r0->v[i])) { first = s->key[i]; break; }
            }
            // jobs are still laid out from the old lowest key (so saved progress keeps its places); pipes
            // below the first printed key are absent from the instrument
            ranks.push_back({ (int)si, rn, os.div, s->kind == opus14::kReed, -1, os.detuneCents, std::max(os.lowest, s->key[0]), s, nullptr, std::max(os.lowest, first) });
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
    std::vector<size_t> absentJob;
    for (auto& R : ranks) if (R.first < R.lowest) R.first = R.lowest;
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
            pipeIdx[r][k] = k < R.first ? -1 : (int)jobs.size();
            jobs.push_back({ (int)r, k, R.s, scaleKey, etHz(k, 8.0 / feet) });
            if (k < R.first) absentJob.push_back(jobs.size() - 1);
        }
    }
    std::vector<char> absent(jobs.size(), 0); for (size_t a : absentJob) absent[a] = 1;
    fprintf(stderr, "%zu ranks, %zu flue pipes (%zu not on the organ: below a stop's first printed key)\n", ranks.size(), jobs.size() - absentJob.size(), absentJob.size());
    // JOBLIST: print job number, rank, key and pitch of every flue pipe, then stop
    if (getenv("JOBLIST")) { for (size_t j = 0; j < jobs.size(); ++j) printf("%zu\t%s\t%d\t%.2f%s\n", j, ranks[(size_t)jobs[j].rank].name.c_str(), jobs[j].key, jobs[j].hz, absent[j] ? "\tabsent" : ""); return 0; }
    // ONLY=<text>: a trial on the ranks whose name contains it (the others are skipped; nothing usable is written)
    const char* only = getenv("ONLY");
    // SKIP=<text>: the ranks whose name contains it are left for a later run (their pipes are not voiced now)
    const char* skipTxt = getenv("SKIP");
    auto skip = [&](int rank) { return (only && ranks[(size_t)rank].name.find(only) == std::string::npos) || (skipTxt && ranks[(size_t)rank].name.find(skipTxt) != std::string::npos); };

    std::vector<Result> res(jobs.size());
    std::atomic<size_t> next{0}; std::mutex io; bool reused = false;
    if (const char* reuse = getenv("REUSE_FLUES")) {
        FILE* f = fopen(reuse, "rb"); char magic[4]; uint32_t ver = 0, n = 0, sz = 0;
        if (f && fread(magic, 1, 4, f) == 4 && fread(&ver, 4, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && fread(&sz, 4, 1, f) == 1
            && ver == 2 && n == jobs.size() && sz == sizeof(FluePipeSpec)) {
            for (auto& r : res) { if (fread(&r.spec, sizeof(FluePipeSpec), 1, f) != 1) break; r.ok = true; r.fam = 0; }
            next = jobs.size();
            fprintf(stderr, "flue pipes reused from %s: checking every one again\n", reuse);
            // reused pipes are not trusted: each is heard again and has to pass the same checks
            std::atomic<size_t> nv{0};
            auto verify = [&] {
                for (size_t j; (j = nv++) < jobs.size();) {
                    Result& out = res[j]; const double hz = jobs[j].hz;
                    if (skip(jobs[j].rank)) continue;
                    const Heard a = hearFlue(out.spec, 0.012), b = hearFlue(out.spec, 0.002);
                    out.centsEnd = a.speaks ? 1200 * std::log2(a.f / hz) : 1e9;
                    const double lim = limitFor(*jobs[j].s, hz);
                    const bool slow = !a.speaks || !b.speaks || a.tSpeech > lim || b.tSpeech > lim;
                    const bool unstable = a.spread > 3 || b.spread > 3 || (b.speaks && std::fabs(1200 * std::log2(b.f / hz)) > 2);
                    std::string wn; const bool robust = !slow && !unstable && windRobust(out.spec, hz, flueAt, wn);
                    out.ok = !slow && !unstable && robust && std::fabs(out.centsEnd) < 0.5 && std::fabs(out.spec.trim - 1.f) < 0.06f;
                    char buf[260]; snprintf(buf, sizeof buf, "[reused: %s rms %.3f %+.1fc fast %+.1fc spread %.1f/%.1f speech %.2f/%.2fs trim %.3f %s]", a.speaks ? "speaks" : "NOT SPEAKING", a.rms, out.centsEnd, b.speaks ? 1200 * std::log2(b.f / hz) : 1e9, a.spread, b.spread, a.tSpeech, b.tSpeech, out.spec.trim, wn.c_str());
                    out.why = buf;
                }
            };
            std::vector<std::thread> tv; for (unsigned i = 0; i < std::max(1u, std::thread::hardware_concurrency()); ++i) tv.emplace_back(verify);
            for (auto& t : tv) t.join();
            next = 0; reused = true;   // the ones that fail are voiced again
        } else fprintf(stderr, "cannot reuse %s: computing the flue pipes\n", reuse);
        if (f) fclose(f);
    }
    const char* ck = getenv("CHECKPOINT");
    std::vector<char> doneF(jobs.size(), 0); std::mutex ckm;
    if (ck) {
        const size_t n = ckRead<FluePipeSpec>(std::string(ck) + ".flue", [&](uint32_t j, bool ok, double a, double b, int fam, const FluePipeSpec& sp, const std::string& why) {
            if (j < jobs.size()) { res[j].spec = sp; res[j].ok = ok; res[j].cents0 = a; res[j].centsEnd = b; res[j].fam = fam; res[j].why = why; doneF[j] = (ok || !getenv("REDO_FAILED")) ? 1 : 0; } });
        fprintf(stderr, "checkpoint: %zu flue pipes already done\n", n);
    }
    FILE* ckF = ck ? fopen((std::string(ck) + ".flue").c_str(), "ab") : nullptr;
    // SEEDCHECK=1: every voiced pipe is heard with three more noise seeds (seedRobust); one that does not hold is
    // written to the checkpoint as failed (REDO_FAILED then voices it again) and listed. Then stops.
    if (getenv("SEEDCHECK")) {
        std::vector<size_t> list; for (size_t j = 0; j < jobs.size(); ++j) if (doneF[j] && res[j].ok && !absent[j] && !skip(jobs[j].rank)) list.push_back(j);
        std::atomic<size_t> nx{0}, nbad{0}, ndone{0};
        auto w = [&] { for (;;) { const size_t i = nx++; if (i >= list.size()) return; const size_t j = list[i];
            std::string note; const bool ok = seedRobust(res[j].spec, flueAt, note);
            if (!ok) { ++nbad; res[j].ok = false; res[j].why += "[seed check: " + note + "]";
                ckWrite(ckF, ckm, (uint32_t)j, false, res[j].cents0, res[j].centsEnd, res[j].fam, res[j].spec, res[j].why);
                std::lock_guard<std::mutex> lk(io); printf("%zu\t%s\t%d\t%s\n", j, ranks[(size_t)jobs[j].rank].name.c_str(), jobs[j].key, note.c_str()); fflush(stdout); }
            const size_t d = ++ndone; if (d % 100 == 0) { std::lock_guard<std::mutex> lk(io); fprintf(stderr, "  %zu / %zu heard, %zu do not hold\n", d, list.size(), nbad.load()); } } };
        std::vector<std::thread> tv; for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) tv.emplace_back(w); for (auto& t : tv) t.join();
        fprintf(stderr, "seed check: %zu of %zu pipes do not hold\n", nbad.load(), list.size());
        return 0;
    }
    // VERIFY: every pipe already voiced is heard again under the current checks (a pipe voiced before a
    // check existed has not passed it); one that fails is voiced again. Pipes verified are listed in
    // <checkpoint>.fverify (job, 1/0, the reading) so that a resumed run does not hear them twice.
    const bool verifyAll = getenv("VERIFY") && ck;
    std::vector<char> verified(jobs.size(), 0);
    FILE* vfF = nullptr;
    if (verifyAll) {
        if (FILE* f = fopen((std::string(ck) + ".fverify").c_str(), "r")) { unsigned jj; int okv; char line[600];
            while (fgets(line, sizeof line, f)) if (sscanf(line, "%u %d", &jj, &okv) == 2 && jj < jobs.size() && okv) verified[jj] = 1; fclose(f); }
        vfF = fopen((std::string(ck) + ".fverify").c_str(), "a");
    }
    auto checkFlue = [&](size_t j, std::string& why) {
        const FluePipeSpec& sp = res[j].spec; const double hz = jobs[j].hz;
        const Heard a = hearFlue(sp, 0.012), b = hearFlue(sp, 0.002);
        const double c = a.speaks ? 1200 * std::log2(a.f / hz) : 1e9, cb = b.speaks ? 1200 * std::log2(b.f / hz) : 1e9;
        const double lim = limitFor(*jobs[j].s, hz);
        bool ok = a.speaks && b.speaks && a.tSpeech <= lim && b.tSpeech <= lim && a.spread <= 3 && b.spread <= 3 && std::fabs(cb) <= 2
                  && std::fabs(c) < 0.5 && std::fabs(sp.trim - 1.f) < 0.06f;
        std::string wn; if (ok) ok = windRobust(sp, hz, flueAt, wn);
        char buf[300]; snprintf(buf, sizeof buf, "[verify: %s rms %.3f %+.2fc fast %+.2fc spread %.1f/%.1f speech %.2f/%.2fs trim %.3f %s]", a.speaks ? "speaks" : "NOT SPEAKING", a.rms, c, cb, a.spread, b.spread, a.tSpeech, b.tSpeech, sp.trim, wn.c_str());
        why = buf; return ok;
    };
    auto worker = [&] {
        for (;;) {
            const size_t j = next++;
            if (j >= jobs.size()) return;
            const Job& jb = jobs[j]; Result& out = res[j];
            if (skip(jb.rank) || getenv("REEDS_ONLY") || absent[j]) { out.ok = true; continue; }
            if (const char* ks = getenv("KEYS")) { int a = 0, b = 127; sscanf(ks, "%d-%d", &a, &b); if (jb.key < a || jb.key > b) { out.ok = true; continue; } }
            if (doneF[j] && verifyAll && out.ok && !verified[j]) {
                std::string vw; const bool vok = checkFlue(j, vw);
                { std::lock_guard<std::mutex> lk(ckm); if (vfF) { fprintf(vfF, "%zu %d %s\n", j, vok ? 1 : 0, vw.c_str()); fflush(vfF); } }
                if (vok) continue;
                doneF[j] = 0; out.ok = false; out.why = vw + " ";   // voiced again below
            }
            if (doneF[j]) continue;
            if (getenv("SURVEY")) {
                out.ok = true;
                if (jb.key % 12 != 0) continue;
                Voicing vs; vs.ising = familyOf(*jb.s) == kString ? 2.8 : 2.0;
                const Design ds = designAt(*jb.s, jb.scaleKey, jb.hz, vs);
                JetParams js = fam[jetFamily(*jb.s, ds)];
                const Heard h = hearFlue(specOf(ds, js), 0.012);
                std::lock_guard<std::mutex> lk(io);
                printf("SURVEY %-24s key %3d %8.2f Hz  rms %.4f growth %+6.1f dB  %+8.1f c  speech %.2fs  %s\n", ranks[(size_t)jb.rank].name.c_str(), jb.key, jb.hz, h.rms, h.growth,
                       h.f > 0 ? 1200 * std::log2(h.f / jb.hz) : 0.0, h.tSpeech, h.speaks ? "speaks" : "NOT SPEAKING");
                fflush(stdout);
                continue;
            }
            if (reused && out.ok) { ckWrite(ckF, ckm, (uint32_t)j, out.ok, out.cents0, out.centsEnd, out.fam, out.spec, out.why); continue; }
            out.why.clear();
            // keen strings take about 50 cycles of the fundamental to speak (Pykett, "The tonal structure of
            // organ strings"); other flue pipes 30
            const double limitF = limitFor(*jb.s, jb.hz);
            Voicing v; v.ising = familyOf(*jb.s) == kString ? 2.8 : 2.0;
            // a harmonic pipe has its hole at half the speaking length (Flynt); its size is not published:
            // opened from 10 % of the diameter, as at the voicing machine, until the pipe settles on its
            // second mode
            double holeFrac = getenv("HOLE") ? (atof(getenv("HOLE")) > 0 ? atof(getenv("HOLE")) : 0.10) : 0.0;   // off: in this model the hole made the pipes worse (open point)
            auto withHole = [&](Design dd) {
                if (dd.speakMode == 2 && holeFrac > 0) { const double L = dd.g.body[0].L, dia = 2 * dd.radius;
                    dd.g.holes = { { 0.5 * L, M_PI * 0.25 * (holeFrac * dia) * (holeFrac * dia), dd.g.wall } }; recut(dd, 1.0); }
                return dd;
            };
            Design d0 = withHole(designAt(*jb.s, jb.scaleKey, jb.hz, v));
            const JetFam jf = jetFamily(*jb.s, d0); out.fam = jf;
            JetParams jpp = fam[jf];
            out.ok = false;
            double isLo = 0, isHi = 1e9;   // Ising numbers found to give a lower / a higher mode
            bool swept = false;
            double gBase = fam[jf].gain;   // the jet gain the escalation starts from (the sweep may raise it)
            int esc = 0;                   // escalation steps since the start or the sweep
            // The voicer sweeps the wind against the cut-up (the Ising number) over its range and voices in
            // the middle of the widest band where a slow and a fast attack both land on the pipe's mode and
            // speak in time. (Used for a pipe on the wrong mode, or one that speaks too slowly: the strings
            // were voiced above what their toe holes pass, and speak in 1.5 s there, at once lower down.)
            auto sweep = [&](int attempt) {
                swept = true;
                int bestA = -1, bestB = -1; double bestY = jpp.y0b, bestG = jpp.gain;
                std::vector<double> Is; for (double I = 1.0; I <= 4.2; I *= 1.04) if (!getenv("SWEEPTRACE") || !getenv("ISTART") || (I >= atof(getenv("ISTART")) && I <= atof(getenv("IEND")))) Is.push_back(I);
                // the languid (lip offset y0/b) as well: the family's own first, then from the jet thrown
                // outside the mouth to the principal's setting (a beard keeps the sheet near the lip)
                std::vector<double> Ys = { jpp.y0b };
                for (double y : { -0.3, 0.0, 0.2, 0.4, 0.58 }) if (std::fabs(y - jpp.y0b) > 0.05) Ys.push_back(y);
                // and the jet's gain, from the family's own up (the beard / ears that steady a large bass pipe,
                // Pykett): the least that gives a band is kept. A bass Bourdon has no band at the family's gain:
                // where it speaks in time it does not lock its upper resonances (quasi-periodic, Auvray et al.),
                // where it locks it speaks too slowly; with a stronger jet it does both.
                for (double gx : { 1.0, 1.6, 2.5, 4.0, 6.3 }) {
                if (getenv("SWEEPGX") && std::fabs(gx - atof(getenv("SWEEPGX"))) > 0.01) continue;
                const double g = fam[jf].gain * gx;
                for (double y : Ys) {
                int runA = -1, runB = -1;
                for (size_t q = 0; q <= Is.size(); ++q) {
                    bool good = false;
                    if (q < Is.size()) {
                        Voicing vq = v; vq.ising = Is[q];
                        Design dq = withHole(designAt(*jb.s, jb.scaleKey, jb.hz, vq));
                        // above the most its toe hole passes (applyIsing then voices at that limit) every Ising
                        // number gives the same pipe: those are not a band (2026-11-16: a Celeste "band" 2.03 -
                        // 4.10 was one capped pipe tried 19 times, which then failed the tremulant)
                        if (dq.ising < Is[q] * 0.99) { if (runA >= 0) { if (bestA < 0 || runB - runA > bestB - bestA) { bestA = runA; bestB = runB; bestY = y; bestG = g; } runA = runB = -1; } break; }
                        JetParams jq = jpp; jq.y0b = y; jq.gain = g;
                        FluePipeSpec sq = specOf(dq, jq);
                        auto ok1 = [&](const Heard& h, double lim) { return h.speaks && std::fabs(1200 * std::log2(h.f / jb.hz)) < lim && h.tSpeech <= limitF && h.spread <= 3; };
                        // cut to pitch first (one cut), then judged within 50 c: a band found on an uncut pipe
                        // moves once the body is cut (the mode, the speech and the Ising number follow the length)
                        const Heard h0 = hearFlue(sq, 0.012);
                        good = ok1(h0, 300);
                        Heard h1, h2; std::string wn;
                        if (good) { recut(dq, std::pow(h0.f / jb.hz, 1.5)); sq = specOf(dq, jq); h1 = hearFlue(sq, 0.012); good = ok1(h1, 50); if (good) { h2 = hearFlue(sq, 0.002); good = ok1(h2, 50); } }
                        if (good) good = windRobust(sq, jb.hz, flueAt, wn);
                        if (getenv("SWEEPTRACE")) { std::lock_guard<std::mutex> lk(io);
                            fprintf(stderr, "sweep %s %d g x%.1f y %.2f I %.2f | uncut %s %+.0fc %.2fs sp %.1f | cut %s %+.1fc %.2fs sp %.1f | fast %s %+.1fc %.2fs sp %.1f | %s\n", ranks[(size_t)jb.rank].name.c_str(), jb.key, gx, y, Is[q],
                                h0.speaks ? "ok" : (h0.over ? "OVER" : "NO"), h0.speaks ? 1200 * std::log2(h0.f / jb.hz) : 0.0, h0.tSpeech, h0.spread,
                                h1.speaks ? "ok" : (h1.over ? "OVER" : "NO"), h1.speaks ? 1200 * std::log2(h1.f / jb.hz) : 0.0, h1.tSpeech, h1.spread,
                                h2.speaks ? "ok" : (h2.over ? "OVER" : "NO"), h2.speaks ? 1200 * std::log2(h2.f / jb.hz) : 0.0, h2.tSpeech, h2.spread, wn.c_str()); }
                    }
                    if (good) { if (runA < 0) runA = (int)q; runB = (int)q; }
                    else if (runA >= 0) { if (bestA < 0 || runB - runA > bestB - bestA) { bestA = runA; bestB = runB; bestY = y; bestG = g; } runA = runB = -1; }
                }
                if (bestA >= 0 && bestB - bestA >= 2) break;   // a band three steps (8 %) wide: enough
                }
                if (bestA >= 0) break;
                }
                char buf[220];
                if (bestA >= 0) {
                    v.ising = std::sqrt(Is[(size_t)bestA] * Is[(size_t)bestB]); jpp.y0b = bestY; jpp.gain = bestG; gBase = bestG; esc = 0;
                    snprintf(buf, sizeof buf, "[%d: swept: speaks on its mode, steady, in time for Ising %.2f - %.2f at lip offset %.2f, jet gain x%.1f, voiced at %.2f] ", attempt, Is[(size_t)bestA], Is[(size_t)bestB], bestY, bestG / fam[jf].gain, v.ising);
                } else snprintf(buf, sizeof buf, "[%d: swept Ising 1.0 - 4.2, lip offset -0.3 - 0.58, jet gain x1 - x6.3: never speaks on its mode, steady, in time with both attacks] ", attempt);
                out.why += buf;
                d0 = withHole(designAt(*jb.s, jb.scaleKey, jb.hz, v));
                return bestA >= 0;
            };
            if (getenv("SWEEPDBG")) {
                for (double I = getenv("ISTART") ? atof(getenv("ISTART")) : 1.3; I <= (getenv("IEND") ? atof(getenv("IEND")) : 4.6); I *= 1.08) {
                    Voicing vq = v; vq.ising = I;
                    Design dq = withHole(designAt(*jb.s, jb.scaleKey, jb.hz, vq));
                    JetParams jd = jpp; if (getenv("GAINX")) jd.gain *= atof(getenv("GAINX")); if (getenv("LIPY")) jd.y0b = atof(getenv("LIPY"));
                    const FluePipeSpec sq = specOf(dq, jd);
                    const Heard ha = hearFlue(sq, 0.012), hb = hearFlue(sq, 0.002);
                    if (const char* dump = getenv("DUMPWAV")) {
                        FluePipeRT p; p.init(sq, 48000, 7); p.setChestPa(kChestPa); p.setPallet(0.012, 0.012); p.key(true);
                        const int N = (int)(renderTime(sq.target) * 48000); std::vector<float> x(N); float m, t;
                        for (int i = 0; i < N; ++i) { p.render(&m, &t, 1); x[i] = m + t; }
                        char fn[512]; snprintf(fn, sizeof fn, "%s/sweep_%d_%d_%.2f.f32", dump, jb.rank, jb.key, I);
                        if (FILE* f = fopen(fn, "wb")) { fwrite(x.data(), 4, N, f); fclose(f); }
                    }
                    std::lock_guard<std::mutex> lk(io);
                    fprintf(stderr, "%s key %d  Ising %.2f flue %.3f mm foot %.0f Pa: slow %s %+.0fc speech %.2fs spread %.1f | fast %+.0fc speech %.2fs | limit %.2fs\n", ranks[(size_t)jb.rank].name.c_str(), jb.key, I, dq.flue * 1e3, dq.footPa,
                            ha.speaks ? "speaks" : "SILENT", ha.speaks ? 1200 * std::log2(ha.f / jb.hz) : 0.0, ha.tSpeech, ha.spread, hb.speaks ? 1200 * std::log2(hb.f / jb.hz) : 0.0, hb.tSpeech, limitF);
                }
                out.ok = true; continue;
            }
            if (getenv("SWEEPTRACE")) { sweep(0); out.ok = true; continue; }
            for (int attempt = 0; attempt < 14 && !out.ok; ++attempt) {
                Design d = d0;
                out.spec = specOf(d, jpp);
                if (const char* dump = getenv("DUMPWAV")) if (attempt == 0) {
                    FluePipeRT p; p.init(out.spec, 48000, 7); p.setChestPa(kChestPa); p.setPallet(0.012, 0.012); p.key(true);
                    const int N = (int)(renderTime(out.spec.target) * 48000); std::vector<float> x(N); float m, t;
                    std::vector<float> fp(N);
                    for (int i = 0; i < N; ++i) { p.render(&m, &t, 1); x[i] = m + t; fp[i] = (float)p.footPressure(); }
                    char fn[512]; snprintf(fn, sizeof fn, "%s/flue_%d_%d.f32", dump, jb.rank, jb.key);
                    if (FILE* f = fopen(fn, "wb")) { fwrite(x.data(), 4, N, f); fclose(f); }
                    snprintf(fn, sizeof fn, "%s/foot_%d_%d.f32", dump, jb.rank, jb.key);
                    if (FILE* f = fopen(fn, "wb")) { fwrite(fp.data(), 4, N, f); fclose(f); }
                    fprintf(stderr, "dump %s key %d: toe %.2f mm  foot design %.0f Pa  flue %.3f mm  cutup %.2f mm\n", ranks[(size_t)jb.rank].name.c_str(), jb.key, out.spec.toe * 1e3, out.spec.footPa, d.flue * 1e3, d.cutup * 1e3);
                }
                // cut the body to pitch
                bool wrongMode = false;
                for (int it = 0; it < 4; ++it) {
                    const Heard h = hearFlue(out.spec, 0.012);
                    if (!h.speaks) break;
                    const double c = 1200 * std::log2(h.f / jb.hz);
                    if (attempt == 0 && it == 0) out.cents0 = c;
                    // on another mode: the voicer changes the wind against the cut-up (the Ising number,
                    // Fletcher & Rossing: it sets the mode a flue pipe speaks): more for a pipe that speaks
                    // below its mode (a harmonic pipe that will not overblow), less for one that overblows
                    // after two tries on the wrong mode: the voicer sweeps the wind against the cut-up over
                    // its range and voices in the middle of the widest band where a slow and a fast attack
                    // both land on the pipe's mode
                    if (it == 0 && std::fabs(c) > 600 && attempt >= 2 && !swept) {
                        sweep(attempt);
                        wrongMode = true; break;
                    }
                    if (it == 0 && std::fabs(c) > 600) {
                        // a harmonic pipe below its mode: first the hole is opened further (to half the diameter)
                        if (c < 0 && d.speakMode == 2 && holeFrac > 0 && holeFrac < 0.499) holeFrac = std::min(0.5, holeFrac + 0.1);
                        else {
                            // bisected once there is a value that is too low and one too high
                            if (c < 0) isLo = std::max(isLo, v.ising); else isHi = std::min(isHi, v.ising);
                            v.ising = (isLo > 0 && isHi < 1e9) ? std::sqrt(isLo * isHi) : v.ising * (c < 0 ? 1.2 : 1.0 / 1.2);
                        }
                        char buf[120]; snprintf(buf, sizeof buf, "[%d: on another mode %+.0fc: hole %.0f%% Ising %.2f] ", attempt, c, 100 * (d.speakMode == 2 ? holeFrac : 0.0), v.ising); out.why += buf;
                        d0 = withHole(designAt(*jb.s, jb.scaleKey, jb.hz, v));
                        wrongMode = true; break;
                    }
                    if (std::fabs(c) < 0.5 || std::fabs(c) > 300) break;
                    recut(d, std::pow(h.f / jb.hz, 1.5));
                    out.spec = specOf(d, jpp);
                }
                if (wrongMode) continue;
                out.spec.target = (float)jb.hz;
                // final tuning: the whole pipe scaled to pitch
                tuneTrim(out.spec, jb.hz, [](const FluePipeSpec& q) { return hearFlue(q, 0.012); });
                // the checks: it speaks on its mode, in time, holds one pitch with a slow and a fast pallet,
                // and is in tune
                if (const char* dump = getenv("DUMPWAV")) {
                    FluePipeRT p; p.init(out.spec, 48000, 7); p.setChestPa(kChestPa); p.setPallet(0.012, 0.012); p.key(true);
                    const int N = (int)(renderTime(out.spec.target) * 48000); std::vector<float> x(N); float m, t;
                    for (int i = 0; i < N; ++i) { p.render(&m, &t, 1); x[i] = m + t; }
                    char fn[512]; snprintf(fn, sizeof fn, "%s/final_%d_%d_%d.f32", dump, jb.rank, jb.key, attempt);
                    if (FILE* f = fopen(fn, "wb")) { fwrite(x.data(), 4, N, f); fclose(f); }
                }
                const Heard a = hearFlue(out.spec, 0.012), b = hearFlue(out.spec, 0.002);
                out.centsEnd = a.speaks ? 1200 * std::log2(a.f / jb.hz) : 1e9;
                // overdriven (on an overtone, or the octave below) is not slow: it gets less jet, not more
                const bool over = (!a.speaks && a.over) || (!b.speaks && b.over);
                const bool slow = !over && (!a.speaks || !b.speaks || a.tSpeech > limitF || b.tSpeech > limitF);
                bool unstable = a.spread > 3 || b.spread > 3 || (b.speaks && std::fabs(1200 * std::log2(b.f / jb.hz)) > 2);
                std::string wn;
                if (!over && !slow && !unstable && !windRobust(out.spec, jb.hz, flueAt, wn)) unstable = true;   // fragile: treated as unstable
                out.ok = !over && !slow && !unstable && std::fabs(out.centsEnd) < 0.5 && std::fabs(out.spec.trim - 1.f) < 0.06f;
                char buf[300];
                snprintf(buf, sizeof buf, "[%d: %s rms %.3f growth %+.1fdB %+.1fc fast %+.1fc spread %.1f/%.1f speech %.2f/%.2fs trim %.3f %s] ", attempt, a.speaks ? "speaks" : "NOT SPEAKING", a.rms, a.growth, out.centsEnd, b.speaks ? 1200 * std::log2(b.f / jb.hz) : 1e9, a.spread, b.spread, a.tSpeech, b.tSpeech, out.spec.trim, wn.c_str());
                out.why += buf;
                if (out.ok) break;
                // a slow or unstable pipe: first the sweep (once), then the jet gain both ways
                // a slow or faint pipe first gets a stronger jet (as a beard steadies the sheet: Pykett); the sweep
                // of wind and languid comes if that has not worked by the sixth try
                if ((slow || unstable || over) && !swept && attempt >= 6) { sweep(attempt); continue; }
                ++esc;
                if (over) jpp.gain = gBase * std::pow(0.85, esc);
                else if (slow) jpp.gain = gBase * std::pow(1.2, esc);
                else if (unstable) jpp.gain = gBase * (esc % 2 == 1 ? std::pow(0.88, (esc + 1) / 2) : std::pow(1.15, esc / 2));
                else {
                    // it speaks well and is only out of tune (the tuning's secant can stall on a bass whose pitch
                    // reading jitters by a cent): tuned again from where it is, and every check heard again
                    for (int rt = 0; rt < 3 && !out.ok; ++rt) {
                        tuneTrim(out.spec, jb.hz, [](const FluePipeSpec& q) { return hearFlue(q, 0.012); });
                        const Heard a2 = hearFlue(out.spec, 0.012), b2 = hearFlue(out.spec, 0.002);
                        out.centsEnd = a2.speaks ? 1200 * std::log2(a2.f / jb.hz) : 1e9;
                        const bool sl2 = !a2.speaks || !b2.speaks || a2.tSpeech > limitF || b2.tSpeech > limitF;
                        bool un2 = a2.spread > 3 || b2.spread > 3 || (b2.speaks && std::fabs(1200 * std::log2(b2.f / jb.hz)) > 2);
                        std::string wn2; if (!sl2 && !un2 && !windRobust(out.spec, jb.hz, flueAt, wn2)) un2 = true;
                        out.ok = !sl2 && !un2 && std::fabs(out.centsEnd) < 0.5 && std::fabs(out.spec.trim - 1.f) < 0.06f;
                        char b3[200]; snprintf(b3, sizeof b3, "[retuned: %+.2fc trim %.4f %s] ", out.centsEnd, out.spec.trim, out.ok ? "holds" : wn2.c_str()); out.why += b3;
                    }
                    break;
                }
            }
            ckWrite(ckF, ckm, (uint32_t)j, out.ok, out.cents0, out.centsEnd, out.fam, out.spec, out.why);
            if ((j + 1) % 200 == 0) { std::lock_guard<std::mutex> lk(io); fprintf(stderr, "  %zu / %zu\n", j + 1, jobs.size()); }
        }
    };
    std::vector<std::thread> th; const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < nt; ++i) th.emplace_back(worker);
    for (auto& t : th) t.join();
    // REGULATE: the rank made even, as the voicer regulates it on the voicing machine. Each pipe's loudness
    // (rms at the slow pallet) is compared with the median of its three neighbours either side in the rank;
    // one more than 3 dB away is brought to that median (within 1.5 dB): too loud, its toe is closed; too
    // quiet, its toe is opened (when it has one) and then its flue widened (the jet at the lip, its width
    // and its transit follow the flue as in FlueSim::init). Every setting is tuned and must pass every
    // check of the pipe (speech, one pitch with both pallets, in tune, the wind) to be kept.
    if (getenv("REGULATE") && !getenv("REEDS_ONLY")) {
        auto rmsDb = [](const FluePipeSpec& q) { const Heard h = hearFlue(q, 0.012); return h.rms > 1e-9 ? 20 * std::log10(h.rms) : -200.0; };
        auto withFlue = [](FluePipeSpec q, double k) {
            // the flue widened by k: the jet at the lip as FlueSim::init makes it; the transit scaled by the
            // ratio of the formula at the new and the old flue (the pipe's own transit kept at k = 1)
            const resonator::Air air;
            auto jet = [&](double h, double& b, double& lipSpeed) {
                const double W = q.W, x0 = 6.1 * h;
                if (W <= x0) { lipSpeed = 1.0; b = 0.4 * h; } else { lipSpeed = 2.47 * std::sqrt(h / W); b = std::max(0.4 * h, 0.10 * W / 0.8814); }
                const double wR = 12.0 * air.eta() * q.languid / (h * h), V = (std::sqrt(wR * wR + 2.0 * air.rho() * q.footPa) - wR) / air.rho();
                const double C = M_PI / std::pow(1.78, 1.5), u = std::cbrt(V * V * C * C * 2 * M_PI * q.target * h);
                return W / u * V;
            };
            double b0, l0, b1, l1; const double t0 = jet(q.h, b0, l0), t1 = jet(q.h * k, b1, l1);
            q.h = (float)(q.h * k); q.b = (float)b1; q.lipSpeed = (float)l1; q.transit = (float)(q.transit * t1 / t0);
            return q;
        };
        auto passes = [&](size_t j, const FluePipeSpec& sp) {
            const double hz = jobs[j].hz; const Heard a = hearFlue(sp, 0.012), b = hearFlue(sp, 0.002);
            const double c = a.speaks ? 1200 * std::log2(a.f / hz) : 1e9, cb = b.speaks ? 1200 * std::log2(b.f / hz) : 1e9;
            const double lim = limitFor(*jobs[j].s, hz); std::string wn;
            return a.speaks && b.speaks && a.tSpeech <= lim && b.tSpeech <= lim && a.spread <= 3 && b.spread <= 3 && std::fabs(cb) <= 2
                   && std::fabs(c) < 0.5 && std::fabs(sp.trim - 1.f) < 0.06f && windRobust(sp, hz, flueAt, wn);
        };
        for (int round = 0; round < 3; ++round) {
            std::vector<double> db(jobs.size(), -200); std::atomic<size_t> nx{0};
            auto hearAll = [&] { for (;;) { const size_t j = nx++; if (j >= jobs.size()) return; if (doneF[j] && res[j].ok && !absent[j] && !skip(jobs[j].rank) && (!getenv("REGRANK") || ranks[(size_t)jobs[j].rank].name.find(getenv("REGRANK")) != std::string::npos)) db[j] = rmsDb(res[j].spec); } };
            { std::vector<std::thread> tv; for (unsigned i = 0; i < nt; ++i) tv.emplace_back(hearAll); for (auto& t : tv) t.join(); }
            std::map<int, std::vector<size_t>> byRank; for (size_t j = 0; j < jobs.size(); ++j) if (db[j] > -199) byRank[jobs[j].rank].push_back(j);
            std::vector<std::pair<size_t, double>> todo;   // pipe, target dB
            // The rank's loudness should change smoothly from pipe to pipe along the compass (a regulated rank):
            // a step of more than 1.5 dB a semitone is a fault. Over each stretch between breaks (a mixture
            // rank that breaks back starts a new stretch: its pipes are a different size) the longest run
            // without such a step is taken as the rank's voice, and the line is carried outward from it pipe by
            // pipe (the straight line through the last five pipes on it, its slope within 1 dB a semitone); a
            // pipe more than 2 dB off the line is brought to it (and the line goes on from its new place).
            for (auto& [rk, v] : byRank) {
                std::sort(v.begin(), v.end(), [&](size_t a, size_t b) { return jobs[a].key < jobs[b].key; });
                std::vector<std::vector<size_t>> segs(1);
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i > 0) { const double r = 1200 * std::log2(jobs[v[i]].hz / jobs[v[i - 1]].hz) / std::max(1, jobs[v[i]].key - jobs[v[i - 1]].key); if (std::fabs(r - 100) > 5) segs.emplace_back(); }
                    segs.back().push_back(v[i]);
                }
                for (const auto& sg : segs) {
                    const int n = (int)sg.size(); if (n < 4) continue;
                    std::vector<double> k(n), d(n); for (int i = 0; i < n; ++i) { k[i] = jobs[sg[i]].key; d[i] = db[sg[i]]; }
                    int ra = 0, rb = 1, a = 0;   // the longest run [ra, rb)
                    for (int i = 1; i <= n; ++i) if (i == n || std::fabs(d[i] - d[i - 1]) > 1.5 * std::max(1.0, k[i] - k[i - 1])) { if (i - a > rb - ra) { ra = a; rb = i; } a = i; }
                    std::vector<double> t = d;
                    auto extra = [&](const std::vector<int>& idx, int i) {
                        if (idx.size() < 2) return t[(size_t)idx.back()];
                        double sx = 0, sy = 0, sxx = 0, sxy = 0; const double m = (double)idx.size();
                        for (int q : idx) { sx += k[q]; sy += t[q]; sxx += k[q] * k[q]; sxy += k[q] * t[q]; }
                        double sl = (m * sxy - sx * sy) / std::max(1e-9, m * sxx - sx * sx); sl = std::clamp(sl, -1.0, 1.0);
                        return t[(size_t)idx.back()] + sl * (k[i] - k[idx.back()]);
                    };
                    for (int i = rb; i < n; ++i) { std::vector<int> idx; for (int q = std::max(ra, i - 5); q < i; ++q) idx.push_back(q);
                        const double e = extra(idx, i); if (std::fabs(d[i] - e) > 2.0) { t[i] = e; todo.push_back({ sg[i], e }); } }
                    for (int i = ra - 1; i >= 0; --i) { std::vector<int> idx; for (int q = std::min(rb, i + 6) - 1; q > i; --q) idx.push_back(q);
                        const double e = extra(idx, i); if (std::fabs(d[i] - e) > 2.0) { t[i] = e; todo.push_back({ sg[i], e }); } }
                }
            }
            fprintf(stderr, "regulation round %d: %zu pipes off the line of their rank\n", round, todo.size());
            if (todo.empty()) break;
            std::atomic<size_t> nt2{0};
            auto reg = [&] {
                for (;;) {
                    const size_t t = nt2++; if (t >= todo.size()) return;
                    const size_t j = todo[t].first; const double want = todo[t].second, had = db[j];
                    FluePipeSpec best = res[j].spec; double bestErr = std::fabs(had - want); std::string how = "unchanged";
                    // the levers, in the voicer's order: the toe (its area), then the flue
                    struct Lever { const char* name; std::function<FluePipeSpec(double)> at; double lo, hi, start = 1.0; };
                    const FluePipeSpec s0 = res[j].spec;
                    std::vector<Lever> lv;
                    if (s0.toe > 0) lv.push_back({ "toe", [s0](double k) { FluePipeSpec q = s0; q.toe = (float)(s0.toe * std::sqrt(k)); return q; }, 0.15, 4.0 });
                    // an open toe is given a hole, from the body's bore down to 0.6 x the flue's area (k = hole / bore area):
                    // a hole of 2 % of the bore (the first bound) is still several times the flue of a large pipe and
                    // took 0.1 dB off (2026-11-16, Praestant 16' c#0 and d0, 3.5 dB too loud)
                    else if (had > want) lv.push_back({ "toe", [s0](double k) { FluePipeSpec q = s0; q.toe = (float)(2.0 * s0.radius * std::sqrt(k)); return q; },
                                                       std::min(0.5, 0.6 * (double)s0.h * s0.Wm / (M_PI * (double)s0.radius * s0.radius)), 1.0,
                                                       std::min(1.0, 3.0 * (double)s0.h * s0.Wm / (M_PI * (double)s0.radius * s0.radius)) });   // from a hole of 3 x the flue
                    if (had < want) lv.push_back({ "flue", [s0, withFlue](double k) { return withFlue(s0, k); }, 1.0, 3.0 });
                    // too loud, and the toe has not brought it down (a starved foot made the pipe slow or unsteady):
                    // the flue narrowed (the voicer sets the languid closer to the lower lip)
                    else lv.push_back({ "flue", [s0, withFlue](double k) { return withFlue(s0, k); }, 0.4, 1.0 });
                    for (const Lever& L : lv) {
                        if (bestErr <= 1.0) break;
                        // secant on log(k) against dB, from k = 1 (or the lever's end nearest)
                        double k0 = std::clamp(L.start, L.lo, L.hi), d0 = rmsDb(L.at(k0)), k1 = had < want ? std::min(L.hi, k0 * 1.5) : std::max(L.lo, k0 / 1.5);
                        for (int it = 0; it < 8; ++it) {
                            FluePipeSpec q = L.at(k1);
                            tuneTrim(q, jobs[j].hz, [](const FluePipeSpec& z) { return hearFlue(z, 0.012); });
                            const double d1 = rmsDb(q);
                            if (std::fabs(d1 - want) < bestErr && passes(j, q)) { best = q; bestErr = std::fabs(d1 - want); char b2[80]; snprintf(b2, sizeof b2, "%s x%.2f", L.name, k1); how = b2; }
                            if (bestErr <= 1.0) break;
                            const double sl = (d1 - d0) / (std::log(k1) - std::log(k0) + 1e-12);
                            double kn = std::fabs(sl) > 0.3 ? std::exp(std::log(k1) + (want - d1) / sl) : (d1 < want ? k1 * 1.5 : k1 / 1.5);
                            kn = std::clamp(kn, L.lo, L.hi); if (std::fabs(std::log(kn / k1)) < 0.01) break;
                            k0 = k1; d0 = d1; k1 = kn;
                        }
                    }
                    const double got = bestErr <= std::fabs(had - want) ? (had < want ? want - bestErr : want + bestErr) : had;
                    char buf[200]; snprintf(buf, sizeof buf, "[regulated: %.1f dB, the line of the rank %.1f dB: %s, now within %.1f dB] ", had, want, how.c_str(), bestErr);
                    std::lock_guard<std::mutex> lk(io);
                    fprintf(stderr, "  %s key %d: %+.1f dB off the line of its rank -> %s, %.1f dB\n", ranks[(size_t)jobs[j].rank].name.c_str(), jobs[j].key, had - want, how.c_str(), bestErr); (void)got;
                    if (how != "unchanged") { res[j].spec = best; res[j].why += buf; ckWrite(ckF, ckm, (uint32_t)j, true, res[j].cents0, res[j].centsEnd, res[j].fam, best, res[j].why); }
                }
            };
            std::vector<std::thread> tv; for (unsigned i = 0; i < nt; ++i) tv.emplace_back(reg); for (auto& t : tv) t.join();
        }
    }
    // reeds: designed from the Pasi resonators and tongues, tuned by wire and scroll
    struct ReedResult { ReedPipeSpec spec; double cents = 0; bool ok = false; std::string why; };
    std::vector<ReedResult> reedRes(reedJobs.size());
    std::atomic<size_t> nextR{0};
    std::vector<char> doneR(reedJobs.size(), 0);
    if (ck) {
        const size_t n = ckRead<ReedPipeSpec>(std::string(ck) + ".reed", [&](uint32_t j, bool ok, double a, double, int, const ReedPipeSpec& sp, const std::string& why) {
            if (j < reedJobs.size()) { reedRes[j].spec = sp; reedRes[j].ok = ok; reedRes[j].cents = a; reedRes[j].why = why; doneR[j] = 1; } });
        fprintf(stderr, "checkpoint: %zu reed pipes already done\n", n);
    }
    FILE* ckR = ck ? fopen((std::string(ck) + ".reed").c_str(), "ab") : nullptr;
    auto reedHz = [](const ReedPipeSpec& sp) {
        ReedPipeRT p; p.init(sp, 48000, 3); p.setChestPa(kChestPa); p.key(true);
        const int N = (int)(48000 * std::max(0.8, 0.3 + 60.0 / sp.target)), L = N / 2;
        std::vector<double> x(N); float o;
        for (int i = 0; i < N; ++i) { p.render(&o, 1); x[i] = o; }
        double e = 0; for (int i = N - L; i < N; ++i) e += x[i] * x[i];
        if (!(e / L > 1e-8)) return 0.0;
        // the pitch it actually speaks, searched widely (0.55 - 1.8 x the target), and its fundamental
        double best = 0, bf = 0;
        const int Lc = std::min(L, std::max(256, (int)(2.0 * 48000 / (1.8 * sp.target * (std::pow(2.0, 10 / 1200.0) - 1)))));   // as in analyse
        for (double c = -1000; c <= 1000; c += 10) { const double ft = sp.target * std::pow(2.0, c / 1200.0); const double a = toneAt(x, N - Lc, Lc, ft, 48000); if (a > best) { best = a; bf = ft; } }
        if (bf <= 0) return 0.0;
        double f = peakFreq(x, N - L, L, bf, 0.02, 48000);
        while (f / 2 > sp.target * 0.5 && toneAt(x, N - L, L, f / 2, 48000) > 0.3 * toneAt(x, N - L, L, f, 48000)) f = peakFreq(x, N - L, L, f / 2, 0.02, 48000);
        return f;
    };
    auto reedWorker = [&] {
        for (;;) {
            const size_t j = nextR++;
            if (j >= reedJobs.size()) return;
            const Job& jb = reedJobs[j];
            if (skip(jb.rank) || getenv("REEDS_SKIP")) { reedRes[j].ok = true; reedRes[j].cents = 0; continue; }
            if (const char* ks = getenv("KEYS")) { int a = 0, b = 127; sscanf(ks, "%d-%d", &a, &b); if (jb.key < a || jb.key > b) { reedRes[j].ok = true; continue; } }
            if (doneR[j]) continue;
            const bool oboe = detailOboe(ranks[(size_t)jb.rank].name);
            ReedResult& out = reedRes[j];
            const reed::ReedConstants& kk = reedK[oboe ? 1 : 0];
            ReedPipeSpec base = reed::design(*jb.s, jb.scaleKey, jb.hz, kk);
            // The tuning slot: a reed pipe sounds just below its resonator's first resonance (measured
            // 25 - 48 c below, mean 37 c: Nagy/Rucz, BME 2016, three pipes). A resonator cut to the
            // printed length whose resonance lies below that (the Oboe's are 25 - 40 c flat as plain
            // cones; the Trumpet 16' c2 200 c, the Bassoon's full-length ones up to 115 c) is cut or
            // slotted by the voicer: its resonance raised to 37 c above the pitch, at most 15 % of its
            // length (more than 8 % is named in the report).
            double slot = 1.0;
            // A half-length resonator supports the octave: there the same holds for 2 x the pitch (one
            // that lies below the octave takes the pipe over as it is tuned down to its note).
            const int hn = (base.nModes > 0 && base.f[0] > 0.8f * jb.hz && base.f[0] < 1.3f * jb.hz) ? 1
                         : (base.nModes > 0 && base.f[0] > 1.6f * jb.hz && base.f[0] < 2.6f * jb.hz) ? 2 : 0;
            if (hn > 0) {
                const double want = hn * jb.hz * std::pow(2.0, 37.0 / 1200.0);
                if (base.f[0] < want) {
                    slot = std::min(want / base.f[0], 1.0 / 0.85);
                    for (int i = 0; i < base.nModes; ++i) base.f[i] = (float)(base.f[i] * slot);
                    base.length = (float)(base.length / slot);
                }
            }
            // Voicing: the voicer sets the tongue (tuning wire) and its curve (opening) until the pipe
            // speaks steadily at its pitch. Settings tried around the resonator's first resonance (or the
            // pitch, for a fractional-length resonator); the steadiest strong one within 100 cents of the
            // pitch is kept, then the whole pipe is tuned (at most 6 %: the tuning scroll).
            const bool resonatorLed = base.nModes > 0 && base.f[0] > 0.7f * jb.hz && base.f[0] < 1.3f * jb.hz;
            const double ref = resonatorLed ? base.f[0] : jb.hz;
            // down to 0.7: a short treble resonator speaks below its resonance, the tongue leading
            static const double kTongue[] = { 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 1.0, 1.03, 1.06, 1.1, 1.15, 1.2, 1.3 };
            static const double kCurve[] = { 1.0, 0.8, 1.25, 0.65, 1.5 };   // gamma scaled: more curve (lower), less
            // each curve is voiced, tuned and checked in turn; the first that passes is kept, else the best
            bool anySpeaks = false; double bestErr = 1e18; ReedResult bestOut;
            auto make = [&](double r, double gs) {
                ReedPipeSpec c = base; c.fr = (float)(r * ref); c.x0 = (float)reed::restOpening(c, c.fr, kk.gamma * gs); reed::applyZeta(c, kk, kk.gamma * gs); c.xs = (float)(kk.closure * c.x0);
                return c;
            };
            for (double gs : kCurve) {
                double bestScore = -1; ReedPipeSpec best = base;
                // the curve follows the tongue: the same p / p_M (gamma) for any wire setting, times the scan
                struct Pt { double r, cents, rms; bool steady; };
                std::vector<Pt> pts;
                for (double r : kTongue) {
                    const ReedPipeSpec c = make(r, gs);
                    const Heard h = hearReed(c, 0.012);
                    const bool steady = h.speaks && h.spread <= 3 && h.tSpeech <= speechLimit(jb.hz);
                    const double cents = h.speaks ? 1200 * std::log2(h.f / jb.hz) : 1e9;
                    pts.push_back({ r, cents, h.rms, steady });
                    if (!steady || std::fabs(cents) > 100) continue;
                    const double score = h.rms / (1.0 + std::fabs(cents) / 50.0);
                    if (score > bestScore) { bestScore = score; best = c; }
                }
                // the tuning wire is continuous: between two settings that sound either side of the pitch
                // (both steadily) the wire is moved until it speaks at the pitch
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    Pt a = pts[i], b = pts[i + 1];
                    if (!a.steady || !b.steady || std::fabs(a.cents) > 700 || std::fabs(b.cents) > 700 || (a.cents > 0) == (b.cents > 0)) continue;
                    for (int it = 0; it < 7; ++it) {
                        const double r = std::sqrt(a.r * b.r);
                        const ReedPipeSpec c = make(r, gs);
                        const Heard h = hearReed(c, 0.012);
                        if (!(h.speaks && h.spread <= 3 && h.tSpeech <= speechLimit(jb.hz))) break;
                        const double cents = 1200 * std::log2(h.f / jb.hz);
                        if (std::fabs(cents) <= 100) {
                            const double score = h.rms / (1.0 + std::fabs(cents) / 50.0);
                            if (score > bestScore) { bestScore = score; best = c; }
                        }
                        if (std::fabs(cents) < 5) break;
                        if ((cents > 0) == (a.cents > 0)) a = { r, cents, h.rms, true }; else b = { r, cents, h.rms, true };
                    }
                }
                if (bestScore <= 0) continue;
                anySpeaks = true;
                ReedResult rr; rr.spec = best;
                tuneTrim(rr.spec, jb.hz, [](const ReedPipeSpec& q) { return hearReed(q, 0.012); });
                if (getenv("TRIMDBG")) {
                    std::lock_guard<std::mutex> lk(io);
                    fprintf(stderr, "%s key %d curve x%.2f:", ranks[(size_t)jb.rank].name.c_str(), jb.key, gs);
                    for (double tc = -60; tc <= 60; tc += 15) { ReedPipeSpec q = rr.spec; q.trim = (float)(rr.spec.trim * std::pow(2.0, tc / 1200.0)); const Heard h = hearReed(q, 0.012);
                        fprintf(stderr, "  %+.0f:%s%+.1f/%.1f", tc, h.speaks ? "" : "X", h.speaks ? 1200 * std::log2(h.f / jb.hz) : 0.0, h.spread); }
                    fprintf(stderr, "\n");
                }
                const Heard a = hearReed(rr.spec, 0.012), b = hearReed(rr.spec, 0.002);
                rr.cents = a.speaks ? 1200 * std::log2(a.f / jb.hz) : 1e9;
                const double cb = b.speaks ? 1200 * std::log2(b.f / jb.hz) : 1e9;
                { char buf[260]; snprintf(buf, sizeof buf, "slot %.1f%% curve x%.2f | rms %.3f growth %+.1fdB | slow pallet %s %+.2fc spread %.1f speech %.2fs | fast pallet %s %+.1fc spread %.1f | trim %.3f",
                      100.0 * (1.0 - 1.0 / slot), gs, a.rms, a.growth, a.speaks ? "speaks" : "NOT SPEAKING", rr.cents, a.spread, a.tSpeech, b.speaks ? "speaks" : "SILENT", b.speaks ? cb : 0.0, b.spread, rr.spec.trim); rr.why = buf; }
                std::string wnr;
                const bool robustR = a.speaks && b.speaks && windRobust(rr.spec, jb.hz, reedAt, wnr);
                rr.why += " | " + wnr;
                rr.ok = robustR && a.speaks && b.speaks && a.spread <= 3 && b.spread <= 3 && std::fabs(rr.cents) < 0.5
                        && std::fabs(cb) < 2 && a.tSpeech <= speechLimit(jb.hz) && std::fabs(rr.spec.trim - 1.f) < 0.06f;
                const double err = rr.ok ? -1.0 : (a.speaks && b.speaks ? std::fabs(rr.cents) + std::fabs(cb) + 10.0 * (std::max(0.0, a.spread - 3) + std::max(0.0, b.spread - 3)) : 1e9);
                if (err < bestErr) { bestErr = err; bestOut = rr; }
                if (rr.ok) break;
            }
            out.cents = 1e9; out.ok = false;
            if (!anySpeaks) { out.spec = base; char buf[120]; snprintf(buf, sizeof buf, "slot %.1f%% | no tongue setting speaks steadily within 100 c of the pitch", 100.0 * (1.0 - 1.0 / slot)); out.why = buf; }
            else { out.spec = bestOut.spec; out.cents = bestOut.cents; out.ok = bestOut.ok; out.why = bestOut.why; }
            ckWrite(ckR, ckm, (uint32_t)j, out.ok, out.cents, 0.0, 0, out.spec, out.why);
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
          fprintf(f, "    { \"%s\", %d, %d, %s, %d, %.2ff, %d, \"%s\" },\n", ranks[r].name.c_str(), ranks[r].div, ranks[r].stop, ranks[r].reed ? "true" : "false", jf, ranks[r].detune, ranks[r].first, ranks[r].s->name);
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
        fprintf(rep, "%-32s key %3d %8.2f Hz reed tongue %.2f mm L %.1f mm fr %.1f Hz  final %+7.2f c %s %s\n", ranks[(size_t)reedJobs[j].rank].name.c_str(), reedJobs[j].key, reedJobs[j].hz, r.spec.t * 1e3, r.spec.L * 1e3, r.spec.fr, r.cents, r.ok ? "ok" : "FAIL", r.why.c_str());
    }
    if (rep != stderr) fclose(rep);
    fprintf(stderr, "%d of %zu reed pipes fail (do not speak steadily at pitch, in time, in tune)\n", badR, reedJobs.size());
    if (bad || badR) { fprintf(stderr, "THE ORGAN IS NOT COMPLETE: %d pipes fail - see the report\n", bad + badR); return 2; }
    return 0;
}
