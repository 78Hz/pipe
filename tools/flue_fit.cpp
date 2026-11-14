// Pipe - fit the jet constants (beta, gamma, kd, y0/b) once for the whole organ against
// reference recordings (Ernoult 2016 ch. 7 method: fit to measurement, then hold fixed).
// Targets: data/reference/targets_grandorgue_demo.txt (family key holdout usable H2 H3 H4 H5 attack).
// Usage: flue_fit <targets> [evaluate beta gamma kd y0b sign]
#include "engine/FlueModel.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <random>
#include <map>
using namespace pipe;
using namespace pipe::flue;

struct Target { std::string fam; int key; bool hold, ok; double H[4], attack; };
struct Case { Target t; Design d; Voicing v; };

static const opus14::Stop* stopFor(const std::string& fam) {
    const char* name = fam == "principal" ? "Octave 8'" : fam == "string" ? "Gamba 8'" : fam == "harmflute" ? "Harmonic Flute 8'" : "Gedeckt 8'";
    const opus14::Division dv = fam == "principal" ? opus14::kHW : fam == "stopped" ? opus14::kOW : opus14::kUW;
    for (const auto& s : opus14::kStops) if (s.div == dv && !std::strcmp(s.name, name)) return &s;
    return nullptr;
}

static int famIndex(const std::string& f) { return f == "principal" ? 0 : f == "string" ? 1 : f == "harmflute" ? 2 : 3; }
static bool perFamily = false;
static double famY0[4];
struct Result { bool speaks; double f, H[4], attack, err; };

static Result measure(const Case& c, const JetParams& jp, std::vector<double>* wav = nullptr) {
    const double fs = 48000;
    FlueSim sim; sim.init(c.d, c.v, fs); sim.jp = jp;
    const double T = settleSeconds(c.d) + 0.4;
    const size_t N = (size_t)(T * fs);
    std::vector<double> m(N), t(N);
    sim.key(true, kChestPa);
    for (size_t n = 0; n < N; ++n) { const double y = sim.tickKey(); m[n] = sim.mouthOut; t[n] = sim.topOut; if (wav) wav->push_back(y); }
    Result r{};
    const size_t from = N - (size_t)(0.35 * fs), len = (size_t)(0.35 * fs);
    // power sum of the two sources at each partial
    auto partial = [&](double f) { const double a = toneAt(m, from, len, f, fs), b = toneAt(t, from, len, f, fs); return std::sqrt(a * a + b * b); };
    double mag = 0;
    r.f = peakFreq(m, from, len, c.d.target, 0.04, fs, &mag);
    const double h1 = partial(r.f);
    double strongest = h1;
    for (int k = 2; k <= 6; ++k) strongest = std::max(strongest, partial(k * r.f));
    for (const Mode& md : c.d.modes) if (md.f < 8 * c.d.target) { double mm; peakFreq(m, from, len, md.f, 0.03, fs, &mm); strongest = std::max(strongest, mm); }
    double rms = 0; for (size_t i = from; i < N; ++i) rms += m[i] * m[i] + t[i] * t[i];
    rms = std::sqrt(rms / (double)len);
    r.speaks = rms > 1e-4 && 20 * std::log10(h1 / strongest) > -20;
    for (int k = 0; k < 4; ++k) r.H[k] = 20 * std::log10(partial((k + 2) * r.f) / h1 + 1e-12);
    // attack: 5 ms power envelope, 10 % -> 90 % of the steady level
    const size_t w = (size_t)(0.005 * fs);
    std::vector<double> e(N / w);
    for (size_t i = 0; i < e.size(); ++i) { double s = 0; for (size_t j = 0; j < w; ++j) s += m[i * w + j] * m[i * w + j] + t[i * w + j] * t[i * w + j]; e[i] = std::sqrt(s / (double)w); }
    const double steady = e[e.size() - 3];
    size_t i10 = 0, i90 = 0;
    for (size_t i = 0; i < e.size(); ++i) if (e[i] > 0.1 * steady) { i10 = i; break; }
    for (size_t i = 0; i < e.size(); ++i) if (e[i] > 0.9 * steady) { i90 = i; break; }
    r.attack = (double)(i90 - i10) * (double)w / fs * c.d.target;
    // error: harmonics in dB (each capped at 20 dB), attack as 10 log2 of the ratio
    double err = 0;
    for (int k = 0; k < 4; ++k) { const double d = std::min(20.0, std::fabs(r.H[k] - c.t.H[k])); err += d * d; }
    const double da = 10 * std::log2(std::max(1.0, r.attack) / std::max(1.0, c.t.attack));
    err += std::min(400.0, da * da);
    if (!r.speaks) err = 4 * 400 + 400;
    r.err = err;
    return r;
}

