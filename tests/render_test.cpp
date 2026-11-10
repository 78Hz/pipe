// Offline checks of the engine: tuning accuracy, stability, polyphony cost.
#include "../src/engine/Engine.hpp"
#include <cstdio>
#include <vector>
#include <cstring>
using namespace pipe;

static void writeWav(const char* path, const std::vector<float>& L, const std::vector<float>& R, int fs) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    int n = (int)L.size(); int data = n * 4;
    auto w32 = [&](int v) { fwrite(&v, 4, 1, f); }; auto w16 = [&](short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + data); fwrite("WAVEfmt ", 1, 8, f); w32(16); w16(1); w16(2); w32(fs); w32(fs * 4); w16(4); w16(16);
    fwrite("data", 1, 4, f); w32(data);
    for (int i = 0; i < n; ++i) { short a = (short)std::max(-32767.f, std::min(32767.f, L[i] * 32767.f)); short b = (short)std::max(-32767.f, std::min(32767.f, R[i] * 32767.f)); w16(a); w16(b); }
    fclose(f);
}
static double measureHz(const std::vector<float>& x, int start, double fs, double guess, int end = -1) {
    int n = end > 0 ? end : (int)x.size(); double T = fs / guess; int lo = (int)(T * 0.8), hi = (int)(T * 1.25) + 1;
    std::vector<double> r(hi + 2);
    for (int L = lo - 1; L <= hi + 1; ++L) { double a = 0, e1 = 0, e2 = 0; for (int i = start; i + L < n; ++i) { a += x[i] * x[i + L]; e1 += x[i] * x[i]; e2 += x[i + L] * x[i + L]; } r[L] = a / std::sqrt(e1 * e2 + 1e-30); }
    int bl = lo; for (int L = lo; L <= hi; ++L) if (r[L] > r[bl]) bl = L;
    double a = r[bl - 1], b = r[bl], c = r[bl + 1]; double p = 0.5 * (a - c) / (a - 2 * b + c);
    return fs / (bl + p);
}
static void silenceAllStops(Engine& e) { for (int s = 0; s < kNumStops; ++s) e.setParameter(s, 0.f); }

