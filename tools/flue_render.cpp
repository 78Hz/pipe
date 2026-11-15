// Render one Octave 8' pipe with the matched constants, cut to pitch first (render, measure the
// sounding frequency, recut the body by f/target, repeat) as the builder tunes a pipe.
//   flue_render out.wav y0b lpk gscale kd noise key [hold_s]
// Writes a 16-bit stereo wav (mouth, top), key down at 0.02 s, up at hold_s + 0.02 s, 2.5 s tail.
#include "engine/FlueModel.hpp"
#include <cstdlib>
using namespace pipe; using namespace pipe::flue;
int main(int argc, char** argv) {
    JetParams jp; jp.fitted = true; jp.turbulent = true; jp.ddRule = 1; jp.sign = -1;
    jp.y0b = atof(argv[2]); jp.lpk = atof(argv[3]); const double gs = atof(argv[4]); jp.kd = atof(argv[5]); jp.noise = atof(argv[6]);
    const int key = atoi(argv[7]); const double hold = argc > 8 ? atof(argv[8]) : 3.0;
    for (auto& s : opus14::kStops) if (s.div == opus14::kHW && !strcmp(s.name, "Octave 8'")) {
        Voicing v; v.ising = 2.0; Design d = design(s, key, v);
        auto setup = [&](FlueSim& sim) { sim.init(d, v, 48000); JetParams j = jp; j.gain = gs * 2.0 * (sim.b / d.flue) / 0.10; sim.jp = j; };
        for (int it = 0; it < 6; ++it) {
            FlueSim sim; setup(sim); sim.jp.noise = 0; sim.key(true, kChestPa);
            const size_t N = 3 * 48000; std::vector<double> vv(N);
            for (size_t n = 0; n < N; ++n) { sim.tickKey(); vv[n] = sim.vPrev; }
            const double f = peakFreq(vv, N - 48000, 48000, d.target, 0.06, 48000), c = 1200 * std::log2(f / d.target);
            fprintf(stderr, "key %d cut %d: %.2f Hz (%+.1f c)\n", key, it, f, c);
            if (std::fabs(c) < 0.5) break;
            recut(d, f / d.target);
        }
        FlueSim sim; setup(sim); std::vector<double> L, R; double pk = 0;
        const size_t N = (size_t)((hold + 2.52) * 48000);
        for (size_t n = 0; n < N; ++n) {
            if (n == (size_t)(0.02 * 48000)) sim.key(true, kChestPa);
            if (n == (size_t)((0.02 + hold) * 48000)) sim.key(false, kChestPa);
            sim.tickKey(); L.push_back(sim.mouthOut); R.push_back(sim.topOut); pk = std::max(pk, std::max(std::fabs(sim.mouthOut), std::fabs(sim.topOut)));
        }
        FILE* f = fopen(argv[1], "wb"); unsigned nn = L.size(), bytes = nn * 4;
        auto w32 = [&](unsigned x) { fwrite(&x, 4, 1, f); }; auto w16 = [&](unsigned short x) { fwrite(&x, 2, 1, f); };
        fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(48000); w32(48000 * 4); w16(4); w16(16); fwrite("data", 1, 4, f); w32(bytes);
        for (size_t i = 0; i < nn; ++i) { w16((unsigned short)(short)lround(L[i] / pk * 0.8 * 32767)); w16((unsigned short)(short)lround(R[i] / pk * 0.8 * 32767)); }
        fclose(f);
    }
}
