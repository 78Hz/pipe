// Reed regime probe: for one Opus 14 reed pipe, sweep the tongue frequency and report what sounds
// (frequency by autocorrelation, level, and whether it is steady).   reed_probe "<stop>" div key [frLo frHi n]
#include "engine/ReedModel.hpp"
#include "engine/FlueModel.hpp"
#include <cstdlib>
using namespace pipe;
static void listen(const ReedPipeSpec& sp, double& f, double& rms, double& drift) {
    ReedPipeRT p; p.init(sp, 48000, 3); p.setChestPa(79 * 9.80665); p.key(true);
    const int N = (int)(48000 * std::max(1.2, 0.4 + 80.0 / sp.target)); std::vector<double> x(N); float o;
    for (int i = 0; i < N; ++i) { p.render(&o, 1); x[i] = o; }
    auto pitchAt = [&](int a, int L) {
        const int lo = 48000 / 2000, hi = std::min(L / 2, 48000 / 15); double best = -1; int bl = 0;
        double e0 = 0; for (int i = 0; i < L; ++i) e0 += x[a + i] * x[a + i];
        std::vector<double> r(hi + 2, 0.0);
        for (int l = lo; l <= hi + 1; ++l) { double s = 0, e1 = 0; for (int i = 0; i + l < L; ++i) { s += x[a + i] * x[a + i + l]; e1 += x[a + i + l] * x[a + i + l]; } r[l] = s / std::sqrt(e0 * e1 + 1e-30); }
        for (int l = lo + 1; l <= hi; ++l) if (r[l] > r[l - 1] && r[l] >= r[l + 1] && r[l] > 0.9 * best + 0.0) { if (r[l] > best + 0.02 || best < 0) { best = r[l]; bl = l; } }
        // first peak within 0.95 of the global max = the period
        double gm = 0; for (int l = lo + 1; l <= hi; ++l) gm = std::max(gm, r[l]);
        for (int l = lo + 1; l <= hi; ++l) if (r[l] > r[l - 1] && r[l] >= r[l + 1] && r[l] > 0.92 * gm) { bl = l; break; }
        if (gm < 0.5) return 0.0;
        const double y0 = r[bl - 1], y1 = r[bl], y2 = r[bl + 1], d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
        return 48000.0 / (bl + d);
    };
    const int L = std::min(N / 4, 9600);
    const double fa = pitchAt(N - 2 * L, L), fb = pitchAt(N - L, L);
    double e = 0; for (int i = N - L; i < N; ++i) e += x[i] * x[i]; rms = std::sqrt(e / L);
    f = fb; drift = (fa > 0 && fb > 0) ? 1200 * std::log2(fb / fa) : 999;
}
int main(int argc, char** argv) {
    const char* stop = argv[1]; const int div = atoi(argv[2]); const int key = atoi(argv[3]);
    const double lo = argc > 4 ? atof(argv[4]) : 0.6, hi = argc > 5 ? atof(argv[5]) : 2.0; const int n = argc > 6 ? atoi(argv[6]) : 15;
    reed::ReedConstants k; k.widthRatio = 0.5655; k.gamma = 0.685; k.Qr = 95.75; k.bootLitres8 = 0.8926; k.closure = 0.5278; k.shallot = 2.0;
    if (getenv("TRUMPET")) { k.widthRatio = 0.6266; k.gamma = 0.529; k.Qr = 61.38; k.bootLitres8 = 0.5789; k.closure = 0.5777; }
    if (getenv("SHALLOT")) k.shallot = atof(getenv("SHALLOT"));
    if (getenv("TUBE")) k.shallotTube = true;
    if (getenv("ZMIN")) k.zetaMin = atof(getenv("ZMIN"));
    if (getenv("SHR")) k.shallotR = atof(getenv("SHR"));
    if (getenv("QR")) k.Qr = atof(getenv("QR"));
    if (getenv("WIDTH")) k.widthRatio = atof(getenv("WIDTH"));
    const double op = getenv("OPEN") ? atof(getenv("OPEN")) : 1.0;
    for (int i = 0; i < opus14::kNumStops; ++i) {
        const auto& s = opus14::kStops[i];
        if (s.div != (opus14::Division)div || std::strcmp(s.name, stop)) continue;
        const double hz = 440.0 * std::pow(2.0, (key - 69) / 12.0) * 8.0 / s.feet;
        ReedPipeSpec sp = reed::design(s, key, hz, k);
        sp.x0 *= op; sp.xs *= op;
        printf("%s key %d target %.2f Hz  resonator length %.3f m  modes:", stop, key, hz, sp.length);
        for (int m = 0; m < std::min(6, sp.nModes); ++m) printf(" %.1f(Q%.0f)", sp.f[m], sp.Q[m]); printf("\n");
        for (int j = 0; j < n; ++j) {
            const double r = lo * std::pow(hi / lo, j / (double)std::max(1, n - 1));
            ReedPipeSpec c = sp; c.fr = (float)(r * hz);
            const double gm = getenv("GAMMA") ? atof(getenv("GAMMA")) : k.gamma; c.x0 = (float)(op * reed::restOpening(c, c.fr, gm)); if (getenv("WIDTH") == nullptr) reed::applyZeta(c, k, gm); c.xs = (float)(k.closure * c.x0);
            double f, rms, dr; listen(c, f, rms, dr);
            printf("  tongue %7.2f Hz (x%.2f): sounds %8.2f Hz (%+7.1f c from target) rms %.4f drift %+.1f c\n", c.fr, r, f, f > 0 ? 1200 * std::log2(f / hz) : 0.0, rms, dr);
        }
    }
}
