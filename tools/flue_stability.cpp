// Pitch stability of a flue stop over its whole compass for given jet constants: for every key,
// render 2.5 s, measure the pitch in eight 150 ms frames after 1 s; report the spread (cents) and
// keys that do not hold one pitch (spread > 3 c) or speak off their mode (> 60 c off).
//   flue_stability y0b lpk gscale kd noise [vortex]    env STOP, DIV, LO, HI
#include "engine/FlueModel.hpp"
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
using namespace pipe; using namespace pipe::flue;
int main(int argc, char** argv) {
    JetParams jp = matchedPrincipal();
    jp.y0b = atof(argv[1]); jp.lpk = atof(argv[2]); jp.gain = -atof(argv[3]); jp.kd = atof(argv[4]); jp.noise = atof(argv[5]);
    if (argc > 6) jp.vortex = atof(argv[6]);
    const char* stop = getenv("STOP"); const int div = atoi(getenv("DIV"));
    const int lo = getenv("LO") ? atoi(getenv("LO")) : 36, hi = getenv("HI") ? atoi(getenv("HI")) : 93;
    const opus14::Stop* S = nullptr;
    for (int i = 0; i < opus14::kNumStops; ++i) if (opus14::kStops[i].div == (opus14::Division)div && !std::strcmp(opus14::kStops[i].name, stop)) S = &opus14::kStops[i];
    std::vector<std::string> line(hi + 1); std::vector<int> bad(hi + 1, 0);
    std::atomic<int> next{lo};
    auto work = [&] {
        for (int key; (key = next++) <= hi;) {
            Voicing v; v.ising = familyOf(*S) == kString ? 2.8 : 2.0;
            const double hz = 440.0 * std::pow(2.0, (key - 69) / 12.0) * 8.0 / S->feet;
            Design d = designAt(*S, key, hz, v);
            FluePipeSpec sp = specOf(d, jp); sp.noise = 0;
            FluePipeRT p; p.init(sp, 48000, 1); p.setChestPa(kChestPa);
            if (getenv("PALLET")) p.setPallet(atof(getenv("PALLET")), atof(getenv("PALLET")));
            p.key(true);
            const int N = (int)(2.5 * 48000); std::vector<double> x(N); float m, t;
            for (int i = 0; i < N; ++i) { p.render(&m, &t, 1); x[i] = m + t; }
            double mn = 1e9, mx = -1e9, sum = 0; const int L = (int)(0.15 * 48000);
            for (int k = 0; k < 8; ++k) { const double f = peakFreq(x, 48000 + k * L, L, hz, 0.3, 48000), c = 1200 * std::log2(f / hz); mn = std::min(mn, c); mx = std::max(mx, c); sum += c; }
            const double spread = mx - mn, mean = sum / 8;
            double e = 0; for (int i = N - 24000; i < N; ++i) e += x[i] * x[i];
            const bool silent = e / 24000 < 1e-10;
            char b[96]; snprintf(b, sizeof b, "%d:%s%+.0f/%.0f", key, silent ? "SILENT" : "", mean, spread);
            line[key] = b; bad[key] = silent || spread > 3 || std::fabs(mean) > 60;
        }
    };
    std::vector<std::thread> th; for (int i = 0; i < 2; ++i) th.emplace_back(work); for (auto& t : th) t.join();
    int nb = 0; std::string badKeys;
    for (int k = lo; k <= hi; ++k) if (bad[k]) { ++nb; badKeys += " " + line[k]; }
    printf("%s: %d of %d keys unstable/off/silent:%s\n", stop, nb, hi - lo + 1, badKeys.c_str());
}
