// Pipe - physically modelled pipe organ
// The engine: keys and pallets, MPE / polyphonic aftertouch, couplers, pipe
// voices, wind, swell box and the room. No plugin-framework dependencies, so
// it can be tested offline (tests/render_test.cpp).
#pragma once

#include "Params.hpp"
#include "Tuning.hpp"
#include "Voicing.hpp"
#include "Wind.hpp"
#include "Room.hpp"
#include "CalibrationTables.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

namespace pipe {

struct ScopedFlushDenormals {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    unsigned int old;
    ScopedFlushDenormals() : old(_mm_getcsr()) { _mm_setcsr(old | 0x8040); }
    ~ScopedFlushDenormals() { _mm_setcsr(old); }
#elif defined(__aarch64__)
    uint64_t old;
    ScopedFlushDenormals() { asm volatile("mrs %0, fpcr" : "=r"(old)); uint64_t n = old | (1ull << 24); asm volatile("msr fpcr, %0" : : "r"(n)); }
    ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" : : "r"(old)); }
#endif
};

// Calibration lookup: cents by which a family sounds sharp of its nominal pitch.
inline float calibFlueCents(int fam, float jr, float lf) {
    using namespace calib;
    float x = (lf - kLfMin) / kLfStep, y = (jr - kJrMin) / kJrStep;
    x = std::max(0.f, std::min((float)kLfN - 1.001f, x));
    y = std::max(0.f, std::min((float)kJrN - 1.001f, y));
    int xi = (int)x, yi = (int)y; float fx = x - xi, fy = y - yi;
    const float a = kFlue[fam][yi][xi] * (1 - fx) + kFlue[fam][yi][xi + 1] * fx;
    const float b = kFlue[fam][yi + 1][xi] * (1 - fx) + kFlue[fam][yi + 1][xi + 1] * fx;
    return a * (1 - fy) + b * fy;
}
inline float calibReedCents(int fam, float lf) {
    using namespace calib;
    float x = (lf - kLfMin) / kLfStep;
    x = std::max(0.f, std::min((float)kLfN - 1.001f, x));
    int xi = (int)x; float fx = x - xi;
    return kReed[fam][xi] * (1 - fx) + kReed[fam][xi + 1] * fx;
}

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}

constexpr int kControlBlock = 32;
constexpr int kMaxFlueVoices = 448;
constexpr int kMaxReedVoices = 96;

struct ChannelState {
    float bend = 0.f;          // -1..1
    float pressure = 0.f;      // 0..1
    float slide = 0.5f;        // 0..1 (CC74, MPE default 64)
    bool pressureSeen = false, slideSeen = false;
    float bendRange = 48.f;    // semitones
    int rpnMsb = 127, rpnLsb = 127;
};

struct Pallet {
    int holds = 0;
    float aperture = 0.f;
    float target = 0.f;
    float openTime = 0.02f, closeTime = 0.02f;   // seconds
    int srcChannel = 0, srcZone = 0;
    float polyPressure = -1.f;                   // <0: none
    float chiff = 0.f;                           // attack transient envelope
    KeyPitch pitch;
    uint32_t order = 0;
    bool coupled = false;
    // live values for display
    float footRatio = 1.f, bendCents = 0.f;
};

struct Voice {
    bool active = false;
    bool reed = false;
    int slot = -1;              // index into the flue or reed pool
    int rank = 0, division = 0, key = 0;
    int family = 0;
    bool stopClosed = false;
    float freq = 0.f;
    float baseBore = 0.f, baseJet = 0.f, jr = 0.4f;     // flue
    float baseA = 0.f, baseB = 0.f;                     // reed
    float voicedRatio = 1.f;    // toe pressure / division nominal
    float gain = 0.f, gL = 0.f, gR = 0.f;
    float lpCoef = 1.f, lp = 0.f;
    float emph = 0.f, emphCoef = 0.f, emphLp = 0.f;   // radiation shelf: narrow mouths and reed bells radiate highs
    float baseNoise = 0.01f, chiffAmt = 0.f, languid = 0.f;
    float bendLimit = 50.f;
    float flowWeight = 1.f;
    float curBreath = 0.f, curBore = 0.f, curJet = 0.f, curA = 0.f, curB = 0.f, curNoise = 0.f, curBias = 0.f;
    float tBreath = 0.f, tBore = 0.f, tJet = 0.f, tA = 0.f, tB = 0.f, tNoise = 0.f;   // control-block targets
    float env = 0.f;
    int quiet = 0;
    uint32_t age = 0;
};

