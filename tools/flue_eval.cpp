// Render Octave 8' pipes for the reference match: turbulent jet, Verge source spacing on the jet at
// the lip, with the unmeasured constants given on the command line (one value each for the stop).
//   flue_eval outdir y0b lpk gscale kd noise key [key ...]
// gscale multiplies the jet gain 2 (b/h) / 0.10 (swing a = 2b at u/U = 0.10). Writes outdir/<key>.wav
// (stereo: mouth, top; 48 kHz float32 raw, 3.2 s from key-down, chest wind from t = 0).
#include "engine/FlueModel.hpp"
#include <cstdlib>
using namespace pipe; using namespace pipe::flue;
int main(int argc, char** argv) {
    const char* out = argv[1];
    JetParams jp; jp.fitted = true; jp.turbulent = true; jp.ddRule = 1; jp.sign = -1;
    jp.y0b = atof(argv[2]); jp.lpk = atof(argv[3]); const double gs = atof(argv[4]); jp.kd = atof(argv[5]); jp.noise = atof(argv[6]);
    const char* stop = getenv("STOP") ? getenv("STOP") : "Octave 8'";
    for (int a = 7; a < argc; ++a) {
        const int key = atoi(argv[a]);
        for (auto& s : opus14::kStops) if (s.div == opus14::kHW && !strcmp(s.name, stop)) {
            Voicing v; v.ising = familyOf(s) == kString ? 2.8 : 2.0; Design d = design(s, key, v);
            FlueSim sim; sim.init(d, v, 48000); JetParams j = jp; j.gain = gs * 2.0 * (sim.b / d.flue) / 0.10; sim.jp = j;
            sim.key(true, kChestPa);
            const size_t N = (size_t)(3.2 * 48000); std::vector<float> buf(2 * N);
            for (size_t n = 0; n < N; ++n) { sim.tickKey(); buf[2 * n] = (float)sim.mouthOut; buf[2 * n + 1] = (float)sim.topOut; }
            char p[512]; snprintf(p, sizeof p, "%s/%d.f32", out, key); FILE* f = fopen(p, "wb"); fwrite(buf.data(), 4, buf.size(), f); fclose(f);
        }
    }
}
