// Offline checks of the engine: tuning accuracy, stability, polyphony cost.
#include "../src/engine/Engine.hpp"
#include <cstdio>
#include <vector>
#include <cstring>
#include <thread>
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
    e->setParameter(kParamRoom, kRoomLiteAbbey); e->setParameter(kParamRoomMix, 0.55f); e->setParameter(kParamDistance, 18.f);
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

    // ---- 4. Expression matrix with the three kinds of controller
    auto run = [&](std::unique_ptr<Engine>& en, int samples) { std::vector<float> a(samples), b(samples); for (int i = 0; i < samples; i += 64) en->render(&a[i], &b[i], std::min(64, samples - i)); return a; };
    auto setup = [&](std::unique_ptr<Engine>& en) {
        en->prepare(fs); silenceAllStops(*en); en->setParameter(9, 1.f);   // Principal 8
        en->setParameter(kParamTuning, kTun12); en->setParameter(kParamRoom, kRoomDry); en->setParameter(kParamAgeDrift, 0.f);
        en->setParameter(kParamZoneLoDiv, kGreat); en->setParameter(kParamPedalSplit, 0.f);
    };
    {   // Osmose (MPE): pressure travel then aftertouch on CC74
        auto en = std::make_unique<Engine>(); setup(en);
        uint8_t on[3] = { 0x91, 60, 90 }; en->midi(on, 3);
        run(en, fs / 50);                                   // before any pressure arrives
        const float waiting = en->getParameter(kOutDstPallet);
        for (int v = 0; v <= 127; v += 16) { uint8_t m[2] = { 0xD1, (uint8_t)v }; en->midi(m, 2); run(en, 256); }
        uint8_t full[2] = { 0xD1, 127 }; en->midi(full, 2);
        uint8_t cc0[3] = { 0xB1, 74, 0 }; en->midi(cc0, 3);
        auto a = run(en, fs);
        const float footVoiced = en->getParameter(kOutDstFoot);
        const double hz = measureHz(a, fs / 2, fs, 261.63);
        uint8_t cc1[3] = { 0xB1, 74, 127 }; en->midi(cc1, 3);
        run(en, fs / 2);
        const float footBoost = en->getParameter(kOutDstFoot);
        uint8_t offA[3] = { 0x81, 60, 64 }; en->midi(offA, 3); run(en, fs / 4);
        uint8_t on2[3] = { 0x92, 64, 90 }; en->midi(on2, 3); run(en, 480);
        const float waiting2 = en->getParameter(kOutDstPallet);
        printf("Osmose MPE: second note waits for its pressure: pallet %.0f%%\n", waiting2);
        if (waiting2 > 5.f) ++failures;
        printf("Osmose MPE: pallet %.0f%% before pressure arrives, foot %.0f mm at full key travel (%+.1f c), %.0f mm deep in aftertouch\n",
               waiting, footVoiced, 1200 * std::log2(hz / 261.63), footBoost);
        if (std::fabs(footVoiced - 78.f) > 3.f || footBoost < 100.f || std::fabs(1200 * std::log2(hz / 261.63)) > 8) ++failures;
    }
    {   // keyboard with polyphonic aftertouch
        auto en = std::make_unique<Engine>(); setup(en);
        uint8_t on[3] = { 0x90, 60, 100 }; en->midi(on, 3); run(en, fs / 4);
        const float f0 = en->getParameter(kOutDstFoot);
        uint8_t at[3] = { 0xA0, 60, 64 }; en->midi(at, 3); run(en, fs / 4);
        const float f1 = en->getParameter(kOutDstFoot);
        printf("poly aftertouch keyboard: %.0f mm, with half aftertouch %.0f mm\n", f0, f1);
        if (std::fabs(f0 - 78.f) > 3.f || f1 <= f0) ++failures;
    }
    {   // plain keyboard: no expression data at all
        auto en = std::make_unique<Engine>(); setup(en);
        uint8_t on[3] = { 0x90, 60, 64 }; en->midi(on, 3); run(en, fs / 4);
        printf("plain keyboard: %.0f mm\n", en->getParameter(kOutDstFoot));
        if (std::fabs(en->getParameter(kOutDstFoot) - 78.f) > 3.f) ++failures;
    }

    // ---- 4b. Real-world MPE streams: how the Osmose sends, how a host may flatten it
    auto send = [&](std::unique_ptr<Engine>& en, int a0, int a1, int a2 = -1) { uint8_t m[3] = { (uint8_t)a0, (uint8_t)a1, (uint8_t)(a2 < 0 ? 0 : a2) }; en->midi(m, a2 < 0 ? 2 : 3); };
    {   // Osmose MPE: channels 2-16, pressure starts before the note-on and keeps rising, CC74 for the second stage
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0xB0, 101, 0); send(en, 0xB0, 100, 6); send(en, 0xB0, 6, 15);   // MCM: lower zone, 15 channels
        int ok = 0; float worstBoost = 0.f;
        for (int i = 0; i < 20; ++i) {
            const int ch = 1 + i % 15, key = 48 + (i * 5) % 24;
            for (int v = 0; v <= 127; v += 8) { if (v == 32) send(en, 0x90 | ch, key, 90); send(en, 0xD0 | ch, v); run(en, 96); }
            send(en, 0xD0 | ch, 127); run(en, fs / 10);
            const float foot = en->getParameter(kOutDstFoot);
            worstBoost = std::max(worstBoost, std::fabs(en->getParameter(kOutDstBoost)));
            if (std::fabs(foot - 78.f) < 3.f && en->getParameter(kOutLiveAftertouch) < 0.f) ++ok;
            for (int v = 127; v >= 0; v -= 16) { send(en, 0xD0 | ch, v); run(en, 64); }
            send(en, 0xD0 | ch, 0); send(en, 0x80 | ch, key, 64); run(en, fs / 20);
        }
        printf("Osmose MPE stream: %d of 20 notes on channels 2-16 at voiced wind, no boost without the second stage (worst %.1f mm), channel 16 bend range %.0f\n",
               ok, worstBoost, en->getParameter(kOutBendLo));
        if (ok != 20 || worstBoost > 1.f) ++failures;
        // channel 16 must be a note channel with the members' bend range, not an upper-zone master
        send(en, 0xEF, 0, 127); send(en, 0x9F, 60, 90); send(en, 0xDF, 127); run(en, fs / 10);
        const float bent = en->getParameter(kOutDstPitch);
        printf("  channel 16 full bend: %+.0f cents (members bend +-48, strict limit applies)\n", bent);
        if (bent < 40.f) ++failures;
        send(en, 0x8F, 60, 64); send(en, 0xEF, 0, 64); run(en, fs / 10);
    }
    {   // a host that is not in MPE mode for the plugin: every note on channel 1, key pressure as poly aftertouch
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x90, 60, 80);
        for (int v = 20; v <= 127; v += 8) { send(en, 0xA0, 60, v); run(en, 96); }
        send(en, 0xA0, 60, 127); run(en, fs / 10);
        const float full = en->getParameter(kOutDstPallet), boost = en->getParameter(kOutDstBoost);
        send(en, 0x90, 64, 80);
        for (int v = 20; v <= 40; v += 4) { send(en, 0xA0, 64, v); run(en, 96); }
        run(en, fs / 10);
        const float partial = en->getParameter(kOutDstPallet);
        const int via = (int)en->getParameter(kOutViaLo);
        printf("host-flattened MPE (poly AT from the start of the note): pallet %.0f%% at full travel, %.0f%% at a light touch, boost %+.0f mm, flagged as one channel %s\n",
               full, partial, boost, (via & kViaOneChannel) && (via & kViaTravelPoly) ? "yes" : "no");
        if (full < 95.f || partial > 60.f || std::fabs(boost) > 1.f || !(via & kViaTravelPoly)) ++failures;
    }
    {   // a polyphonic-aftertouch keyboard (or the Osmose's poly aftertouch configuration): aftertouch starts once the key is down
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x90, 60, 100); run(en, fs / 5);
        const float before = en->getParameter(kOutDstFoot);
        for (int v = 0; v <= 100; v += 10) { send(en, 0xA0, 60, v); run(en, 128); }
        run(en, fs / 10);
        const float pal = en->getParameter(kOutDstPallet), boost = en->getParameter(kOutDstBoost);
        printf("poly aftertouch keyboard: %.0f mm when struck, aftertouch adds %+.0f mm, pallet stays %.0f%%\n", before, boost, pal);
        if (std::fabs(before - 78.f) > 3.f || boost < 10.f || pal < 95.f) ++failures;
    }
    {   // classic keyboard: mono aftertouch (channel pressure on channel 1) after the key is down
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x90, 60, 100); run(en, fs / 5);
        for (int v = 0; v <= 100; v += 10) { send(en, 0xD0, v); run(en, 128); }
        run(en, fs / 10);
        const float pal = en->getParameter(kOutDstPallet), boost = en->getParameter(kOutDstBoost);
        printf("classic keyboard: channel aftertouch adds %+.0f mm, pallet stays %.0f%%\n", boost, pal);
        if (boost < 10.f || pal < 95.f) ++failures;
    }
    {   // MPE+ (Haken): CC87 adds 7 bits to the next pressure value
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0xD1, 30); send(en, 0x91, 60, 90); send(en, 0xB1, 87, 64); send(en, 0xD1, 63); run(en, 480);
        const float v = en->getParameter(kOutLivePressure);
        printf("MPE+ 14-bit pressure: %.2f of 127 (expect 63.50)\n", v);
        if (std::fabs(v - 63.5f * 127.f * 128.f / 16256.f) > 0.05f) ++failures;
    }

    // ---- 5. Measured rooms (convolution), if this build has them
    {
        auto en = std::make_unique<Engine>(); setup(en);
        en->setParameter(kParamRoom, kRoomMinster); en->setParameter(kParamRoomMix, 0.5f);
        int waited = 0;
        do { run(en, 480); std::this_thread::sleep_for(std::chrono::milliseconds(10)); ++waited; }
        while (en->getParameter(kOutRoomStatus) == kRoomBuilding && waited < 400);
        const int st = (int)en->getParameter(kOutRoomStatus);
        if (st == kRoomNoData) printf("measured rooms: not in this build (lite fallback)\n");
        else {
            uint8_t on[3] = { 0x90, 60, 100 }, off[3] = { 0x80, 60, 64 };
            en->midi(on, 3); run(en, fs / 2); en->midi(off, 3);
            auto t0 = std::chrono::steady_clock::now();
            auto tail = run(en, fs * 3);
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            double e1 = 0, e2 = 0; for (int i = 0; i < fs / 2; ++i) e1 += tail[i] * tail[i]; for (int i = fs * 2; i < fs * 5 / 2; ++i) e2 += tail[i] * tail[i];
            printf("York Minster: status %d, position %.0f at %.1f m, tail after 2 s %.1f dB below release, cost %.1f%% of one core\n",
                   st, en->getParameter(kOutRoomPos), en->getParameter(kOutRoomPosDist), 10 * std::log10(e2 / (e1 + 1e-20) + 1e-20), 100 * el / 3.0);
            if (st != kRoomReady || !(e2 > 0) || e2 > e1) ++failures;
        }
    }
    // ---- 6. Custom impulse response from a WAV file (stereo, 24-bit), and a bad path
    {
        const char* path = "custom_ir_test.wav";
        {
            const int n = fs * 2; std::vector<uint8_t> d;
            auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back((uint8_t)(v >> (8 * i))); };
            auto u16 = [&](uint16_t v) { d.push_back((uint8_t)v); d.push_back((uint8_t)(v >> 8)); };
            d.insert(d.end(), { 'R', 'I', 'F', 'F' }); u32(36 + n * 6); d.insert(d.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
            u32(16); u16(1); u16(2); u32(44100); u32(44100 * 6); u16(6); u16(24);
            d.insert(d.end(), { 'd', 'a', 't', 'a' }); u32(n * 6);
            uint32_t rng = 1;
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c) {
                    rng = rng * 1664525u + 1013904223u;
                    const float v = i == 0 ? 0.9f : ((int)(rng >> 9) / 4194304.f - 1.f) * 0.3f * std::exp(-3.f * i / 44100.f);
                    const int32_t q = (int32_t)(v * 8388607.f);
                    d.push_back((uint8_t)q); d.push_back((uint8_t)(q >> 8)); d.push_back((uint8_t)(q >> 16));
                }
            FILE* f = std::fopen(path, "wb"); std::fwrite(d.data(), 1, d.size(), f); std::fclose(f);
        }
        auto wait = [&](std::unique_ptr<Engine>& en) {
            int waited = 0;
            do { run(en, 480); std::this_thread::sleep_for(std::chrono::milliseconds(10)); ++waited; }
            while (en->getParameter(kOutRoomStatus) == kRoomBuilding && waited < 400);
            return (int)en->getParameter(kOutRoomStatus);
        };
        auto en = std::make_unique<Engine>(); setup(en);
        en->setCustomIrPath(path); en->setParameter(kParamRoom, kRoomCustom);
        const int stOk = wait(en);
        uint8_t on[3] = { 0x90, 60, 100 }, off[3] = { 0x80, 60, 64 };
        en->midi(on, 3); run(en, fs / 2); en->midi(off, 3);
        auto tail = run(en, fs * 2);
        double e1 = 0, e2 = 0; for (int i = 0; i < fs / 4; ++i) e1 += tail[i] * tail[i]; for (int i = fs; i < fs * 5 / 4; ++i) e2 += tail[i] * tail[i];
        en->setCustomIrPath("/no/such/file.wav");
        const int stBad = wait(en);
        printf("custom IR: status %d, tail after 1 s %.1f dB; missing file -> status %d\n", stOk, 10 * std::log10(e2 / (e1 + 1e-20) + 1e-20), stBad);
        if (stOk != kRoomReady || !(e2 > 0) || stBad != kRoomCustomError) ++failures;
        std::remove(path);
    }

    // ---- 7. Every room at the same loudness, none overloading (limiter off so nothing is hidden)
    {
        auto loud = [&](int room, int mic, float mix, double& peak) {
            auto en = std::make_unique<Engine>();
            en->prepare(fs);
            for (int s = 0; s < kNumStops; ++s) en->setParameter(s, paramInfo(s).def);   // P01 registration
            en->setParameter(kParamTuning, kTun12); en->setParameter(kParamAgeDrift, 0.f);
            en->setParameter(kParamLimiter, 0.f); en->setParameter(kParamMicType, (float)mic);
            en->setParameter(kParamRoomMix, mix); en->setParameter(kParamRoom, (float)room);
            int waited = 0;
            do { run(en, 480); std::this_thread::sleep_for(std::chrono::milliseconds(10)); ++waited; }
            while (en->getParameter(kOutRoomStatus) == kRoomBuilding && waited < 600);
            if (roomIsMeasured(room) && en->getParameter(kOutRoomStatus) == kRoomNoData) return -999.0;
            run(en, fs / 2);
            const int keys[6] = { 36, 48, 55, 60, 64, 67 };
            for (int k : keys) { uint8_t on[3] = { 0x90, (uint8_t)k, 100 }; en->midi(on, 3); }
            std::vector<float> a(fs * 4), b(fs * 4);
            for (int i = 0; i < fs * 4; i += 64) {
                if (i == fs * 3) for (int k : keys) { uint8_t off[3] = { 0x80, (uint8_t)k, 64 }; en->midi(off, 3); }
                en->render(&a[i], &b[i], 64);
            }
            peak = 0; double e = 0;
            for (int i = 0; i < fs * 4; ++i) peak = std::max(peak, (double)std::max(std::fabs(a[i]), std::fabs(b[i])));
            for (int i = fs; i < fs * 3; ++i) e += 0.5 * ((double)a[i] * a[i] + (double)b[i] * b[i]);
            return 10 * std::log10(e / (2 * fs) + 1e-20);
        };
        double pk; const double ref = loud(kRoomDry, 0, 0.55f, pk);
        printf("room loudness, full chord, limiter off (relative to dry %.1f dBFS RMS, peak %.2f):\n", ref, pk);
        for (int room = 0; room < kNumRooms; ++room) {
            if (room == kRoomCustom || room == kRoomDry) continue;
            for (int mic = 0; mic < 3; ++mic) for (float mix : { 0.55f, 1.f }) {
                const double l = loud(room, mic, mix, pk);
                if (l < -900) { if (mic == 0 && mix < 1.f) printf("  %-22s not in this build\n", kRoomLabels[room]); continue; }
                const bool bad = std::fabs(l - ref) > 3.0 || pk >= 1.0;
                printf("  %-22s %-8s mix %3.0f%%: %+5.1f dB, peak %.2f%s\n", kRoomLabels[room], kMicLabels[mic], mix * 100, l - ref, pk, bad ? "   <-- FAIL" : "");
                if (bad) ++failures;
            }
        }
    }

    printf("%s\n", failures ? "FAILURES" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
