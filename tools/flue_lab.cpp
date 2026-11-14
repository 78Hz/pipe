// Pipe - flue model laboratory: designs Opus 14 pipes from the tables, plays them through the
// jet-drive model and measures what comes out (pitch, harmonics, attack, level, overblowing).
// Not part of the plugin build. Usage: flue_lab <outdir>
#include "engine/FlueModel.hpp"
#include <cstdio>
#include <vector>
#include <complex>
#include <string>
using namespace pipe;
using namespace pipe::flue;

static void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) { size_t bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / (double)len; const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; ++j) { auto u = a[i + j], v = a[i + j + len / 2] * w; a[i + j] = u + v; a[i + j + len / 2] = u - v; w *= wl; } }
    }
}
struct Spectrum { std::vector<double> mag; double df; };
static Spectrum spectrum(const std::vector<double>& x, size_t from, size_t len, double fs) {
    size_t N = 1; while (N < len * 4) N <<= 1;
    std::vector<std::complex<double>> a(N);
    for (size_t i = 0; i < len; ++i) a[i] = x[from + i] * (0.5 - 0.5 * std::cos(2 * M_PI * (double)i / (double)(len - 1)));
    fft(a);
    Spectrum s; s.df = fs / (double)N; s.mag.resize(N / 2);
    for (size_t i = 0; i < N / 2; ++i) s.mag[i] = std::abs(a[i]);
    return s;
}
// peak in [f1, f2]: interpolated frequency and magnitude
static void peak(const Spectrum& s, double f1, double f2, double& f, double& m) {
    size_t i1 = (size_t)std::max(1.0, f1 / s.df), i2 = std::min(s.mag.size() - 2, (size_t)(f2 / s.df));
    size_t k = i1; for (size_t i = i1; i <= i2; ++i) if (s.mag[i] > s.mag[k]) k = i;
    const double a = std::log(s.mag[k - 1] + 1e-30), b = std::log(s.mag[k] + 1e-30), c = std::log(s.mag[k + 1] + 1e-30);
    const double p = 0.5 * (a - c) / (a - 2 * b + c);
    f = ((double)k + p) * s.df; m = std::exp(b - 0.25 * (a - c) * p);
}

static std::vector<double> play(const Design& d, const Voicing& v, double fs, double seconds, double pScale = 1.0, double ramp = 0.020) {
    FlueSim sim; sim.init(d, v, fs);
    std::vector<double> out((size_t)(seconds * fs));
    const size_t off = (size_t)((seconds - 0.35) * fs);
    for (size_t n = 0; n < out.size(); ++n) {
        const double t = (double)n / fs;
        double g = t < ramp ? 0.5 - 0.5 * std::cos(M_PI * t / ramp) : 1.0;
        if (n >= off) { const double tr = (double)(n - off) / fs; g = tr < ramp ? 0.5 + 0.5 * std::cos(M_PI * tr / ramp) : 0.0; }
        out[n] = sim.tick(d.footPa * pScale * g);
    }
    return out;
}

static void writeWav(const std::string& path, const std::vector<float>& x, int fs) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const unsigned n = (unsigned)x.size(), bytes = n * 2;
    auto w32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); }; auto w16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32((unsigned)fs); w32((unsigned)fs * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    for (float s : x) { const int v = (int)std::lround(std::max(-1.f, std::min(1.f, s)) * 32767.f); w16((unsigned short)(short)v); }
    std::fclose(f);
}