static double total(const std::vector<Case>& cs, const JetParams& jp, bool holdout, std::vector<Result>* out = nullptr) {
    std::vector<Result> res(cs.size());
    std::vector<std::thread> th;
    for (size_t i = 0; i < cs.size(); ++i) th.emplace_back([&, i] { if (cs[i].t.ok && cs[i].t.hold == holdout) { JetParams j = jp; j.y0b = famY0[famIndex(cs[i].t.fam)]; res[i] = measure(cs[i], j); } });
    for (auto& x : th) x.join();
    double s = 0; int n = 0;
    for (size_t i = 0; i < cs.size(); ++i) if (cs[i].t.ok && cs[i].t.hold == holdout) { s += res[i].err; ++n; }
    if (out) *out = res;
    return n ? s / n : 0;
}

static JetParams fromX(const double* x, double sign) {
    JetParams p; p.fitted = true; p.sign = sign;
    p.beta = std::exp(x[0]); p.gamma = 1.0 / (1.0 + std::exp(-x[1])); p.kd = std::exp(x[2]); p.y0b = x[3];
    for (int i = 0; i < 4; ++i) famY0[i] = perFamily ? x[3 + i] : x[3];
    return p;
}

int main(int argc, char** argv) {
    perFamily = std::getenv("FIT_FAMILY_Y0") != nullptr;
    for (double& y : famY0) y = 0.25;
    std::ifstream in(argv[1]);
    std::vector<Case> cs;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line); Target t; int h, ok;
        ss >> t.fam >> t.key >> h >> ok >> t.H[0] >> t.H[1] >> t.H[2] >> t.H[3] >> t.attack;
        t.hold = h; t.ok = ok;
        Case c; c.t = t; c.v.ising = t.fam == "string" ? 2.8 : 2.0;
        const opus14::Stop* st = stopFor(t.fam);
        c.d = design(*st, t.key, c.v);
        if (c.d.speakMode == 2) {
            const double dia = 2 * c.d.radius;
            c.d.g.holes = { { 0.5 * c.d.g.body[0].L, M_PI * 0.25 * (0.2 * dia) * (0.2 * dia), c.d.g.wall } };
            recut(c.d, 1.0);
        }
        cs.push_back(c);
    }
    std::fprintf(stderr, "%zu cases designed\n", cs.size());
    auto report = [&](const JetParams& jp) {
        for (int hold = 0; hold <= 1; ++hold) {
            std::vector<Result> res; const double e = total(cs, jp, hold, &res);
            std::printf("%s  mean error %.0f\n", hold ? "HOLD-OUT" : "FIT", e);
            for (size_t i = 0; i < cs.size(); ++i) {
                const auto& c = cs[i]; if (!c.t.ok || c.t.hold != (bool)hold) continue;
                const auto& r = res[i];
                std::printf("  %-9s %3d %s  model H2..5 %+4.0f %+4.0f %+4.0f %+4.0f attack %5.1f | ref %+4.0f %+4.0f %+4.0f %+4.0f attack %5.1f\n",
                    c.t.fam.c_str(), c.t.key, r.speaks ? "  " : "!!", r.H[0], r.H[1], r.H[2], r.H[3], r.attack, c.t.H[0], c.t.H[1], c.t.H[2], c.t.H[3], c.t.attack);
            }
        }
    };
    if (argc > 2 && std::string(argv[2]) == "evaluate") {
        JetParams jp; jp.fitted = true; jp.beta = atof(argv[3]); jp.gamma = atof(argv[4]); jp.kd = atof(argv[5]); jp.y0b = atof(argv[6]); jp.sign = atof(argv[7]);
        for (int i = 0; i < 4; ++i) famY0[i] = argc > 8 + i ? atof(argv[8 + i]) : jp.y0b;
        report(jp); return 0;
    }
    // coarse search over both signs, then Nelder-Mead from the best point
    std::mt19937 rng(7);
    const int D = perFamily ? 7 : 4;
    double bestE = 1e18, bestX[7] = { 0 }, bestSign = 1;
    for (double sign : { 1.0, -1.0 })
        for (int i = 0; i < 40; ++i) {
            double x[7] = { std::log(0.03 + 0.37 * std::uniform_real_distribution<>(0, 1)(rng)), std::uniform_real_distribution<>(-1.5, 1.5)(rng),
                            std::uniform_real_distribution<>(-1.4, 1.4)(rng), 0, 0, 0, 0 };
            for (int k = 3; k < D; ++k) x[k] = std::uniform_real_distribution<>(0.0, 1.5)(rng);
            const double e = total(cs, fromX(x, sign), false);
            if (e < bestE) { bestE = e; std::copy(x, x + D, bestX); bestSign = sign; std::fprintf(stderr, "  search %d sign %+.0f err %.0f\n", i, sign, e); }
        }
    // Nelder-Mead
    double P[8][7], F[8];
    for (int i = 0; i <= D; ++i) { for (int j = 0; j < D; ++j) P[i][j] = bestX[j] + (i == j + 1 ? 0.3 : 0.0); F[i] = total(cs, fromX(P[i], bestSign), false); }
    for (int it = 0; it < (perFamily ? 160 : 90); ++it) {
        int o[8]; for (int i = 0; i <= D; ++i) o[i] = i;
        std::sort(o, o + D + 1, [&](int a, int b) { return F[a] < F[b]; });
        double c[7] = { 0 }; for (int i = 0; i < D; ++i) for (int j = 0; j < D; ++j) c[j] += P[o[i]][j] / D;
        const int w = o[D];
        double xr[7], xe[7], xc[7];
        for (int j = 0; j < D; ++j) xr[j] = c[j] + (c[j] - P[w][j]);
        const double fr = total(cs, fromX(xr, bestSign), false);
        if (fr < F[o[0]]) { for (int j = 0; j < D; ++j) xe[j] = c[j] + 2 * (c[j] - P[w][j]); const double fe = total(cs, fromX(xe, bestSign), false);
            if (fe < fr) { std::copy(xe, xe + D, P[w]); F[w] = fe; } else { std::copy(xr, xr + D, P[w]); F[w] = fr; } }
        else if (fr < F[o[D - 1]]) { std::copy(xr, xr + D, P[w]); F[w] = fr; }
        else { for (int j = 0; j < D; ++j) xc[j] = c[j] + 0.5 * (P[w][j] - c[j]); const double fc = total(cs, fromX(xc, bestSign), false);
            if (fc < F[w]) { std::copy(xc, xc + D, P[w]); F[w] = fc; }
            else for (int i = 1; i <= D; ++i) { for (int j = 0; j < D; ++j) P[o[i]][j] = P[o[0]][j] + 0.5 * (P[o[i]][j] - P[o[0]][j]); F[o[i]] = total(cs, fromX(P[o[i]], bestSign), false); } }
        if (it % 10 == 0) std::fprintf(stderr, "  NM %d best %.0f\n", it, *std::min_element(F, F + D + 1));
    }
    const int b = (int)(std::min_element(F, F + D + 1) - F);
    const JetParams jp = fromX(P[b], bestSign);
    std::printf("FITTED beta %.4f gamma %.4f kd %.4f sign %+.0f  y0/b principal %.3f string %.3f harmflute %.3f stopped %.3f\n", jp.beta, jp.gamma, jp.kd, jp.sign, famY0[0], famY0[1], famY0[2], famY0[3]);
    report(jp);
}
