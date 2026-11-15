// Render pipes of one Opus 14 flue stop on the real-time model with given jet constants, for
// matching against references (tools/ref/match.py).
//   flue_eval outdir y0b lpk gscale kd noise key [key ...]      env STOP (name), DIV (opus14 division)
// Writes outdir/<key>.f32: stereo float32 (mouth, top), 48 kHz, 3.2 s from key-down.
#include "engine/FlueModel.hpp"
#include <cstdlib>
#include <thread>
using namespace pipe; using namespace pipe::flue;
int main(int argc, char** argv) {
    const char* out = argv[1];
    JetParams jp = matchedPrincipal();
    jp.y0b = atof(argv[2]); jp.lpk = atof(argv[3]); jp.gain = -atof(argv[4]); jp.kd = atof(argv[5]); jp.noise = atof(argv[6]);
    if (getenv("VORTEX")) jp.vortex = atof(getenv("VORTEX"));   // (set by tools/ref/match.py)
    const char* stop = getenv("STOP") ? getenv("STOP") : "Octave 8'";
    const int div = getenv("DIV") ? atoi(getenv("DIV")) : 0;
    std::vector<int> keys; for (int a = 7; a < argc; ++a) keys.push_back(atoi(argv[a]));
    auto one = [&](int key) {
        for (int i = 0; i < opus14::kNumStops; ++i) {
            const auto& s = opus14::kStops[i];
            if (s.div != (opus14::Division)div || std::strcmp(s.name, stop)) continue;
            Voicing v; v.ising = familyOf(s) == kString ? 2.8 : 2.0;
            const double hz = 440.0 * std::pow(2.0, (key - 69) / 12.0) * 8.0 / s.feet;
            const Design d = designAt(s, key, hz, v);
            FluePipeRT p; p.init(specOf(d, jp), 48000, 11); p.setChestPa(kChestPa);
            if (getenv("PALLET")) p.setPallet(atof(getenv("PALLET")), atof(getenv("PALLET")));
            p.key(true);
            const size_t N = (size_t)(3.2 * 48000); std::vector<float> buf(2 * N); float m, t;
            for (size_t n = 0; n < N; ++n) { p.render(&m, &t, 1); buf[2 * n] = m; buf[2 * n + 1] = t; }
            char path[512]; snprintf(path, sizeof path, "%s/%d.f32", out, key); FILE* f = fopen(path, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
        }
    };
    std::vector<std::thread> th; for (int k : keys) th.emplace_back(one, k); for (auto& t : th) t.join();
}
