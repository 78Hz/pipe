// Offline scanner: measures the sounding pitch of the flue/reed cores.
#include "../src/engine/PipeModels.hpp"
#include <cstdio>
#include <vector>
#include <complex>
#include <memory>
using namespace pipe;
static double estimateF0(const std::vector<float>& x, double fs, double guess) {
    // autocorrelation peak near guess, refined parabolically
    int n = (int)x.size();
    int start = n / 2;
    int lag0 = (int)(fs / guess);
    int lo = std::max(2, (int)(lag0 * 0.6)), hi = std::min(n / 4, (int)(lag0 * 1.6) + 2);
    double best = -1e30; int bl = lo; std::vector<double> r(hi + 2, 0.0);
    for (int L = lo - 1; L <= hi + 1; ++L) {
        double acc = 0, e1 = 0, e2 = 0;
        for (int i = start; i + L < n; ++i) { acc += x[i] * x[i + L]; e1 += x[i]*x[i]; e2 += x[i+L]*x[i+L]; }
        r[L] = acc / std::sqrt(e1 * e2 + 1e-30);
    }
    for (int L = lo; L <= hi; ++L) if (r[L] > best) { best = r[L]; bl = L; }
    double a = r[bl - 1], b = r[bl], c = r[bl + 1];
    double p = 0.5 * (a - c) / (a - 2 * b + c);
    return fs / (bl + p);
}
int main(int argc, char** argv) {
    const double fs = 48000.0;
    auto core = std::make_unique<FlueCore>();
    int kind = argc > 1 ? atoi(argv[1]) : 0;
    double boreFc = argc > 2 ? atof(argv[2]) : 6.0;
    double jetFc = argc > 3 ? atof(argv[3]) : 3.0;
    double freqs[] = {32.7, 65.4, 130.8, 261.6, 523.3, 1046.5, 2093, 4186, 7000};
    double ratios[] = {0.30,0.35,0.40,0.45,0.50,0.55,0.60,0.65};
    printf("kind=%d boreFc=%.1f jetFc=%.1f\n", kind, boreFc, jetFc);
    for (double f : freqs) {
        printf("%7.1f:", f);
        for (double jr : ratios) {
            core->reset(); core->noise.seed(7);
            double T = fs / f;
            core->sign = kind == 1 ? -1.f : 1.f;
            core->boreDelay = (float)(kind == 1 ? T / 2 : T);
            core->jetDelay = (float)(jr * T);
            core->aBore = onePoleCoef(boreFc * f, fs);
            core->aJet = onePoleCoef(jetFc * f, fs);
            core->aDc = onePoleCoef(0.25f * f, fs);
            int N = (int)(fs * std::max(0.4, 40.0 / f));
            std::vector<float> x(N);
            int speak = -1; double peak = 0;
            for (int i = 0; i < N; ++i) {
                core->breath = 0.9f * std::min(1.0, i / (0.01 * fs));
                x[i] = core->tick();
            }
            double rms = 0; for (int i = N/2; i < N; ++i) rms += x[i]*x[i]; rms = std::sqrt(rms/(N/2));
            // speech time: first time envelope reaches 70% of final rms
            double env = 0; for (int i = 0; i < N; ++i) { env += (std::fabs(x[i]) - env) * (1.0 / (0.002 * fs)); if (env > 0.7 * rms * 1.2 && speak < 0) speak = i; }
            double fe = rms > 0.25 ? estimateF0(x, fs, f) : 0;
            if (fe > 0) printf(" %+6.1f/%3.0f", 1200 * std::log2(fe / f), speak * 1000.0 / fs);
            else printf("   ----/---");
        }
        printf("\n");
    }
}