class Engine {
public:
    Engine() {
        flues.reserve(kMaxFlueVoices);
        for (int i = 0; i < kMaxFlueVoices; ++i) flues.emplace_back(new FlueCore());
        reeds.reserve(kMaxReedVoices);
        for (int i = 0; i < kMaxReedVoices; ++i) reeds.emplace_back(new ReedCore());
        voices.resize(kMaxFlueVoices + kMaxReedVoices);
        voicing = std::make_unique<Voicing>();
        pendingVoicing = std::make_unique<Voicing>();
        voicing->setDefaults();
        pendingVoicing->setDefaults();
        for (int i = 0; i < kNumParams; ++i) params[i] = paramInfo(i).def;
        for (int c = 0; c < 16; ++c) channels[c] = ChannelState();
        channels[0].bendRange = 2.f; channels[15].bendRange = 2.f;
        std::memset(outputs, 0, sizeof(outputs));
        for (int i = 0; i < kNumShownNotes; ++i) outputs[kOutNote0 - kOutFirst + i * 4] = -1.f;
        outputs[kOutMembersLo - kOutFirst] = 15.f;
        outputs[kOutBendLo - kOutFirst] = 48.f;
        outputs[kOutBendHi - kOutFirst] = 48.f;
        for (int d = 0; d < kNumDivisions; ++d) outputs[kOutChestPd - kOutFirst + d] = kChestNominal[d];
        outputs[kOutReservoir - kOutFirst] = 96.f;
    }

    void prepare(double sampleRate) {
        fs = (float)sampleRate;
        room.prepare(sampleRate);
        applyAllParams();
        wind.reset();
        for (auto& v : voices) v.active = false;
        for (int d = 0; d < kNumDivisions; ++d) for (int k = 0; k < kNumKeys; ++k) pallets[d][k] = Pallet();
        room.clear();
        for (int d = 0; d < kNumDivisions; ++d) { swellLpL = swellLpR = 0.f; }
    }

    // ---------------------------------------------------------------- params
    void setParameter(int id, float v) {
        if (id < 0 || id >= kOutFirst) return;
        const float old = params[id];
        params[id] = v;
        if (id < kNumStops) {
            if ((old >= 0.5f) != (v >= 0.5f)) stopChanged(id, v >= 0.5f);
            if (kStops[id].type == kStopTremulant) wind.tremOn = v >= 0.5f;
            return;
        }
        applyParam(id);
    }
    float getParameter(int id) const {
        if (id >= kOutFirst && id < kNumParams) return outputs[id - kOutFirst];
        return (id >= 0 && id < kNumParams) ? params[id] : 0.f;
    }

    // ------------------------------------------------------- non-realtime state
    // Called from the host/UI thread. The audio thread picks the data up at the
    // start of the next control block.
    void setVoicingText(const std::string& text) {
        std::lock_guard<std::mutex> lk(stateMutex);
        pendingVoicing->parseVoicing(text);
        voicingDirty = true;
    }
    void setPipeTuningText(const std::string& text) {
        std::lock_guard<std::mutex> lk(stateMutex);
        pendingVoicing->parseTuning(text);
        tuningDirty = true;
    }
    void setScala(const std::string& scl, const std::string& kbm) {
        ScalaScale s; ScalaKbm k;
        if (!scl.empty()) parseScl(scl, s);
        if (!kbm.empty()) parseKbm(kbm, k);
        std::lock_guard<std::mutex> lk(stateMutex);
        pendingScl = std::move(s);
        pendingKbm = std::move(k);
        scalaDirty = true;
    }

    // ------------------------------------------------------------------- MIDI
    void midi(const uint8_t* d, int size) {
        if (size < 1) return;
        const int st = d[0] & 0xF0, ch = d[0] & 0x0F;
        const int zone = zoneOf(ch);
        switch (st) {
        case 0x90:
            if (size >= 3 && d[2] > 0) { noteOn(ch, d[1], d[2]); break; }
            if (size >= 3) noteOff(ch, d[1], 64);
            break;
        case 0x80: if (size >= 3) noteOff(ch, d[1], d[2]); break;
        case 0xA0:
            if (size >= 3) {
                seen[zone] |= kSeenPolyAT;
                for (int dv = 0; dv < kNumDivisions; ++dv) {
                    Pallet& p = pallets[dv][d[1] & 127];
                    if (p.holds > 0 && p.srcChannel == ch) p.polyPressure = d[2] / 127.f;
                }
            }
            break;
        case 0xD0:
            if (size >= 2) { channels[ch].pressure = d[1] / 127.f; channels[ch].pressureSeen = true; seen[zone] |= kSeenPressure; }
            break;
        case 0xE0:
            if (size >= 3) {
                const int v = (d[1] | (d[2] << 7)) - 8192;
                channels[ch].bend = std::max(-1.f, v / 8192.f);
                if (v != 0) seen[zone] |= kSeenBend;
            }
            break;
        case 0xB0: if (size >= 3) controller(ch, d[1], d[2]); break;
        default: break;
        }
    }

    void allNotesOff() {
        for (int dv = 0; dv < kNumDivisions; ++dv)
            for (int k = 0; k < kNumKeys; ++k) { pallets[dv][k].holds = 0; pallets[dv][k].target = 0.f; }
        notes.clear();
    }

