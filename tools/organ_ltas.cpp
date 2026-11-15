// Long-term spectrum of Opus 14 through the engine (dry), in 1/3-octave bands 31.5 Hz - 8 kHz, for the
// rooms' loudness matching: a principal chorus (I Octave 8' 4' 2', Mixture V; Pedal Subbass 16' Octave 8')
// playing chords across the compass. Prints the band weights (power, normalised to sum 1).
#include "engine/Engine.hpp"
#include <cstdio>
#include <complex>
using namespace pipe;
int main() {
    const int fs = 48000; auto e = std::make_unique<Engine>(); e->prepare(fs);
    for (int i = 0; i < kOutFirst; ++i) e->setParameter(i, paramInfo(i).def);
    for (int s = 0; s < kNumStops; ++s) e->setParameter(s, 0.f);
    for (const char* id : { "i_octave_8", "i_octave_4", "i_superoctave_2", "i_mixture_v_113", "p_subbass_16", "p_octave_8" }) e->setParameter(stopById(id), 1.f);
    e->setParameter(kParamRoom, (float)kRoomDry); e->setParameter(kParamLimiter, 0.f); e->setParameter(kParamPedalSplit, 47.f);
    std::vector<float> L, R;
    auto run = [&](double sec) { for (int i = 0; i < (int)(sec * fs); i += 64) { float l[64], r[64]; e->render(l, r, 64); L.insert(L.end(), l, l + 64); R.insert(R.end(), r, r + 64); } };
    const int chords[6][4] = { { 36, 52, 55, 60 }, { 41, 57, 60, 65 }, { 43, 59, 62, 67 }, { 38, 53, 57, 62 }, { 45, 60, 64, 69 }, { 40, 55, 59, 64 } };
    run(0.3);
    for (auto& c : chords) { for (int k : c) { uint8_t m[3] = { 0x90, (uint8_t)k, 100 }; e->midi(m, 3); } run(2.0); for (int k : c) { uint8_t m[3] = { 0x80, (uint8_t)k, 64 }; e->midi(m, 3); } run(0.2); }
    // Welch spectrum
    const int N = 16384; std::vector<double> pw(N / 2, 0.0); int segs = 0;
    for (size_t a = fs; a + N < L.size(); a += N / 2, ++segs) {
        for (int ch = 0; ch < 2; ++ch) {
            const std::vector<float>& x = ch ? R : L;
            std::vector<std::complex<double>> X(N);
            for (int i = 0; i < N; ++i) X[i] = x[a + i] * (0.5 - 0.5 * std::cos(2 * M_PI * i / N));
            // naive DFT is too slow: use a radix-2 FFT
            for (int i = 1, j = 0; i < N; ++i) { int bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(X[i], X[j]); }
            for (int len = 2; len <= N; len <<= 1) { const double ang = -2 * M_PI / len; const std::complex<double> wl(std::cos(ang), std::sin(ang));
                for (int i = 0; i < N; i += len) { std::complex<double> w(1); for (int j = 0; j < len / 2; ++j) { auto u = X[i + j], v = X[i + j + len / 2] * w; X[i + j] = u + v; X[i + j + len / 2] = u - v; w *= wl; } } }
            for (int k = 1; k < N / 2; ++k) pw[k] += std::norm(X[k]);
        }
    }
    double band[25], tot = 0;
    for (int b = -15; b <= 9; ++b) {
        const double fc = 1000.0 * std::pow(2.0, b / 3.0);
        const int k0 = std::max(1, (int)(fc / std::pow(2.0, 1.0 / 6.0) / fs * N)), k1 = std::min(N / 2 - 1, (int)(fc * std::pow(2.0, 1.0 / 6.0) / fs * N));
        double s = 0; for (int k = k0; k <= k1; ++k) s += pw[k];
        band[b + 15] = s; tot += s;
    }
    printf("// 1/3-octave power of the Opus 14 principal chorus, 31.5 Hz - 8 kHz (tools/organ_ltas.cpp)\nstatic const double kOrganBandWeight[25] = {");
    for (int i = 0; i < 25; ++i) printf("%s%.5g", i ? ", " : " ", band[i] / tot);
    printf(" };\n");
}