struct Case { opus14::Division div; const char* name; int key; const char* label; };

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const double fs = 48000;
    const Case cases[] = {
        { opus14::kHW, "Octave 8'", 36, "HW Octave 8' C" },
        { opus14::kHW, "Octave 8'", 60, "HW Octave 8' c1" },
        { opus14::kHW, "Octave 8'", 84, "HW Octave 8' c3" },
        { opus14::kHW, "Salicional 8'", 60, "HW Salicional 8' c1" },
        { opus14::kUW, "Gamba 8'", 60, "UW Gamba 8' c1" },
        { opus14::kOW, "Gedeckt 8'", 60, "OW Gedeckt 8' c1" },
        { opus14::kHW, "Rohrflote 8'", 60, "HW Rohrflote 8' c1" },
        { opus14::kHW, "Spitzflote 4'", 60, "HW Spitzflote 4' c1" },
        { opus14::kUW, "Harmonic Flute 8'", 60, "UW Harmonic Flute 8' c1" },
        { opus14::kUW, "Harmonic Flute 8'", 72, "UW Harmonic Flute 8' c2 (harmonic)" },
        { opus14::kPed, "Subbass 16'", 36, "Ped Subbass 16' C" },
        { opus14::kHW, "Superoctave 2'", 84, "HW Superoctave 2' c3" },
    };
    std::vector<std::vector<double>> sounds; std::vector<std::string> names;
    for (const Case& c : cases) {
        const opus14::Stop* st = nullptr;
        for (const auto& s : opus14::kStops) if (s.div == c.div && !std::strcmp(s.name, c.name)) st = &s;
        if (!st) { std::printf("missing %s\n", c.name); continue; }
        Voicing v; v.ising = familyOf(*st) == kString ? 2.8 : 2.0;
        Design d = design(*st, c.key, v);
        const VoicingReport vr = voice(d, v, fs);
        std::vector<double> x = play(d, v, fs, std::max(1.6, settleSeconds(d) + 0.9));
        const size_t a0 = (size_t)(settleSeconds(d) * fs), aLen = (size_t)(0.5 * fs);
        double f = peakFreq(x, a0, aLen, d.target, 0.04, fs);
        const Spectrum sp = spectrum(x, a0, aLen, fs);
        double fstrong, mstrong; peak(sp, d.target * 0.4, std::min(20000.0, d.target * 8.5), fstrong, mstrong);
        double rms = 0; for (size_t i = a0; i < a0 + aLen; ++i) rms += x[i] * x[i];
        rms = std::sqrt(rms / (0.5 * fs));
        // attack: 2 ms RMS envelope reaching 90 % of steady
        double t90 = -1;
        for (size_t i = 0; i + 96 < a0; i += 48) {
            double e = 0; for (size_t j = 0; j < 96; ++j) e += x[i + j] * x[i + j];
            if (std::sqrt(e / 96) > 0.9 * rms) { t90 = (double)i / fs; break; }
        }
        std::printf("\n%s   target %.2f Hz   voiced: %s I %.2f (model max efficiency at I %.2f)%s  (pitch before cutting %+.0f c, %d tries)\n", c.label, d.target,
            vr.ok ? "yes" : "NO", vr.ising, vr.isingOpt, vr.hole > 0 ? (std::string("  hole ") + std::to_string(vr.hole * 1e3).substr(0, 4) + " mm").c_str() : "", vr.centsBeforeCut, vr.tries);
        std::printf("  body L %.1f mm  radius %.2f mm  shape %d  mode %d  mouth %.1f x %.2f mm  flue %.3f mm  I %.2f  foot %.0f Pa (%.1f mm)%s\n",
            d.g.body[0].L * 1e3, d.radius * 1e3, (int)d.shape, d.speakMode, d.mouthWidth * 1e3, d.cutup * 1e3, d.flue * 1e3, d.ising,
            d.footPa, d.footPa / 9.80665, d.toe > 0 ? "  (toe hole)" : "");
        std::printf("  resonances/f1:");
        for (size_t i = 0; i < std::min<size_t>(6, d.modes.size()); ++i) std::printf(" %.3f", d.modes[i].f / d.modes[0].f);
        std::printf("   Q1 %.0f\n", d.modes.empty() ? 0.0 : d.modes[0].Q);
        std::printf("  sounds %.2f Hz (%+.1f c)  strongest %.1f Hz  level %.1f dB SPL @1 m  attack(90%%) %.0f ms\n",
            f, 1200 * std::log2(f / d.target), fstrong, 20 * std::log10(rms / 2e-5), t90 * 1e3);
        std::printf("  harmonics dB re H1:");
        double h1f, h1; peak(sp, f * 0.97, f * 1.03, h1f, h1);
        for (int k = 2; k <= 10 && f * k < 20000; ++k) { double hf, hm; peak(sp, f * k * 0.98, f * k * 1.02, hf, hm); std::printf(" %+.0f", 20 * std::log10(hm / h1)); }
        std::printf("\n  pressure sweep (x chest): ");
        for (double ps : { 0.4, 0.7, 1.0, 1.5, 2.0, 3.0 }) {
            const Steady y = blow(d, v, fs, ps);
            const Spectrum s2 = spectrum(y.x, y.from, y.len, fs);
            double ff, mm; peak(s2, d.target / d.speakMode * 0.6, d.target * 4.5, ff, mm);
            double r = 0; for (size_t i = y.from; i < y.x.size(); ++i) r += y.x[i] * y.x[i];
            r = std::sqrt(r / (double)y.len);
            std::printf(" %.1f:%s%.2f(%.0fdB)", ps, r < 1e-4 ? "silent " : "", ff / d.target, 20 * std::log10(r / 2e-5 + 1e-9));
        }
        std::printf("\n");
        if (!d.note.empty()) std::printf("  assumed: %s\n", d.note.c_str());
        sounds.push_back(x); names.push_back(c.label);
    }
    // one common gain for all files, so their levels compare
    double peakAll = 0; for (auto& x : sounds) for (double s : x) peakAll = std::max(peakAll, std::fabs(s));
    for (size_t i = 0; i < sounds.size(); ++i) {
        std::vector<float> y; for (double s : sounds[i]) y.push_back((float)(0.8 * s / peakAll));
        std::string fn = names[i]; for (auto& ch : fn) if (!isalnum((unsigned char)ch)) ch = '_';
        writeWav(dir + "/" + fn + ".wav", y, (int)fs);
    }
    // all in one file, in the order above, 0.5 s apart, same gain
    std::vector<float> all;
    for (auto& x : sounds) { for (double s : x) all.push_back((float)(0.8 * s / peakAll)); all.insert(all.end(), (size_t)(0.5 * fs), 0.f); }
    writeWav(dir + "/flue_lab_all.wav", all, (int)fs);
    return 0;
}