    // ----------------------------------------------------------------- render
    void render(float* outL, float* outR, int n) {
        ScopedFlushDenormals ftz;
        const auto t0 = std::chrono::steady_clock::now();
        int done = 0;
        while (done < n) {
            if (blockPos == 0) controlUpdate();
            const int len = std::min(n - done, kControlBlock - blockPos);
            renderVoices(outL + done, outR + done, len);
            blockPos = (blockPos + len) % kControlBlock;
            done += len;
        }
        // Swell box on its own bus was mixed in renderVoices; now the building.
        room.process(outL, outR, n);
        float peakL = 0.f, peakR = 0.f;
        const float og = outGain;
        for (int i = 0; i < n; ++i) {
            float l = outL[i] * og, r = outR[i] * og;
            if (limiter) { l = softClip(l); r = softClip(r); }
            outL[i] = l; outR[i] = r;
            peakL = std::max(peakL, std::fabs(l)); peakR = std::max(peakR, std::fabs(r));
        }
        outputs[kOutPeakL - kOutFirst] = std::max(peakL, outputs[kOutPeakL - kOutFirst] * 0.9f);
        outputs[kOutPeakR - kOutFirst] = std::max(peakR, outputs[kOutPeakR - kOutFirst] * 0.9f);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double budget = n / (double)fs;
        cpuAvg += 0.05 * (100.0 * el / budget - cpuAvg);
        outputs[kOutCpu - kOutFirst] = (float)cpuAvg;
    }

    // -------------------------------------------------------- info for the UI
    const Tuning& getTuning() const { return tuning; }
    Tuning& tuningMut() { return tuning; }
    float sampleRate() const { return fs; }

private:
    // ------------------------------------------------------------ zones / MPE
    int lowerMembers = 15, upperMembers = 0;
    int zoneOf(int ch) const {
        if (upperMembers > 0 && (ch == 15 || ch >= 15 - upperMembers)) return 1;
        return 0;
    }

    void controller(int ch, int cc, int val) {
        ChannelState& c = channels[ch];
        const int zone = zoneOf(ch);
        switch (cc) {
        case 74: c.slide = val / 127.f; c.slideSeen = true; seen[zone] |= kSeenSlide; break;
        case 11: expressionCC = val / 127.f; break;
        case 66: altSpellingCC = val >= 64; break;
        case 101: c.rpnMsb = val; break;
        case 100: c.rpnLsb = val; break;
        case 6:
            if (c.rpnMsb == 0 && c.rpnLsb == 0) {           // pitch bend sensitivity
                c.bendRange = (float)val;
                // MPE: setting it on a member channel applies to all members of the zone
                const int z = zoneOf(ch);
                if (!(ch == 0 || ch == 15))
                    for (int k = 0; k < 16; ++k) if (k != 0 && k != 15 && zoneOf(k) == z) channels[k].bendRange = (float)val;
            } else if (c.rpnMsb == 0 && c.rpnLsb == 6) {     // MPE configuration message
                if (ch == 0) { lowerMembers = std::min(15, val); if (lowerMembers + upperMembers > 14) upperMembers = std::max(0, 14 - lowerMembers); }
                if (ch == 15) { upperMembers = std::min(15, val); if (lowerMembers + upperMembers > 14) lowerMembers = std::max(0, 14 - upperMembers); }
                // MPE defaults after MCM: members bend 48, master 2
                for (int k = 0; k < 16; ++k) channels[k].bendRange = (k == 0 || k == 15) ? 2.f : 48.f;
                seen[ch == 15 ? 1 : 0] |= kSeenMcm;
            }
            break;
        case 120: case 123: allNotesOff(); break;
        default: break;
        }
    }

    struct ActiveNote { int ch, key; int div[3]; int nDiv; };
    std::vector<ActiveNote> notes;

    void noteOn(int ch, int key, int vel) {
        const int zone = zoneOf(ch);
        seen[zone] |= (vel != 100 && vel != 127 && vel != 64) ? kSeenVelocity : 0;
        int div = (int)(zone == 0 ? params[kParamZoneLoDiv] : params[kParamZoneHiDiv]);
        int mapMode = (int)(zone == 0 ? params[kParamMapLo] : params[kParamMapHi]);
        const int split = (int)params[kParamPedalSplit];
        if (zone == 0 && split > 0 && key < split) { div = kPedal; mapMode = (int)params[kParamMapPed]; }
        if (div < 0 || div >= kNumDivisions) return;
        const bool alt = (params[kParamEnharmonic] >= 0.5f) != altSpellingCC;
        const KeyPitch kp = tuning.keyPitch(key, mapMode, alt);
        if (!kp.valid) return;
        ActiveNote an{ ch, key, { -1, -1, -1 }, 0 };
        const float v = vel / 127.f;
        auto press = [&](int d, bool coupled) {
            if (pressPallet(d, key, kp, ch, zone, v, coupled)) an.div[an.nDiv++] = d;
        };
        press(div, false);
        if (div == kGreat && stopOn(17)) press(kSwell, true);           // SW > GT
        if (div == kPedal && stopOn(6)) press(kGreat, true);            // GT > PED
        if (div == kPedal && stopOn(7)) press(kSwell, true);            // SW > PED
        if (an.nDiv > 0) notes.push_back(an);
    }

