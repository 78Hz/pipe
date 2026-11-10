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
    // ---- 4. MIDI / MPE input, rule by rule (MMA/AMEI RP-053; MIDI 1.0). Each check names its clause.
    auto send = [&](std::unique_ptr<Engine>& en, int a0, int a1, int a2 = -1) { uint8_t m[3] = { (uint8_t)a0, (uint8_t)a1, (uint8_t)(a2 < 0 ? 0 : a2) }; en->midi(m, a2 < 0 ? 2 : 3); };
    auto check = [&](bool ok, const char* rule, const std::string& detail) { printf("  %-4s %s: %s\n", ok ? "ok" : "FAIL", rule, detail.c_str()); if (!ok) ++failures; };
    auto fmt = [](const char* f, double a, double b = 0, double c = 0) { char buf[200]; snprintf(buf, sizeof buf, f, a, b, c); return std::string(buf); };
    auto mcm = [&](std::unique_ptr<Engine>& en, int ch, int n) { send(en, 0xB0 | ch, 101, 0); send(en, 0xB0 | ch, 100, 6); send(en, 0xB0 | ch, 6, n); };
    auto rpnBend = [&](std::unique_ptr<Engine>& en, int ch, int semis) { send(en, 0xB0 | ch, 101, 0); send(en, 0xB0 | ch, 100, 0); send(en, 0xB0 | ch, 6, semis); };
    const int G = kGreat;
    printf("MIDI / MPE input:\n");
    {   // §2.1.3 power-on layout; §3.2 notes on the Master Channel; §2.4 default bend ranges
        auto en = std::make_unique<Engine>(); setup(en);
        int open = 0;
        for (int ch = 0; ch < 16; ++ch) { send(en, 0x90 | ch, 40 + ch, 80); }
        run(en, fs / 10);
        for (int ch = 0; ch < 16; ++ch) open += en->debugAperture(G, 40 + ch) > 0.95f;
        check(open == 16, "§2.1.3 + §3.2", fmt("notes on all 16 channels sound (Master Channel 1 and Members 2-16): %.0f of 16", open));
        send(en, 0xE4, 127, 127); send(en, 0xE0, 127, 127); run(en, 64);
        const double mb = en->debugBendSemis(4, 44), ms = en->debugBendSemis(0, 40);
        check(std::fabs(mb - (48 + 2)) < 0.1 && std::fabs(ms - 2) < 0.1, "§2.4", fmt("full bend: member note %.1f st (48 + master 2), note on master %.1f st", mb, ms));
    }
    {   // §2.4 Pitch Bend Sensitivity on one Member applies to all Members (incl. channel 16); Master separate
        auto en = std::make_unique<Engine>(); setup(en);
        rpnBend(en, 3, 24); rpnBend(en, 0, 12);
        send(en, 0x9F, 60, 80); send(en, 0xEF, 127, 127); run(en, 64);
        check(std::fabs(en->debugBendSemis(15, 60) - 24) < 0.1, "§2.4", fmt("sensitivity 24 sent on ch 4 reaches ch 16: %.1f st", en->debugBendSemis(15, 60)));
        send(en, 0x90, 50, 80); send(en, 0xE0, 127, 127); run(en, 64);
        check(std::fabs(en->debugBendSemis(0, 50) - 12) < 0.1, "§2.4", fmt("Master sensitivity set separately: %.1f st", en->debugBendSemis(0, 50)));
    }
    {   // §2.1 MCM layouts; §2.1.4 notes on changed channels stop; channels in no zone are MIDI 1.0 (bend ±2)
        auto en = std::make_unique<Engine>(); setup(en);
        en->setParameter(kParamZoneHiDiv, kSwell); en->setParameter(18, 1.f);   // a Swell stop for the upper zone
        send(en, 0x9A, 62, 80); run(en, fs / 20);
        mcm(en, 0, 7); run(en, fs / 10);
        check(en->debugHolds(G, 62) == 0, "§2.1.4", "a note held on ch 11 stops when an MCM takes ch 11 out of the zone");
        send(en, 0x9B, 63, 80); send(en, 0xEB, 127, 127); run(en, 64);
        check(std::fabs(en->debugBendSemis(11, 63) - 2) < 0.1, "§2.1 / MIDI 1.0", fmt("ch 12 is in no zone: plain MIDI bend range %.1f st", en->debugBendSemis(11, 63)));
        mcm(en, 15, 3); run(en, 64);
        send(en, 0x9D, 64, 80); run(en, fs / 10);
        check(en->debugHolds(kSwell, 64) == 1, "§2.1", "MCM on ch 16 makes an Upper Zone: a note on member ch 14 plays the upper division (Swell)");
    }
    {   // §3.3 values sent before a Note On set the new note's start; §2.4 Master bend adds
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0xE4, 0, 80); send(en, 0x94, 60, 80); run(en, 64);
        const double pre = en->debugBendSemis(4, 60);
        check(std::fabs(pre - (80 * 128 - 8192) / 8192.0 * 48) < 0.1, "§3.3", fmt("bend sent before the Note On is the note's starting pitch: %+.2f st", pre));
        send(en, 0xE0, 127, 127); run(en, 64);
        check(std::fabs(en->debugBendSemis(4, 60) - pre - 2) < 0.1, "§2.4", "Master Channel bend adds to the member note");
    }
    {   // §2.5 per-note pressure; §3.3.4 pressure to zero before Note Off must not touch other notes
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0xD1, 0); send(en, 0x91, 60, 80); send(en, 0xD1, 127);
        send(en, 0xD2, 0); send(en, 0x92, 64, 80); send(en, 0xD2, 40);
        run(en, fs / 5);
        const float a = en->debugAperture(G, 60), b = en->debugAperture(G, 64);
        check(a > 0.95f && b > 0.3f && b < 0.45f, "§2.5", fmt("each note follows its own channel's pressure: %.0f%% and %.0f%%", a * 100, b * 100));
        send(en, 0xD2, 0); send(en, 0x82, 64, 64); run(en, fs / 5);
        check(en->debugAperture(G, 60) > 0.95f, "§3.3.4", fmt("releasing the other note (pressure to 0, Note Off) leaves this one at %.0f%%", en->debugAperture(G, 60) * 100));
    }
    {   // §2.2.1 Channel messages affect every note on their channel
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x93, 60, 80); send(en, 0x93, 67, 80); send(en, 0xD3, 50); run(en, fs / 5);
        const float a = en->debugAperture(G, 60), b = en->debugAperture(G, 67);
        check(std::fabs(a - b) < 0.02f && a < 0.6f, "§2.2.1", fmt("two notes on one Member Channel both follow its pressure: %.0f%% %.0f%%", a * 100, b * 100));
    }
    {   // pressure exists only once the zone sends it: an MPE controller without pressure plays at voiced wind
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x95, 60, 80); run(en, fs / 5);
        check(std::fabs(en->debugFootMm(G, 60) - 78.f) < 3.f, "MpeInput", fmt("no pressure ever sent: pipe at voiced wind %.0f mm", en->debugFootMm(G, 60)));
    }
    {   // a complete MPE controller stream (Osmose MPE mode): pressure on members, CC74 for the second stage
        auto en = std::make_unique<Engine>(); setup(en);
        mcm(en, 0, 15);
        int voiced = 0; float worstBoost = 0.f;
        for (int i = 0; i < 20; ++i) {
            const int ch = 1 + i % 15, key = 48 + (i * 5) % 24;
            send(en, 0xD0 | ch, 0); send(en, 0xB0 | ch, 74, 0);             // §3.3.4: pressure zero before Note On
            send(en, 0x90 | ch, key, 70);
            for (int v = 8; v <= 127; v += 8) { send(en, 0xD0 | ch, v); run(en, 96); }
            send(en, 0xD0 | ch, 127); run(en, fs / 10);
            voiced += std::fabs(en->debugFootMm(G, key) - 78.f) < 3.f;
            worstBoost = std::max(worstBoost, std::fabs(en->debugBoostMm(G, key)));
            for (int v = 127; v >= 0; v -= 16) { send(en, 0xD0 | ch, v); run(en, 64); }
            send(en, 0xD0 | ch, 0); send(en, 0x80 | ch, key, 64); run(en, fs / 20);
        }
        check(voiced == 20 && worstBoost < 0.5f, "MPE stream", fmt("20 notes on ch 2-16 at voiced wind at full key travel: %.0f of 20, no boost without CC74 (%.1f mm)", voiced, worstBoost));
        send(en, 0xD7, 0); send(en, 0x97, 60, 70); send(en, 0xD7, 127); send(en, 0xB7, 74, 127); run(en, fs / 5);
        check(en->debugBoostMm(G, 60) > 25.f, "MPE stream", fmt("CC74 (second stage) adds wind: %+.0f mm", en->debugBoostMm(G, 60)));
        send(en, 0xD8, 0); send(en, 0x98, 64, 70); send(en, 0xD8, 38); run(en, fs / 5);
        check(en->debugAperture(G, 64) < 0.36f, "strict", fmt("a light press (30%% of the travel) opens the pallet only %.0f%%", en->debugAperture(G, 64) * 100));
    }
    {   // MIDI 1.0 keyboard on channel 1: Channel Pressure there is aftertouch for the whole zone (§3.2)
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x90, 60, 100); send(en, 0x90, 64, 100); run(en, fs / 5);
        send(en, 0xD0, 100); run(en, fs / 5);
        check(en->debugAperture(G, 60) > 0.95f && en->debugBoostMm(G, 60) > 15.f && en->debugBoostMm(G, 64) > 15.f, "§3.2 / MIDI 1.0",
              fmt("channel aftertouch on ch 1 adds wind to both notes (%+.0f, %+.0f mm), pallets stay open", en->debugBoostMm(G, 60), en->debugBoostMm(G, 64)));
    }
    {   // MIDI 1.0 Polyphonic Key Pressure: aftertouch for that key only
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x90, 60, 100); send(en, 0x90, 64, 100); run(en, fs / 5);
        send(en, 0xA0, 60, 100); run(en, fs / 5);
        check(en->debugBoostMm(G, 60) > 15.f && std::fabs(en->debugBoostMm(G, 64)) < 0.5f, "MIDI 1.0 poly pressure",
              fmt("only the pressed key gets wind: %+.0f mm vs %+.0f mm", en->debugBoostMm(G, 60), en->debugBoostMm(G, 64)));
    }
    {   // MPE+: CC87 adds 7 bits to the next pressure value
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x91, 60, 90); send(en, 0xB1, 87, 64); send(en, 0xD1, 63); run(en, 480);
        check(std::fabs(en->getParameter(kOutLivePressure) - 63.5f * 127.f * 128.f / 16256.f) < 0.05f, "MPE+",
              fmt("14-bit pressure %.2f of 127", en->getParameter(kOutLivePressure)));
    }
    {   // host per-note expression (CLAP, via the wrapper's private events): notes sharing a channel stay independent
        auto en = std::make_unique<Engine>(); setup(en);
        auto expr = [&](int type, int ch, int key, double v) {
            const int q = (int)(v * 2097151.0 + 0.5);
            uint8_t m[7] = { kPrivateNoteExpression, (uint8_t)type, (uint8_t)ch, (uint8_t)key, (uint8_t)(q & 127), (uint8_t)((q >> 7) & 127), (uint8_t)(q >> 14) };
            en->midi(m, 7);
        };
        send(en, 0x90, 60, 80); expr(kExprPressure, 0, 60, 1.0);
        send(en, 0x90, 64, 80); expr(kExprPressure, 0, 64, 0.3);
        run(en, fs / 5);
        const float a = en->debugAperture(G, 60), b = en->debugAperture(G, 64);
        expr(kExprPressure, 0, 64, 0.0); send(en, 0x80, 64, 64); run(en, fs / 5);
        check(a > 0.95f && b < 0.4f && en->debugAperture(G, 60) > 0.95f, "CLAP note expression",
              fmt("two notes on one channel: %.0f%% and %.0f%%; releasing one leaves the other at %.0f%%", a * 100, b * 100, en->debugAperture(G, 60) * 100));
        expr(kExprTuning, 0, 60, (1.5 + 120) / 240.0); run(en, 64);
        check(std::fabs(en->debugBendSemis(0, 60) - 1.5) < 0.01, "CLAP note expression", fmt("per-note tuning %+.2f st", en->debugBendSemis(0, 60)));
    }
    {   // MIDI 1.0: a second Note On for a sounding key retriggers it; All Notes Off on a Master Channel ends the zone
        auto en = std::make_unique<Engine>(); setup(en);
        send(en, 0x92, 60, 80); send(en, 0x92, 60, 80); run(en, 64);
        check(en->debugHolds(G, 60) == 1, "MIDI 1.0", "repeated Note On for the same key and channel retriggers, no stuck note");
        send(en, 0x93, 62, 80); send(en, 0xB0, 123, 0); run(en, fs / 10);
        check(en->debugHolds(G, 60) == 0 && en->debugHolds(G, 62) == 0, "MIDI 1.0 / MPE", "All Notes Off on the Master Channel ends every note in the zone");
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