int main(int argc, char** argv) {
    const int fs = 48000;
    auto e = std::make_unique<Engine>();
    e->prepare(fs);
    int failures = 0;
    // ---- 1. tuning accuracy of single pipes, dry, steady wind
    e->setParameter(kParamRoom, kRoomDry); e->setParameter(kParamRoomMix, 0.f); e->setParameter(kParamDistance, 2.f);
    e->setParameter(kParamAgeDrift, 0.f); e->setParameter(kParamTuning, kTun12); e->setParameter(kParamStability, 0.f);
    e->setParameter(kParamBlowerOn, 1.f); e->setParameter(kParamBlowerNoise, -90.f);
    struct Case { int stop; const char* name; };
    Case cases[] = { {9, "GT PRINCIPAL 8"}, {10, "GT ROHRFLOTE 8"}, {19, "SW SALICIONAL 8"}, {16, "GT TROMPETE 8"}, {26, "SW HAUTBOIS 8"}, {1, "PD SUBBASS 16"}, {11, "GT OCTAVE 4"} };
    printf("pitch accuracy (cents vs expected):\n");
    double worst = 0;
    for (auto& c : cases) {
        printf("  %-16s", c.name);
        for (int key : {36, 48, 60, 72, 84, 93}) {
            silenceAllStops(*e); e->setParameter(c.stop, 1.f);
            int div = kStops[c.stop].division;
            e->setParameter(kParamZoneLoDiv, (float)div);
            if (div == kPedal && key > kPedalHigh) { printf("    ---"); continue; }
            e->prepare(fs);
            uint8_t on[3] = { 0x90, (uint8_t)key, 100 }; e->midi(on, 3);
            int N = fs * 1; std::vector<float> L(N), R(N);
            for (int i = 0; i < N; i += 256) e->render(&L[i], &R[i], std::min(256, N - i));
            uint8_t off[3] = { 0x80, (uint8_t)key, 64 }; e->midi(off, 3);
            double ratio = kStops[c.stop].type == kStopRank ? kRanks[kStops[c.stop].rankFirst].ratio : 1.0;
            double expect = 440.0 * std::pow(2.0, (key - 69) / 12.0) * ratio;
            double rms = 0; for (int i = N / 2; i < N; ++i) rms += L[i] * L[i]; rms = std::sqrt(rms / (N / 2));
            if (rms < 1e-4) { printf("  mute "); ++failures; continue; }
            double hz = measureHz(L, N / 2, fs, expect);
            double cents = 1200 * std::log2(hz / expect);
            worst = std::max(worst, std::fabs(cents));
            printf(" %+6.1f", cents);
        }
        printf("\n");
    }
    printf("worst |error| = %.1f cents\n", worst);
    if (worst > 10) ++failures;

    // ---- 2. full organ chord: stability + cost
    e->prepare(fs);
    for (int s = 0; s < kNumStops; ++s) e->setParameter(s, paramInfo(s).def);
    e->setParameter(kParamRoom, kRoomAbbey); e->setParameter(kParamRoomMix, 0.55f); e->setParameter(kParamDistance, 18.f);
    e->setParameter(kParamTuning, kTun31); e->setParameter(kParamStability, 0.7f); e->setParameter(kParamBlowerNoise, -62.f);
    e->setParameter(kParamZoneLoDiv, kGreat); e->setParameter(kParamPedalSplit, 48.f);
    int N = fs * 6; std::vector<float> L(N), R(N);
    int chord[] = { 36, 60, 64, 67, 72, 76 };
    double tSum = 0;
    for (int i = 0; i < N; i += 128) {
        if (i == 0) for (int k : chord) { uint8_t m[3] = { 0x90, (uint8_t)k, 100 }; e->midi(m, 3); }
        if (i == fs * 3) printf("  during chord: %d pipes speaking, reservoir %.1f mm, great chest %.1f mm, demand %.0f%%\n", (int)e->getParameter(kOutPipes), e->getParameter(kOutReservoir), e->getParameter(kOutChestGt), 100 * e->getParameter(kOutDemand));
        if (i == fs * 4) for (int k : chord) { uint8_t m[3] = { 0x80, (uint8_t)k, 64 }; e->midi(m, 3); }
        auto t0 = std::chrono::steady_clock::now();
        e->render(&L[i], &R[i], std::min(128, N - i));
        tSum += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    double peak = 0; bool finite = true;
    for (int i = 0; i < N; ++i) { if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false; peak = std::max(peak, (double)std::fabs(L[i])); }
    printf("plein jeu chord: peak %.3f, finite %d, pipes %d, realtime cost %.1f%% of one core\n",
           peak, (int)finite, (int)e->getParameter(kOutPipes), 100.0 * tSum / 6.0);
    if (!finite || peak > 4 || peak < 0.01) ++failures;
    writeWav(argc > 1 ? argv[1] : "/tmp/pipe_chord.wav", L, R, fs);

    // ---- 3. MPE: pressure and bend on one note (lower zone member channel)
    e->prepare(fs);
    silenceAllStops(*e); e->setParameter(10, 1.f);   // Rohrflote 8 (stopped, bend limit 200c)
    e->setParameter(kParamTuning, kTun12); e->setParameter(kParamRoom, kRoomDry); e->setParameter(kParamAgeDrift, 0.f);
    e->setParameter(kParamZoneLoDiv, kGreat); e->setParameter(kParamPedalSplit, 0.f);
    uint8_t on[3] = { 0x91, 60, 100 }; e->midi(on, 3);
    std::vector<float> L2(fs * 2), R2(fs * 2);
    for (int i = 0; i < fs * 2; i += 64) {
        if (i == fs) { uint8_t pb[3] = { 0xE1, 0, 0x50 }; e->midi(pb, 3); }   // bend up
        e->render(&L2[i], &R2[i], 64);
    }
    double h1 = measureHz(L2, fs / 2, fs, 261.63, fs), h2 = measureHz(L2, fs + fs / 2, fs, 261.63 * 1.1);
    if (std::fabs(1200 * std::log2(h1 / 261.63)) > 5) ++failures;
    printf("MPE bend on stopped flute: %.1f Hz -> %.1f Hz (%+.0f cents, limit 200)\n", h1, h2, 1200 * std::log2(h2 / h1));
    printf("%s\n", failures ? "FAILURES" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