    void noteOff(int ch, int key, int relVel) {
        const int zone = zoneOf(ch);
        if (relVel != 64 && relVel != 0) seen[zone] |= kSeenRelease;
        for (size_t i = 0; i < notes.size(); ++i) {
            if (notes[i].ch == ch && notes[i].key == key) {
                for (int j = 0; j < notes[i].nDiv; ++j) {
                    Pallet& p = pallets[notes[i].div[j]][key];
                    if (p.holds > 0 && --p.holds == 0) {
                        p.target = 0.f;
                        p.closeTime = (relVel != 64 && relVel > 0)
                            ? (0.080f - 0.075f * (relVel / 127.f))
                            : params[kParamReleaseMs] * 0.001f;
                    }
                }
                notes.erase(notes.begin() + (long)i);
                return;
            }
        }
    }

    bool stopOn(int s) const { return params[s] >= 0.5f; }

    bool pressPallet(int d, int key, const KeyPitch& kp, int ch, int zone, float vel, bool coupled) {
        if (!keyInCompass(d, kp.keyFloat)) return false;
        Pallet& p = pallets[d][key];
        const bool wasClosed = p.holds == 0;
        p.holds++;
        p.target = 1.f;
        p.srcChannel = ch; p.srcZone = zone; p.coupled = coupled;
        const float fast = params[kParamPalletFast] * 0.001f, slow = params[kParamPalletSlow] * 0.001f;
        p.openTime = slow + (fast - slow) * vel;
        p.pitch = kp;
        p.order = ++orderCounter;
        if (wasClosed) {
            p.polyPressure = -1.f;
            p.chiff = 1.f;
            for (int r = 0; r < kNumRanks; ++r) {
                if (kRanks[r].division != d) continue;
                const int s = stopForRank(r);
                if (s >= 0 && stopOn(s)) startVoice(r, d, key);
            }
        }
        return true;
    }

    void stopChanged(int s, bool on) {
        const StopDef& sd = kStops[s];
        if (sd.type != kStopRank && sd.type != kStopMixture) return;
        for (int r = sd.rankFirst; r < sd.rankFirst + sd.rankCount; ++r) {
            if (on) {
                // slider opens under keys already held: those pipes start speaking
                for (int k = 0; k < kNumKeys; ++k)
                    if (pallets[sd.division][k].holds > 0) startVoice(r, sd.division, k);
            } else {
                for (auto& v : voices) if (v.active && v.rank == r) v.stopClosed = true;
            }
        }
    }

