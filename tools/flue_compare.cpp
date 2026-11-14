// Pipe - render Opus 14 flue pipes for comparison with reference recordings.
// Voicing is fixed (Ising number per family, Pasi's toe holes), the body is cut to pitch;
// no search. Usage: flue_compare <outdir> "<div>|<stop>|<midi key>" ...   div = HW OW UW Ped
#include "engine/FlueModel.hpp"
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
using namespace pipe;
using namespace pipe::flue;

static void writeWav(const std::string& path, const std::vector<double>& x, double gain, int fs) {
    FILE* f = std::fopen(path.c_str(), "wb"); if (!f) return;
    const unsigned n = (unsigned)x.size(), bytes = n * 2;
    auto w32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); }; auto w16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(1); w32((unsigned)fs); w32((unsigned)fs * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    for (double s : x) { const int v = (int)std::lround(std::max(-1.0, std::min(1.0, s * gain)) * 32767.0); w16((unsigned short)(short)v); }
    std::fclose(f);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const double fs = 48000;
    for (int a = 2; a < argc; ++a) {
        std::string spec = argv[a];
        const size_t p1 = spec.find('|'), p2 = spec.rfind('|');
        const std::string dv = spec.substr(0, p1), name = spec.substr(p1 + 1, p2 - p1 - 1);
        const int key = std::atoi(spec.substr(p2 + 1).c_str());
        const opus14::Division div = dv == "HW" ? opus14::kHW : dv == "OW" ? opus14::kOW : dv == "UW" ? opus14::kUW : opus14::kPed;
        const opus14::Stop* st = nullptr;
        for (const auto& s : opus14::kStops) if (s.div == div && name == s.name) st = &s;
        if (!st) { std::printf("missing %s\n", spec.c_str()); continue; }
        Voicing v; v.ising = familyOf(*st) == kString ? 2.8 : 2.0;
        Design d = design(*st, key, v);
        // harmonic pipes: hole at half the speaking length, 0.2 D (ASSUMPTION, size unpublished)
        if (d.speakMode == 2) {
            const double dia = 2 * d.radius;
            d.g.holes = { { 0.5 * d.g.body[0].L, M_PI * 0.25 * (0.2 * dia) * (0.2 * dia), d.g.wall } };
            recut(d, 1.0);
        }
        const double cents = cutToPitch(d, v, fs);
        FlueSim sim; sim.init(d, v, fs);
        const double hold = 2.5, tail = 2.0;
        std::vector<double> x((size_t)((0.1 + hold + tail) * fs), 0.0);
        for (size_t n = (size_t)(0.1 * fs); n < x.size(); ++n) {
            if (n == (size_t)(0.1 * fs)) sim.key(true, kChestPa);
            if (n == (size_t)((0.1 + hold) * fs)) sim.key(false, kChestPa);
            x[n] = sim.tickKey();
        }
        double rms = 0; for (size_t i = (size_t)(1.5 * fs); i < (size_t)(2.5 * fs); ++i) rms += x[i] * x[i];
        rms = std::sqrt(rms / fs);
        std::string fn = dv + "_" + name + "_" + std::to_string(key); for (auto& ch : fn) if (!isalnum((unsigned char)ch)) ch = '_';
        writeWav(dir + "/" + fn + ".wav", x, 0.1 / std::max(1e-9, rms), (int)fs);   // each file at -20 dBFS rms; level printed
        std::printf("%-26s key %3d  %7.2f Hz  I %.2f flue %.3f mm foot %.0f Pa  cut %+.0f c  level %.1f dB SPL @1m  -> %s.wav\n",
            spec.c_str(), key, d.target, d.ising, d.flue * 1e3, d.footPa, cents, 20 * std::log10(rms / 2e-5), fn.c_str());
    }
}
