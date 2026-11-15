// Render reed pipes of one Opus 14 reed stop with given constants (tuned first), for matching
// against references (tools/ref/match.py, MODE=reed).
//   reed_eval outdir widthRatio openRatio Qr bootLitres8 closure key [key ...]     env STOP, DIV
// Writes outdir/<key>.f32: stereo float32 (radiated, 0), 48 kHz, 3.2 s from key-down.
#include "engine/ReedModel.hpp"
#include "engine/FlueModel.hpp"
#include <cstdlib>
#include <thread>
using namespace pipe;
static double soundHz(const ReedPipeSpec& sp) {
    ReedPipeRT p; p.init(sp, 48000, 3); p.setChestPa(79 * 9.80665); p.key(true);
    const int N = (int)(48000 * std::max(0.8, 0.3 + 60.0 / sp.target)), L = N / 2;
    std::vector<double> x(N); float o;
    for (int i = 0; i < N; ++i) { p.render(&o, 1); x[i] = o; }
    double e = 0; for (int i = N - L; i < N; ++i) e += x[i] * x[i];
    if (!(e / L > 1e-8)) return 0;
    const double f = flue::peakFreq(x, N - L, L, sp.target, 0.15, 48000);
    const double h1 = flue::toneAt(x, N - L, L, f, 48000), hh = flue::toneAt(x, N - L, L, f / 2, 48000);
    return h1 > hh ? f : 0;
}
int main(int argc, char** argv) {
    const char* out = argv[1];
    reed::ReedConstants k; k.widthRatio = atof(argv[2]); k.openRatio = atof(argv[3]); k.Qr = atof(argv[4]); k.bootLitres8 = atof(argv[5]); k.closure = atof(argv[6]);
    const char* stop = getenv("STOP") ? getenv("STOP") : "Trompette 8'";
    const int div = getenv("DIV") ? atoi(getenv("DIV")) : 2;
    std::vector<int> keys; for (int a = 7; a < argc; ++a) keys.push_back(atoi(argv[a]));
    auto one = [&](int key) {
        for (int i = 0; i < opus14::kNumStops; ++i) {
            const auto& s = opus14::kStops[i];
            if (s.div != (opus14::Division)div || std::strcmp(s.name, stop)) continue;
            const double hz = 440.0 * std::pow(2.0, (key - 69) / 12.0) * 8.0 / s.feet;
            ReedPipeSpec sp = reed::design(s, key, hz, k);
            reed::tune(sp, soundHz);
            ReedPipeRT p; p.init(sp, 48000, 5); p.setChestPa(79 * 9.80665); p.key(true);
            const size_t N = (size_t)(3.2 * 48000); std::vector<float> buf(2 * N); float o;
            for (size_t n = 0; n < N; ++n) { p.render(&o, 1); buf[2 * n] = o; buf[2 * n + 1] = 0.f; }
            char path[512]; snprintf(path, sizeof path, "%s/%d.f32", out, key); FILE* f = fopen(path, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
        }
    };
    std::vector<std::thread> th; for (int kk : keys) th.emplace_back(one, kk); for (auto& t : th) t.join();
}