    // ----------------------------------------------------------------- voices
    void startVoice(int rank, int d, int key) {
        const RankDef& rd = kRanks[rank];
        const Pallet& p = pallets[d][key];
        if (rd.lowestKey > 0 && p.pitch.keyFloat < rd.lowestKey - 0.5f) return;
        // already speaking (re-struck or stop redrawn)?
        for (auto& v : voices) if (v.active && v.rank == rank && v.key == key && v.division == d) { v.stopClosed = false; return; }

        const bool isReed = rd.kind == kKindReed;
        double f = p.pitch.hz * rankRatio(rank, key);
        // tuning offsets: rank, pipe, celeste, age, temperature
        const RankVoicing& rv = voicing->rank[rank];
        (void)rv;
        double cents = voicing->pipeTune[rank][p.pitch.pipeIndex] + rd.detuneCents;
        const float drift = params[kParamAgeDrift];
        if (drift > 0) cents += drift * ((hash32((uint32_t)(rank * 4096 + p.pitch.pipeIndex)) & 0xFFFF) / 32767.5f - 1.f);
        const double tC = params[kParamPipeTemp];
        const double tempCents = 1200.0 * std::log2(std::sqrt((273.15 + tC) / (273.15 + 18.0)));
        cents += isReed ? 0.15 * tempCents : tempCents;
        f *= std::pow(2.0, cents / 1200.0);
        if (f > 0.42 * fs || f < 15.0) return;

        // pick a free voice slot of the right kind, or steal
        const int limit = qualityLimit(isReed);
        int poolUsed = 0, freeIdx = -1;
        for (int i = 0; i < (int)voices.size(); ++i) {
            if (voices[i].active) { if (voices[i].reed == isReed) ++poolUsed; }
            else if (freeIdx < 0 && slotKind(i) == isReed) freeIdx = i;
        }
        if (poolUsed >= limit || freeIdx < 0) {
            freeIdx = stealVoice(isReed);
            if (freeIdx < 0) return;
        }
        Voice& v = voices[freeIdx];
        v = Voice();
        v.active = true; v.reed = isReed; v.slot = slotIndex(freeIdx);
        v.rank = rank; v.division = d; v.key = key; v.freq = (float)f;
        v.age = ++ageCounter;

        const int k = std::max(0, std::min(kNumKeys - 1, key));
        const PipeGeometry g = pipeGeometry(*voicing, rank, k, f);
        v.voicedRatio = (float)(g.toeMM / kChestNominal[d]);
        v.flowWeight = (float)g.flowWeight;
        const float reg = std::pow(10.f, voicing->rank[rank].key[kVRegulation][k] / 20.f);
        const uint32_t seed = hash32((uint32_t)(rank * 131071 + key * 31 + d));

        if (!isReed) {
            v.family = rd.family;
            v.jr = (float)std::max(kJetModelMin, std::min(kJetModelMax, modelJetRatio(g.jetRatio)));
            // The jet pulls the pipe off its resonator pitch; build the model at
            // the frequency that makes it sound exactly f at the voiced point.
            double fe = f;
            for (int it = 0; it < 2; ++it) fe = f * std::pow(2.0, -calibFlueCents(v.family, v.jr, (float)std::log2(fe / fs)) / 1200.0);
            const double Te = fs / fe;
            v.baseBore = (float)((rd.kind == kKindStopped) ? Te * 0.5 : Te);
            v.baseJet = (float)(v.jr * Te);
            FlueCore& core = *flues[(size_t)v.slot];
            core.reset();
            core.noise.seed(seed);
            const auto& fc = kFlueFamily[v.family];
            core.sign = rd.kind == kKindStopped ? -1.f : 1.f;
            core.loopGain = fc.loopGain;
            core.aBore = onePoleCoef(fc.boreFc * (float)fe, fs);
            core.aJet = onePoleCoef(fc.jetFc * (float)fe, fs);
            core.aDc = onePoleCoef(kDcFc * (float)fe, fs);
            core.boreDelay = v.baseBore; core.jetDelay = v.baseJet;
            const float nick = voicing->rank[rank].key[kVNicking][k];
            v.baseNoise = 0.02f * (1.f - 0.3f * nick);
            v.chiffAmt = voicing->rank[rank].key[kVChiff][k] * 0.35f * (1.f - 0.25f * nick);
            v.languid = voicing->rank[rank].key[kVLanguid][k];
            // radiation / scale: wide pipes are mellow, narrow pipes bright
            float fcOut = (float)(3.0 * kSpeedOfSound / (M_PI * g.diameterMM * 0.001));
            if (rd.kind == kKindStopped) fcOut *= 0.7f;
            v.lpCoef = onePoleCoef(std::max(1200.f, fcOut), fs);
            v.emph = rd.kind == kKindString ? 3.0f : rd.kind == kKindStopped ? 0.0f : 1.0f;
            v.emphCoef = onePoleCoef((rd.kind == kKindString ? 2.5f : 4.0f) * (float)f, fs);
            v.gain = 0.12f * reg * std::pow((float)g.flowWeight, 0.25f);
            if (rd.kind == kKindString) v.gain *= 1.6f;
            v.bendLimit = rd.kind == kKindStopped ? params[kParamBendStopped]
                        : rd.kind == kKindString ? params[kParamBendString] : params[kParamBendOpen];
            v.curBore = v.baseBore; v.curJet = v.baseJet;
        } else {
            v.family = rd.family;
            const auto& rc = kReedFamily[v.family];
            double fe = f;
            for (int it = 0; it < 2; ++it) fe = f * std::pow(2.0, -calibReedCents(v.family, (float)std::log2(fe / fs)) / 1200.0);
            const double D = fs / fe;
            v.baseA = (float)((1.0 - rc.position) * D);
            v.baseB = (float)(rc.position * D);
            ReedCore& core = *reeds[(size_t)v.slot];
            core.reset();
            core.noise.seed(seed);
            core.reedSlope = rc.slope; core.reedOffset = 0.7f;
            core.aBore = onePoleCoef(rc.boreFc * (float)fe, fs);
            core.aDc = onePoleCoef(kDcFc * (float)fe, fs);
            core.delayA = v.baseA; core.delayB = v.baseB;
            v.baseNoise = 0.005f;
            v.lpCoef = onePoleCoef(std::min(0.45f * fs, (float)f * 14.f + 2500.f), fs);
            v.emph = v.family == kReedOboe ? 2.5f : 2.0f;
            v.emphCoef = onePoleCoef(3.0f * (float)f, fs);
            v.gain = 0.16f * reg;
            v.bendLimit = params[kParamBendReed];
            v.curA = v.baseA; v.curB = v.baseB;
        }
        // where the pipe stands in the case: C side left, C# side right;
        // big pipes outside, pedal towers at the flanks, swell box in the middle
        const float side = (key % 2 == 0) ? -1.f : 1.f;
        const float pos01 = std::max(0.f, std::min(1.f, (p.pitch.keyFloat - 36.f) / 57.f));
        float spread = d == kPedal ? 0.85f : d == kSwell ? 0.18f + 0.25f * (1.f - pos01) : 0.25f + 0.55f * (1.f - pos01);
        const float pan = side * spread;
        v.gL = std::cos((pan + 1.f) * (float)M_PI * 0.25f);
        v.gR = std::sin((pan + 1.f) * (float)M_PI * 0.25f);
    }

