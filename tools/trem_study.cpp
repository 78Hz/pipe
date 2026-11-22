// Tremulant study: chest pressure under each kind of tremulant against the load (stops drawn).
#include "../src/engine/Tremulant.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace pipe;
int main() {
    const float dt = 32.f / 48000.f;
    const float depth = getenv("DEPTH") ? atof(getenv("DEPTH")) : 0.22f, rate = getenv("RATE") ? atof(getenv("RATE")) : 5.8f;
    for (int k = 0; k < kNumTremKinds; ++k) {
        printf("%s  rate %.1f Hz depth %.2f\n", kTremKindLabels[k], rate, depth);
        for (float d : { 0.0f, 0.3f, 0.6f, 1.0f, 1.2f, 1.5f, 2.0f }) {
            Tremulant t; t.kind = k;
            auto E = [](const char* n, float& v) { if (getenv(n)) v = atof(getenv(n)); };
            E("DA", t.kDouxArea); E("DB", t.kDouxBack); E("VA", t.kVentArea); E("VG", t.kVentGain); E("VS", t.kVentSeat); E("VZ", t.kVentZeta); E("VH", t.kHelmVent); E("TL", t.kTrunkLin); t.rate = rate; t.depth = depth; t.reset();
            for (int i = 0; i < (int)(3 / dt); ++i) t.step(dt, 1.f, d, false);
            const float pOff = t.p;
            std::vector<float> ps;
            for (int i = 0; i < (int)(6 / dt); ++i) { float p = t.step(dt, 1.f, d, true); if (i * dt > 3) ps.push_back(p); }
            float mn = 1e9, mx = -1e9, mean = 0; for (float p : ps) { mn = std::min(mn, p); mx = std::max(mx, p); mean += p; } mean /= ps.size();
            int cross = 0; size_t above = 0; for (size_t i = 1; i < ps.size(); ++i) { if (ps[i - 1] < mean && ps[i] >= mean) ++cross; if (ps[i] > mean) ++above; }
            const float f = cross / (ps.size() * dt);
            printf("  load %.2f: off %.3f  on mean %.3f  peak-peak %5.1f %%  rate %.2f Hz  above mean %.0f %%\n", d, pOff, mean, 100 * (mx - mn) / mean, f, 100.f * above / ps.size());
        }
    }
}