    bool slotKind(int i) const { return i >= kMaxFlueVoices; }
    int slotIndex(int i) const { return i >= kMaxFlueVoices ? i - kMaxFlueVoices : i; }
    int qualityLimit(bool reed) const {
        const int q = (int)params[kParamQuality];
        if (reed) return q == 0 ? 48 : q == 1 ? 72 : kMaxReedVoices;
        return q == 0 ? 192 : q == 1 ? 320 : kMaxFlueVoices;
    }
    int stealVoice(bool reed) {
        int best = -1; float bestScore = 1e30f;
        for (int i = 0; i < (int)voices.size(); ++i) {
            const Voice& v = voices[i];
            if (!v.active || v.reed != reed) continue;
            const Pallet& p = pallets[v.division][v.key];
            // prefer released pipes, then the quietest, then the oldest
            float score = (p.holds > 0 && !v.stopClosed ? 1e6f : 0.f) + v.env * 1e3f + (float)v.age * 1e-6f;
            if (score < bestScore) { bestScore = score; best = i; }
        }
        if (best >= 0) {
            Voice& v = voices[best];
            v.active = false;
            // return the index of a slot of the right kind (same slot)
        }
        return best;
    }

    // --------------------------------------------------------- control block
    void controlUpdate() {
        // pick up state from the host/UI thread without blocking
        if ((voicingDirty || tuningDirty || scalaDirty) && stateMutex.try_lock()) {
            if (voicingDirty) { std::memcpy(voicing->rank, pendingVoicing->rank, sizeof(voicing->rank)); voicingDirty = false; }
            if (tuningDirty) { std::memcpy(voicing->pipeTune, pendingVoicing->pipeTune, sizeof(voicing->pipeTune)); tuningDirty = false; }
            if (scalaDirty) { std::swap(tuning.scl, pendingScl); std::swap(tuning.kbm, pendingKbm); scalaDirty = false; }
            stateMutex.unlock();
        }
        const float dt = kControlBlock / fs;

        // wind
        float flow = 0.f; int speaking = 0;
        for (const auto& v : voices) {
            if (!v.active) continue;
            ++speaking;
            const Pallet& p = pallets[v.division][v.key];
            if (!v.stopClosed) flow += v.flowWeight * p.aperture * std::sqrt(std::max(0.f, p.footRatio));
        }
        wind.update(dt, flow);

        // pallets
        for (int d = 0; d < kNumDivisions; ++d) {
            const float chestRatio = wind.chest[d] / kChestNominal[d];
            for (int k = 0; k < kNumKeys; ++k) {
                Pallet& p = pallets[d][k];
                if (p.holds == 0 && p.aperture <= 0.f) continue;
                const float tc = p.target > p.aperture ? p.openTime : p.closeTime;
                const float step = dt / std::max(0.001f, tc);
                if (p.target > p.aperture) p.aperture = std::min(p.target, p.aperture + step);
                else p.aperture = std::max(p.target, p.aperture - step);
                p.chiff *= std::exp(-dt / 0.035f);
                const float a = p.aperture;
                const float open = a * a / (a * a + 0.0144f) * 1.0144f;
                // key pressure (MPE / polyphonic aftertouch) throttles the pallet
                const ChannelState& c = channels[p.srcChannel];
                float press = -1.f;
                if (p.polyPressure >= 0.f) press = p.polyPressure;
                else if (c.pressureSeen) press = c.pressure;
                float mpe = 1.f;
                if (press >= 0.f) {
                    const float mm = params[kParamPressMin] + (params[kParamPressMax] - params[kParamPressMin]) * press;
                    mpe = mm / 78.f;
                }
                p.footRatio = chestRatio * open * mpe;
                // bend: member channel + zone master channel
                const int master = p.srcZone == 0 ? 0 : 15;
                float bend = c.bend * c.bendRange * 100.f;
                if (p.srcChannel != master) bend += channels[master].bend * channels[master].bendRange * 100.f;
                p.bendCents = bend;
            }
        }

        // voices: targets for this block
        const bool strict = params[kParamStrict] >= 0.5f;
        const float slideDepth = params[kParamSlideDepth];
        for (auto& v : voices) {
            if (!v.active) continue;
            const Pallet& p = pallets[v.division][v.key];
            const ChannelState& c = channels[p.srcChannel];
            // foot pressure relative to the pressure the pipe was voiced on
            const float ratio = v.stopClosed ? 0.f : std::max(0.f, p.footRatio);
            float bend = p.bendCents;
            if (strict) bend = std::max(-v.bendLimit, std::min(v.bendLimit, bend));
            const float pitchMul = std::pow(2.f, -bend / 1200.f);
            float targetBreath, tBore = 0, tJet = 0, tA = 0, tB = 0;
            const float slide = c.slideSeen ? (c.slide - 0.5f) * 2.f * slideDepth : 0.f;
            if (!v.reed) {
                const float jetVel = std::sqrt(ratio);                  // jet speed ~ sqrt(pressure)
                targetBreath = kFlueVoicedBreath * jetVel;
                tBore = v.baseBore * pitchMul;
                const float jet = v.baseJet * pitchMul / std::max(0.35f, jetVel);
                tJet = std::min((float)(kJetSize - 8), jet);
                v.curBias = v.languid * 0.04f + slide * 0.07f;
            } else {
                targetBreath = kReedVoicedBreath * std::sqrt(ratio);
                tA = v.baseA * pitchMul; tB = v.baseB * pitchMul;
            }
            // targets stored for per-sample ramps
            v.tBreath = targetBreath;
            v.tBore = tBore; v.tJet = tJet; v.tA = tA; v.tB = tB;
            v.tNoise = v.baseNoise + v.chiffAmt * p.chiff;

            // end of a pipe's life: pallet shut (or stop in) and sound has died away
            if ((p.aperture <= 0.f || v.stopClosed) && v.env < 2e-5f) { if (++v.quiet > 8) v.active = false; }
            else v.quiet = 0;
        }

        // shown notes: the most recent held pallets
        int shown = 0;
        Pallet* order[kNumShownNotes] = {};
        for (int d = 0; d < kNumDivisions; ++d) for (int k = 0; k < kNumKeys; ++k) {
            Pallet& p = pallets[d][k];
            if (p.holds <= 0) continue;
            // insertion by order (newest first)
            int pos = shown < kNumShownNotes ? shown : kNumShownNotes;
            for (int i = 0; i < std::min(shown, kNumShownNotes); ++i) if (!order[i] || p.order > order[i]->order) { pos = i; break; }
            if (pos < kNumShownNotes) {
                for (int i = kNumShownNotes - 1; i > pos; --i) order[i] = order[i - 1];
                order[pos] = &p;
                if (shown < kNumShownNotes) ++shown;
            }
        }
        for (int i = 0; i < kNumShownNotes; ++i) {
            float* o = &outputs[kOutNote0 - kOutFirst + i * 4];
            if (i < shown && order[i]) {
                const Pallet& p = *order[i];
                const int d = (int)(&p - &pallets[0][0]) / kNumKeys;
                o[0] = (float)((&p - &pallets[0][0]) % kNumKeys);
                o[1] = (float)((p.coupled ? 1000 : 0) + d * 100 + p.srcZone * 16 + p.srcChannel + 1);
                o[2] = p.footRatio * kChestNominal[d];
                o[3] = p.bendCents;
            } else { o[0] = -1.f; o[1] = o[2] = o[3] = 0.f; }
        }

        outputs[kOutReservoir - kOutFirst] = wind.reservoir * (1.f - wind.sag);
        outputs[kOutChestPd - kOutFirst] = wind.chest[kPedal];
        outputs[kOutChestGt - kOutFirst] = wind.chest[kGreat];
        outputs[kOutChestSw - kOutFirst] = wind.chest[kSwell];
        outputs[kOutDemand - kOutFirst] = wind.demand;
        outputs[kOutPipes - kOutFirst] = (float)speaking;
        outputs[kOutSeenLo - kOutFirst] = (float)seen[0];
        outputs[kOutSeenHi - kOutFirst] = (float)seen[1];
        outputs[kOutMembersLo - kOutFirst] = (float)lowerMembers;
        outputs[kOutMembersHi - kOutFirst] = (float)upperMembers;
        outputs[kOutBendLo - kOutFirst] = channels[1].bendRange;
        outputs[kOutBendHi - kOutFirst] = channels[14].bendRange;

        // swell shutters (parameter x expression pedal CC11)
        const float sw = std::max(0.f, std::min(1.f, params[kParamSwell] * expressionCC));
        swellGainT = 0.12f + 0.88f * std::pow(sw, 1.5f);
        swellCoefT = onePoleCoef(900.f + 17000.f * sw * sw, fs);
        blowerNoiseT = (params[kParamBlowerOn] >= 0.5f) ? std::pow(10.f, params[kParamBlowerNoise] / 20.f) : 0.f;
    }

    void renderVoices(float* L, float* R, int n) {
        float busL[kNumDivisions][kControlBlock] = {};
        float busR[kNumDivisions][kControlBlock] = {};
        const float inv = 1.f / (float)(kControlBlock);
        for (auto& v : voices) {
            if (!v.active) continue;
            float* bl = busL[v.division]; float* br = busR[v.division];
            // per-sample ramps towards this block's targets (over a whole control block)
            const float dBreath = (v.tBreath - v.curBreath) * inv;
            const float dNoise = (v.tNoise - v.curNoise) * inv;
            float env = v.env;
            if (!v.reed) {
                FlueCore& c = *flues[(size_t)v.slot];
                const float dBore = (v.tBore - v.curBore) * inv, dJet = (v.tJet - v.curJet) * inv;
                c.bias = v.curBias;
                for (int i = 0; i < n; ++i) {
                    v.curBreath += dBreath; v.curBore += dBore; v.curJet += dJet; v.curNoise += dNoise;
                    c.breath = v.curBreath; c.boreDelay = v.curBore; c.jetDelay = v.curJet; c.noiseAmt = v.curNoise;
                    float x = c.tick();
                    v.lp += v.lpCoef * (x - v.lp);
                    v.emphLp += v.emphCoef * (v.lp - v.emphLp);
                    x = (v.lp + v.emph * (v.lp - v.emphLp)) * v.gain;
                    bl[i] += x * v.gL; br[i] += x * v.gR;
                    const float a = std::fabs(x);
                    env = a > env ? a : env * 0.9995f;
                }
            } else {
                ReedCore& c = *reeds[(size_t)v.slot];
                const float dA = (v.tA - v.curA) * inv, dB = (v.tB - v.curB) * inv;
                for (int i = 0; i < n; ++i) {
                    v.curBreath += dBreath; v.curA += dA; v.curB += dB; v.curNoise += dNoise;
                    c.breath = v.curBreath; c.delayA = v.curA; c.delayB = v.curB; c.noiseAmt = v.curNoise;
                    float x = c.tick();
                    v.lp += v.lpCoef * (x - v.lp);
                    v.emphLp += v.emphCoef * (v.lp - v.emphLp);
                    x = (v.lp + v.emph * (v.lp - v.emphLp)) * v.gain;
                    bl[i] += x * v.gL; br[i] += x * v.gR;
                    const float a = std::fabs(x);
                    env = a > env ? a : env * 0.9995f;
                }
            }
            if (!std::isfinite(env)) { v.active = false; env = 0.f; }
            v.env = env;
        }
        // swell box: closing the shutters makes the division quieter and darker
        for (int i = 0; i < n; ++i) {
            swellGain += (swellGainT - swellGain) * 0.002f;
            swellCoef += (swellCoefT - swellCoef) * 0.002f;
            swellLpL += swellCoef * (busL[kSwell][i] - swellLpL);
            swellLpR += swellCoef * (busR[kSwell][i] - swellLpR);
            blowerGain += (blowerNoiseT - blowerGain) * 0.001f;
            // blower: low rumble
            const float w = blowerNoise.next();
            blowerLp += 0.004f * (w - blowerLp);
            const float hum = blowerLp * blowerGain * 6.f;
            L[i] = busL[kPedal][i] + busL[kGreat][i] + swellLpL * swellGain + hum;
            R[i] = busR[kPedal][i] + busR[kGreat][i] + swellLpR * swellGain + hum;
        }
    }

    // --------------------------------------------------------------- params
    void applyAllParams() {
        for (int i = kNumStops; i < kOutFirst; ++i) applyParam(i);
        wind.tremOn = params[kNumStops - 1] >= 0.5f;
    }
    void applyParam(int id) {
        const float v = params[id];
        switch (id) {
        case kParamTremSpeed: wind.tremSpeed = v; break;
        case kParamTremDepth: wind.tremDepth = v; break;
        case kParamTremTarget: wind.tremTarget = std::max(0, std::min(2, (int)v)); break;
        case kParamStability: wind.flexibility = v; break;
        case kParamReservoir: wind.reservoirTarget = v; break;
        case kParamConcussion: wind.concussion = v; break;
        case kParamBlowerOn: wind.blowerOn = v >= 0.5f; break;
        case kParamTuning: tuning.system = (int)v; break;
        case kParamEdoN: tuning.customN = (int)v; break;
        case kParamARef: tuning.aRef = v; break;
        case kParamRoom: room.preset = (int)v; room.configure(); break;
        case kParamDistance: room.distance = v; room.configure(); break;
        case kParamMicHeight: room.height = v; room.configure(); break;
        case kParamWidth: room.width = v; room.configure(); break;
        case kParamRoomMix: room.mix = v; room.configure(); break;
        case kParamPreDelay: room.preDelayMs = v; room.configure(); break;
        case kParamMicType: room.mic = (int)v; room.configure(); break;
        case kParamOutputGain: outGain = std::pow(10.f, v / 20.f); break;
        case kParamLimiter: limiter = v >= 0.5f; break;
        default: break;
        }
    }
    static float softClip(float x) {
        const float t = 0.89f;   // -1 dBFS
        if (std::fabs(x) <= t) return x;
        const float s = x > 0 ? 1.f : -1.f;
        return s * (t + (1.f - t) * std::tanh((std::fabs(x) - t) / (1.f - t)));
    }

public:
    float params[kNumParams];
    float outputs[kNumParams - kOutFirst];

private:
    float fs = 48000.f;
    int blockPos = 0;
    std::vector<std::unique_ptr<FlueCore>> flues;
    std::vector<std::unique_ptr<ReedCore>> reeds;
    std::vector<Voice> voices;
    Pallet pallets[kNumDivisions][kNumKeys];
    ChannelState channels[16];
    int seen[2] = { 0, 0 };
    float expressionCC = 1.f;
    bool altSpellingCC = false;
    uint32_t orderCounter = 0, ageCounter = 0;
    Tuning tuning;
    std::unique_ptr<Voicing> voicing, pendingVoicing;
    ScalaScale pendingScl; ScalaKbm pendingKbm;
    std::mutex stateMutex;
    std::atomic<bool> voicingDirty{false}, tuningDirty{false}, scalaDirty{false};
    WindSystem wind;
    Room room;
    float outGain = 0.5f;
    bool limiter = false;
    double cpuAvg = 0;
    float swellGain = 1.f, swellGainT = 1.f, swellCoef = 1.f, swellCoefT = 1.f, swellLpL = 0.f, swellLpR = 0.f;
    float blowerGain = 0.f, blowerNoiseT = 0.f, blowerLp = 0.f;
    Noise blowerNoise;
};

} // namespace pipe
