// Pipe - physically modelled pipe organ
// The engine: keys and pallets, MPE / polyphonic aftertouch, couplers, pipe
// voices, wind, swell box and the room. No plugin-framework dependencies, so
// it can be tested offline (tests/render_test.cpp).
#pragma once

#include "Params.hpp"
#include "Tuning.hpp"
#include "Voicing.hpp"
#include "Wind.hpp"
#include "RoomProcessor.hpp"
#include "Matrix.hpp"
#include "MpeInput.hpp"
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

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}

constexpr int kControlBlock = 32;
constexpr int kMaxFlueVoices = 448;
constexpr int kMaxReedVoices = 96;

struct Pallet {
    int holds = 0;
    float aperture = 0.f;
    float target = 0.f;
    float openTime = 0.02f, closeTime = 0.02f;   // seconds
    int srcChannel = 0, srcZone = 0;
    float chiff = 0.f;                           // attack transient envelope
    KeyPitch pitch;
    uint32_t order = 0;
    bool coupled = false;
    // expression
    float vel = 0.6f, relVel = -1.f;    // strike / release velocity 0..1 (<0: not sent)
    float sinceOn = 0.f;                // seconds since the key went down
    float modState[kNumModRows] = {};   // smoothed matrix row outputs
    bool modInit[kNumModRows] = {};
    float boost = 0.f, jet = 0.f, chiffScale = 1.f;
    bool palletDriven = false;
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
    float lpDark = 1.f, blkLp = 1.f, blkEmph = 0.f;   // tone on failing wind: upper partials go first
    float speakMin = 0.2f;      // foot pressure (re voiced) below which this pipe stops speaking
    float flutter = 0.f, flutterB = 0.f;   // slow random wavering near the speaking threshold
    uint32_t rng = 1;
    float emph = 0.f, emphCoef = 0.f, emphLp = 0.f;   // radiation shelf: narrow mouths and reed bells radiate highs
    float baseNoise = 0.01f, chiffAmt = 0.f, languid = 0.f;
    float bendLimit = 50.f;
    float flowWeight = 1.f;
    float curBreath = 0.f, curBore = 0.f, curJet = 0.f, curA = 0.f, curB = 0.f, curNoise = 0.f, curBias = 0.f;
    float tBreath = 0.f, tBore = 0.f, tJet = 0.f, tA = 0.f, tB = 0.f, tNoise = 0.f;   // control-block targets
    float env = 0.f;
    int quiet = 0;
    uint32_t age = 0;
    float level = 0.f, tLevel = 0.f, levelLag = 0.f;    // loudness follows the wind at the foot
    // tuner: holds the sounding pitch on the target (see Engine::tunerUpdate)
    float trim = 0.f;           // cents added to the model so that it sounds at the target
    float rotC = 1.f, rotS = 0.f, phC = 1.f, phS = 0.f;
    float tgtW = 0.f;           // target frequency, rad/sample
    double accRe = 0, accIm = 0, accE = 0;
    int accN = 0, winLen = 480, winMin = 64, winMax = 960, prevN = 0;
    float prevPh = 0.f, prevMag = 0.f;
    bool prevOk = false, cacheable = false, tunable = false;
    bool twice = false;         // model run at twice the sample rate (pipes only a few samples long)
    float rawEnv = 0.f;         // level inside the resonator (mean of |x| over about two periods)
};

class Engine {
public:
    Engine() {
        sink.e = this;
        flues.reserve(kMaxFlueVoices);
        for (int i = 0; i < kMaxFlueVoices; ++i) flues.emplace_back(new FlueCore());
        reeds.reserve(kMaxReedVoices);
        for (int i = 0; i < kMaxReedVoices; ++i) reeds.emplace_back(new ReedCore());
        voices.resize(kMaxFlueVoices + kMaxReedVoices);
        voicing = std::make_unique<Voicing>();
        pendingVoicing = std::make_unique<Voicing>();
        voicing->setDefaults();
        pendingVoicing->setDefaults();
        matrix.setDefaults();
        pendingMatrix.setDefaults();
        for (int i = 0; i < 128; ++i) { ccVal[i] = 0.f; ccSeen[i] = false; ccZone[i] = 0; }
        for (int i = 0; i < kNumParams; ++i) params[i] = paramInfo(i).def;
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
        limRelease = 1.f - std::exp(-1.f / (0.08f * fs));   // 80 ms
        limGain = 1.f;
        room.prepare(sampleRate);
        applyAllParams();
        wind.reset();
        windLive = false;
        for (auto& v : voices) v.active = false;
        for (int d = 0; d < kNumDivisions; ++d) for (int k = 0; k < kNumKeys; ++k) pallets[d][k] = Pallet();
        std::memset(trimValid, 0, sizeof(trimValid));
        room.clear();
        for (int d = 0; d < kNumDivisions; ++d) { swellLpL = swellLpR = 0.f; }
    }

    // ---------------------------------------------------------------- params
    void setParameter(int id, float v) {
        if (id < 0 || id >= kOutFirst) return;
        const float old = params[id];
        params[id] = v;
        // A key's remembered tuning trim belongs to the pipe it played; once the keys
        // play other pipes, the trims are learnt again.
        if (v != old && (id == kParamTuning || id == kParamEdoN || id == kParamAnchorKey || id == kParamMapLo
                         || id == kParamMapHi || id == kParamMapPed || id == kParamEnharmonic))
            std::memset(trimValid, 0, sizeof(trimValid));
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
    void setMatrixText(const std::string& text) {
        std::lock_guard<std::mutex> lk(stateMutex);
        pendingMatrix.parse(text);
        matrixDirty = true;
    }
    void setCustomIrPath(const std::string& path) { room.setCustomPath(path); }
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
    // All MIDI / MPE interpretation lives in MpeInput (see the rules there); the engine only
    // turns notes into pallets and reads each note's expression for the matrix.
    void midi(const uint8_t* d, int size) { mpe.process(d, size, sink); }

    void allNotesOff() {
        for (int dv = 0; dv < kNumDivisions; ++dv)
            for (int k = 0; k < kNumKeys; ++k) { pallets[dv][k].holds = 0; pallets[dv][k].target = 0.f; }
        notes.clear();
        mpe.reset();
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
            prePeak = std::max(prePeak, std::max(std::fabs(l), std::fabs(r)));
            if (limiter) {
                // zero-latency peak limiter: instant attack, smooth release, stereo-linked
                const float pk = std::max(std::fabs(l), std::fabs(r));
                const float want = pk > kLimitCeiling ? kLimitCeiling / pk : 1.f;
                limGain = want < limGain ? want : limGain + (want - limGain) * limRelease;
                minLimGain = std::min(minLimGain, limGain);
                l *= limGain; r *= limGain;
            }
            outL[i] = l; outR[i] = r;
            peakL = std::max(peakL, std::fabs(l)); peakR = std::max(peakR, std::fabs(r));
        }
        outputs[kOutPeakL - kOutFirst] = std::max(peakL, outputs[kOutPeakL - kOutFirst] * 0.9f);
        outputs[kOutPeakR - kOutFirst] = std::max(peakR, outputs[kOutPeakR - kOutFirst] * 0.9f);
        outputs[kOutClip - kOutFirst] = std::max(prePeak, outputs[kOutClip - kOutFirst] * 0.995f);
        outputs[kOutLimitDb - kOutFirst] = std::max(-20.f * std::log10(std::max(1e-6f, minLimGain)), outputs[kOutLimitDb - kOutFirst] * 0.9f);
        prePeak = 0.f; minLimGain = 1.f;
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double budget = n / (double)fs;
        cpuAvg += 0.05 * (100.0 * el / budget - cpuAvg);
        outputs[kOutCpu - kOutFirst] = (float)cpuAvg;
    }

    // ------------------------------------------------------------ for tests
    const Voice* debugVoice(int rank, int key) const { for (const auto& v : voices) if (v.active && v.rank == rank && v.key == key) return &v; return nullptr; }
    float debugTrim(int rank, int key) const { for (const auto& v : voices) if (v.active && v.rank == rank && v.key == key) return v.trim; return trimValid[rank][key] ? trimCache[rank][key] : 0.f; }
    float debugAperture(int d, int k) const { return pallets[d][k].aperture; }
    float debugFootMm(int d, int k) const { return pallets[d][k].footRatio * kChestNominal[d]; }
    float debugBoostMm(int d, int k) const { return pallets[d][k].boost * (params[kParamPressMax] - 78.f); }
    int debugHolds(int d, int k) const { return pallets[d][k].holds; }
    float debugBendSemis(int ch, int key) const { const MpeInput::Note* n = mpe.find(ch, key); return n ? mpe.bendSemitones(*n) : -999.f; }

    // -------------------------------------------------------- info for the UI
    const Tuning& getTuning() const { return tuning; }
    Tuning& tuningMut() { return tuning; }
    float sampleRate() const { return fs; }

private:
    // ------------------------------------------------------------ MIDI input
    MpeInput mpe;
    struct Sink : MpeInput::Listener {
        Engine* e = nullptr;
        void onNoteOn(int ch, int key, int vel, int zone) override { e->noteOn(ch, key, vel, zone); }
        void onNoteOff(int ch, int key, int vel) override { e->noteOff(ch, key, vel); }
        void onAllNotesOff(int mask) override { e->releaseChannels(mask); }
        void onController(int ch, int cc, int val, int zone) override { e->controller(ch, cc, val, zone); }
        void onLearn(int src, int cc) override { e->learn(src, cc); }
    } sink;

    // ordinary controllers (MpeInput keeps the MPE ones): matrix CC sources, enharmonic spelling
    void controller(int ch, int cc, int val, int zone) {
        (void)ch;
        if (cc == 66) altSpellingCC = val >= 64;
        if (cc < 120) {
            ccVal[cc] = val / 127.f; ccSeen[cc] = true; ccZone[cc] = zone;
            learn(kSrcCC, cc);
        }
    }

    void learn(int src, int cc) {
        ++learnCounter;
        outputs[kOutLearn - kOutFirst] = (float)(src * 128 + cc) + 1024.f * (float)(learnCounter % 4096);
    }

    struct ActiveNote { int ch, key; int div[3]; int nDiv; };
    std::vector<ActiveNote> notes;

    void noteOn(int ch, int key, int vel, int zone) {
        int div = (int)(zone == 0 ? params[kParamZoneLoDiv] : params[kParamZoneHiDiv]);
        int mapMode = (int)(zone == 0 ? params[kParamMapLo] : params[kParamMapHi]);
        const int split = (int)params[kParamPedalSplit];
        if (zone == 0 && split > 0 && key < split) { div = kPedal; mapMode = (int)params[kParamMapPed]; }
        if (div < 0 || div >= kNumDivisions) return;
        const bool alt = (params[kParamEnharmonic] >= 0.5f) != altSpellingCC;
        const KeyPitch kp = tuning.keyPitch(key, mapMode, alt);
        if (!kp.valid) { silentKey = 1000 + key; silentTimer = 3.f; return; }
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
        // tell the player why a key makes no sound: no pipe at that pitch, or no stop drawn
        if (an.nDiv == 0) { silentKey = 1000 + key; silentTimer = 3.f; }
        else {
            bool any = false;
            for (int j = 0; j < an.nDiv && !any; ++j)
                for (int r = 0; r < kNumRanks && !any; ++r) {
                    const int s = stopForRank(r);
                    any = kRanks[r].division == an.div[j] && s >= 0 && stopOn(s)
                          && !(kRanks[r].lowestKey > 0 && kp.keyFloat < kRanks[r].lowestKey - 0.5f);
                }
            if (!any) { silentKey = 2000 + key; silentTimer = 3.f; } else silentTimer = 0.f;
        }
    }

    void releaseNote(size_t i, int relVel) {
        const int key = notes[i].key;
        for (int j = 0; j < notes[i].nDiv; ++j) {
            Pallet& p = pallets[notes[i].div[j]][key];
            if (p.holds > 0 && --p.holds == 0) {
                p.target = 0.f;
                p.relVel = (relVel != 64 && relVel > 0) ? relVel / 127.f : -1.f;
                p.closeTime = params[kParamReleaseMs] * 0.001f;   // the matrix may change it
            }
        }
        notes.erase(notes.begin() + (long)i);
    }
    void noteOff(int ch, int key, int relVel) {
        for (size_t i = 0; i < notes.size(); ++i)
            if (notes[i].ch == ch && notes[i].key == key) { releaseNote(i, relVel); return; }
    }
    void releaseChannels(int mask) {
        for (size_t i = notes.size(); i-- > 0;) if (mask & (1 << notes[i].ch)) releaseNote(i, 64);
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
        p.openTime = slow + (fast - slow) * 0.6f;   // the matrix may change it
        p.pitch = kp;
        p.order = ++orderCounter;
        p.vel = vel; p.relVel = -1.f; p.sinceOn = 0.f;
        if (wasClosed) {
            p.chiff = 1.f;
            p.palletDriven = false;
            for (int i = 0; i < kNumModRows; ++i) p.modInit[i] = false;
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
    // Frequency the pipe under (rank, key) is tuned to right now.
    double pipeFrequency(int rank, const Pallet& p, int key) const {
        const RankDef& rd = kRanks[rank];
        double f = p.pitch.hz * rankRatio(rank, key);
        // tuning offsets: rank, pipe, celeste, age, temperature
        double cents = voicing->pipeTune[rank][p.pitch.pipeIndex] + rd.detuneCents;
        const float drift = params[kParamAgeDrift];
        if (drift > 0) cents += drift * ((hash32((uint32_t)(rank * 4096 + p.pitch.pipeIndex)) & 0xFFFF) / 32767.5f - 1.f);
        const double tC = params[kParamPipeTemp];
        const double tempCents = 1200.0 * std::log2(std::sqrt((273.15 + tC) / (273.15 + 18.0)));
        cents += rd.kind == kKindReed ? 0.15 * tempCents : tempCents;
        return f * std::pow(2.0, cents / 1200.0);
    }

    // The smallest pipes are only a few samples long at the host's rate, too coarse
    // for the jet model; they run at twice the rate.
    float modelRate(const Voice& v) const { return v.twice ? 2.f * fs : fs; }

    // Resonator lengths for the voice's frequency.
    void tuneVoice(Voice& v) {
        const double f = v.freq;
        if (!v.reed) {
            const RankDef& rd = kRanks[v.rank];
            const double mfs = modelRate(v);
            const double T = mfs / f;
            const double jet = std::min((double)kJetSize - 8.0, v.jr * T);
            const double bore = solveFlueBore(*flues[(size_t)v.slot], f, mfs, jet, rd.kind == kKindStopped ? T * 0.5 : T);
            // the bore line is read before it is written, which is one more sample round the loop
            v.baseJet = (float)jet; v.baseBore = (float)std::max(4.0, bore - 1.0);
        } else {
            const auto& rc = kReedFamily[v.family];
            // one period round the two sections, less what the loop already holds: the
            // loss filter's phase delay and one sample for each line (read before write)
            const double w = 2.0 * M_PI * f / fs, a = reeds[(size_t)v.slot]->aBore;
            const double lag = std::atan2((1.0 - a) * std::sin(w), 1.0 - (1.0 - a) * std::cos(w));
            const double D = std::max(8.0, fs / f - lag / w - 2.0);
            v.baseA = (float)((1.0 - rc.position) * D);
            v.baseB = (float)(rc.position * D);
        }
        // short windows catch a large error, long ones settle to a fraction of a cent
        v.winMin = std::max(64, (int)(6.0 * fs / f));
        v.winMax = std::max((int)(0.020f * fs), (int)(24.0 * fs / f));
        v.winLen = v.winMin;
        v.levelLag = 1.f - std::exp(-(kControlBlock / fs) / std::max(0.003f, 3.f / (float)f));
        // until a pipe has been heard once, the usual residual of its model stands in
        v.trim = trimValid[v.rank][v.key] ? trimCache[v.rank][v.key] : v.reed ? -4.f : 14.f;
        v.prevOk = false; v.accRe = v.accIm = v.accE = 0; v.accN = 0;
    }

    void startVoice(int rank, int d, int key) {
        const RankDef& rd = kRanks[rank];
        const Pallet& p = pallets[d][key];
        if (rd.lowestKey > 0 && p.pitch.keyFloat < rd.lowestKey - 0.5f) return;
        const bool isReed = rd.kind == kKindReed;
        const double f = pipeFrequency(rank, p, key);
        if (f > 0.42 * fs || f < 15.0) return;
        // already speaking (re-struck or stop redrawn)? It keeps sounding, at today's tuning.
        for (auto& v : voices) if (v.active && v.rank == rank && v.key == key && v.division == d) {
            v.stopClosed = false;
            v.freq = (float)f; tuneVoice(v);
            return;
        }

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
        v.rng = hash32((uint32_t)(rank * 7919 + key * 104729 + (int)v.age)) | 1u;

        // voicing belongs to a place in the compass, i.e. to the pitch the key plays, not to the key number
        const int k = std::max(0, std::min(kNumKeys - 1, (int)std::lround(p.pitch.keyFloat)));
        const PipeGeometry g = pipeGeometry(*voicing, rank, k, f);
        v.voicedRatio = (float)(g.toeMM / kChestNominal[d]);
        v.flowWeight = (float)g.flowWeight;
        const float reg = std::pow(10.f, voicing->rank[rank].key[kVRegulation][k] / 20.f);
        const uint32_t seed = hash32((uint32_t)(rank * 131071 + key * 31 + d));

        if (!isReed) {
            v.family = rd.family;
            v.jr = (float)std::max(kJetModelMin, std::min(kJetModelMax, modelJetRatio(g.jetRatio)));
            FlueCore& core = *flues[(size_t)v.slot];
            core.reset();
            core.noise.seed(seed);
            const auto& fc = kFlueFamily[v.family];
            core.sign = rd.kind == kKindStopped ? -1.f : 1.f;
            core.loopGain = fc.loopGain;
            v.twice = f > 0.09 * fs;
            const float mfs = modelRate(v);
            core.aBore = onePoleCoef(fc.boreFc * (float)f, mfs);
            core.aJet = onePoleCoef(fc.jetFc * (float)f, mfs);
            core.aDc = onePoleCoef(kDcFc * (float)f, mfs);
            tuneVoice(v);
            core.boreDelay = v.baseBore; core.jetDelay = v.baseJet;
            const float nick = voicing->rank[rank].key[kVNicking][k];
            v.baseNoise = 0.02f * (1.f - 0.3f * nick);
            v.chiffAmt = voicing->rank[rank].key[kVChiff][k] * 0.35f * (1.f - 0.25f * nick);
            v.languid = voicing->rank[rank].key[kVLanguid][k];
            // radiation / scale: wide pipes are mellow, narrow pipes bright
            float fcOut = (float)(3.0 * kSpeedOfSound / (M_PI * g.diameterMM * 0.001));
            if (rd.kind == kKindStopped) fcOut *= 0.7f;
            v.lpCoef = onePoleCoef(std::max(1200.f, fcOut), fs);
            v.lpDark = onePoleCoef(std::max(250.f, 2.5f * (float)f), fs);
            // Where a pipe stops speaking as its wind fails depends on its mouth: a high
            // cut-up (large jet ratio) needs more wind. No two pipes stop at the same pressure.
            v.speakMin = 0.13f + 0.35f * std::max(0.f, (float)v.jr - (float)kJetModelMin)
                       + 0.03f * ((seed >> 8 & 0xFF) / 127.5f - 1.f);
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
            ReedCore& core = *reeds[(size_t)v.slot];
            core.reset();
            core.noise.seed(seed);
            core.reedSlope = rc.slope; core.reedOffset = 0.7f;
            core.aBore = onePoleCoef(rc.boreFc * (float)f, fs);
            core.aDc = onePoleCoef(kDcFc * (float)f, fs);
            tuneVoice(v);
            core.delayA = v.baseA; core.delayB = v.baseB;
            v.baseNoise = 0.005f;
            v.chiffAmt = 0.06f;     // the tongue's first flutter
            v.lpCoef = onePoleCoef(std::min(0.45f * fs, (float)f * 14.f + 2500.f), fs);
            v.lpDark = onePoleCoef(std::max(300.f, 3.f * (float)f), fs);
            v.speakMin = 0.32f + 0.04f * ((seed >> 8 & 0xFF) / 127.5f - 1.f);   // a reed tongue needs more wind to start beating
            v.emph = v.family == kReedOboe ? 2.5f : 2.0f;
            v.emphCoef = onePoleCoef(3.0f * (float)f, fs);
            v.gain = 0.16f * reg;
            v.bendLimit = params[kParamBendReed];
            v.curA = v.baseA; v.curB = v.baseB;
        }
        v.blkLp = v.lpCoef; v.blkEmph = v.emph;
        // where the pipe stands in the case: C side left, C# side right;
        // big pipes outside, pedal towers at the flanks, swell box in the middle
        const float side = (key % 2 == 0) ? -1.f : 1.f;
        const float pos01 = std::max(0.f, std::min(1.f, (p.pitch.keyFloat - 36.f) / 57.f));
        float spread = d == kPedal ? 0.85f : d == kSwell ? 0.18f + 0.25f * (1.f - pos01) : 0.25f + 0.55f * (1.f - pos01);
        const float pan = side * spread;
        v.gL = std::cos((pan + 1.f) * (float)M_PI * 0.25f);
        v.gR = std::sin((pan + 1.f) * (float)M_PI * 0.25f);
    }

    // The pipe models are self-oscillating loops whose pitch is pulled a few
    // cents by the exciter, by the interpolated delays and by voicing. Each
    // voice therefore measures its own fundamental against the frequency it is
    // meant to sound (phase drift of a quadrature detector running at the
    // target) and trims its resonator until the two agree. The trim is
    // remembered per pipe, so a pipe that has spoken once starts in tune.
    void tunerUpdate(Voice& v) {
        const double re = v.accRe, im = v.accIm, e = v.accE; const int n = v.accN;
        v.accRe = v.accIm = v.accE = 0; v.accN = 0;
        const double mag2 = re * re + im * im;
        const bool ok = e / n > 1e-5 && 2.0 * mag2 > 0.1 * (double)n * e && v.tgtW > 0.f;
        const float ph = (float)std::atan2(im, re), mag = (float)(std::sqrt(mag2) / n);
        // only a settled pipe is measured: while it is still building up, its pitch is on the move
        if (ok && v.prevOk && v.tunable && mag > 0.95f * v.prevMag && mag < 1.05f * v.prevMag) {
            float d = ph - v.prevPh;
            if (d > (float)M_PI) d -= 2.f * (float)M_PI; else if (d < -(float)M_PI) d += 2.f * (float)M_PI;
            const float spacing = 0.5f * (float)(n + v.prevN);
            float err = 1200.f * std::log2(std::max(0.5f, 1.f + d / (spacing * v.tgtW)));   // cents sharp of target
            err = std::max(-40.f, std::min(40.f, err));
            v.trim = std::max(-150.f, std::min(150.f, v.trim - (std::fabs(err) > 2.f ? 0.6f : 0.3f) * err));
            if (v.cacheable) { trimCache[v.rank][v.key] = v.trim; trimValid[v.rank][v.key] = true; }
            if (std::fabs(err) < 3.f) v.winLen = std::min(v.winMax, v.winLen * 2);
            else if (std::fabs(err) > 10.f) v.winLen = v.winMin;
        }
        v.prevOk = ok && v.tunable; v.prevPh = ph; v.prevMag = mag; v.prevN = n;
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
        if ((voicingDirty || tuningDirty || scalaDirty || matrixDirty) && stateMutex.try_lock()) {
            if (voicingDirty) { std::memcpy(voicing->rank, pendingVoicing->rank, sizeof(voicing->rank)); voicingDirty = false; }
            if (tuningDirty) { std::memcpy(voicing->pipeTune, pendingVoicing->pipeTune, sizeof(voicing->pipeTune)); tuningDirty = false; }
            if (scalaDirty) { std::swap(tuning.scl, pendingScl); std::swap(tuning.kbm, pendingKbm); scalaDirty = false; }
            if (matrixDirty) { matrix = pendingMatrix; matrixDirty = false; }
            stateMutex.unlock();
        }
        const float dt = kControlBlock / fs;
        if (silentTimer > 0.f) silentTimer -= dt;
        outputs[kOutSilentKey - kOutFirst] = silentTimer > 0.f ? (float)silentKey : 0.f;

        // wind
        float flow = 0.f; int speaking = 0;
        for (const auto& v : voices) {
            if (!v.active) continue;
            ++speaking;
            const Pallet& p = pallets[v.division][v.key];
            if (!v.stopClosed) flow += v.flowWeight * p.aperture * std::sqrt(std::max(0.f, p.footRatio));
        }
        wind.update(dt, flow);
        windLive = true;

        // pallets: the expression matrix decides how each key's mechanisms move
        const float fastT = params[kParamPalletFast] * 0.001f, slowT = params[kParamPalletSlow] * 0.001f;
        const float boostSpan = (params[kParamPressMax] - 78.f) / 78.f;
        Pallet* latest = nullptr;
        for (int d = 0; d < kNumDivisions; ++d) {
            const float chestRatio = wind.chest[d] / kChestNominal[d];
            for (int k = 0; k < kNumKeys; ++k) {
                Pallet& p = pallets[d][k];
                if (p.holds == 0 && p.aperture <= 0.f) continue;
                p.sinceOn += dt;
                float sum[kNumModDests] = {}; bool has[kNumModDests] = {};
                for (int i = 0; i < kNumModRows; ++i) {
                    const ModRow& r = matrix.rows[i];
                    if (!r.on || modDestGlobal(r.dest)) continue;
                    const int z = r.src == kSrcCC ? ccZone[r.cc] : p.srcZone;
                    if (r.zone != kZoneAll && r.zone != z) continue;
                    float x;
                    if (!sourceValue(r, p, k, x)) continue;   // not sent: the voiced default stands
                    float out = r.eval(x);
                    if (r.dest == kDstPitch) out *= r.src == kSrcBend ? 4800.f : 100.f;
                    if (r.smoothMs > 0.f && p.modInit[i]) p.modState[i] += (1.f - std::exp(-dt * 1000.f / r.smoothMs)) * (out - p.modState[i]);
                    else { p.modState[i] = out; p.modInit[i] = true; }
                    sum[r.dest] += p.modState[i]; has[r.dest] = true;
                }
                auto clamp01 = [](float v) { return std::max(0.f, std::min(1.f, v)); };
                if (has[kDstOpenSpeed]) p.openTime = slowT + (fastT - slowT) * clamp01(sum[kDstOpenSpeed]);
                if (has[kDstCloseSpeed] && p.holds == 0) p.closeTime = 0.080f - 0.075f * clamp01(sum[kDstCloseSpeed]);
                p.boost = has[kDstBoost] ? std::max(-1.f, std::min(1.f, sum[kDstBoost])) : 0.f;
                p.jet = has[kDstJet] ? std::max(-1.f, std::min(1.f, sum[kDstJet])) : 0.f;
                p.bendCents = has[kDstPitch] ? sum[kDstPitch] : 0.f;
                p.chiffScale = has[kDstChiff] ? 2.f * clamp01(sum[kDstChiff]) : 1.f;
                if (p.holds > 0) {
                    if (has[kDstPallet]) { p.target = clamp01(sum[kDstPallet]); p.palletDriven = true; }
                    else if (!p.palletDriven) p.target = 1.f;
                }
                // the key moves the pallet directly when it is pressure-driven
                const float tOpen = p.palletDriven ? fastT : p.openTime;
                const float tc = p.target > p.aperture ? tOpen : (p.holds > 0 && p.palletDriven ? fastT : p.closeTime);
                const float step = dt / std::max(0.001f, tc);
                if (p.target > p.aperture) p.aperture = std::min(p.target, p.aperture + step);
                else p.aperture = std::max(p.target, p.aperture - step);
                p.chiff *= std::exp(-dt / 0.035f);
                const float a = p.aperture;
                const float open = a * a / (a * a + 0.0144f) * 1.0144f;
                // second pallet from the high-pressure chest adds wind (or a throttle takes it away)
                p.footRatio = chestRatio * open * std::max(0.f, 1.f + p.boost * boostSpan);
                if (p.holds > 0 && (!latest || p.order > latest->order)) latest = &p;
            }
        }
        if (latest) lastPallet = latest;

        // whole-organ destinations (swell box, tremulant)
        {
            float sum[kNumModDests] = {}; bool has[kNumModDests] = {};
            for (int i = 0; i < kNumModRows; ++i) {
                const ModRow& r = matrix.rows[i];
                if (!r.on || !modDestGlobal(r.dest)) continue;
                float x;
                if (r.src == kSrcCC) {
                    if (!ccSeen[r.cc] || (r.zone != kZoneAll && r.zone != ccZone[r.cc])) continue;
                    x = ccVal[r.cc];
                } else {
                    if (!lastPallet || (r.zone != kZoneAll && r.zone != lastPallet->srcZone) || !sourceValue(r, *lastPallet, (int)((lastPallet - &pallets[0][0]) % kNumKeys), x)) continue;
                }
                const float out = r.eval(x);
                if (r.smoothMs > 0.f && globalInit[i]) globalState[i] += (1.f - std::exp(-dt * 1000.f / r.smoothMs)) * (out - globalState[i]);
                else { globalState[i] = out; globalInit[i] = true; }
                sum[r.dest] += globalState[i]; has[r.dest] = true;
            }
            auto clamp01 = [](float v) { return std::max(0.f, std::min(1.f, v)); };
            swellEff = has[kDstSwell] ? params[kParamSwell] * clamp01(sum[kDstSwell]) : params[kParamSwell];
            wind.tremDepth = has[kDstTremDepth] ? params[kParamTremDepth] * clamp01(sum[kDstTremDepth]) : params[kParamTremDepth];
            wind.tremSpeed = has[kDstTremSpeed] ? params[kParamTremSpeed] * (0.6f + 0.8f * clamp01(sum[kDstTremSpeed])) : params[kParamTremSpeed];
        }

        // voices: targets for this block
        const bool strict = params[kParamStrict] >= 0.5f;
        for (auto& v : voices) {
            if (!v.active) continue;
            const Pallet& p = pallets[v.division][v.key];
            // foot pressure relative to the pressure the pipe was voiced on
            const float ratio = v.stopClosed ? 0.f : std::max(0.f, p.footRatio);
            float bend = p.bendCents;
            if (strict) bend = std::max(-v.bendLimit, std::min(v.bendLimit, bend));
            // How a pipe answers its wind. More wind than it was voiced for: louder and, for a
            // flue, a few cents sharp. Less: quieter, flatter, duller and breathier, and near
            // its own threshold it wavers, then stops. A flue falls towards a semitone flat
            // just before it stops (measured on real pipes as their wind decays); a reed
            // keeps closer to pitch, held by its tongue, and its tone follows its blowing
            // pressure in the model itself.
            const float rT = v.speakMin;
            const float fail = std::max(0.f, std::min(1.f, (1.f - ratio) / std::max(0.05f, 1.f - rT)));
            const float windCents = v.reed ? 0.f
                : ratio >= 1.f ? std::min(6.f, 12.f * std::log2(ratio))
                : 12.f * std::log2(std::max(rT, ratio)) - 85.f * fail * fail * fail;
            const float bright = std::pow(std::max(0.f, std::min(1.f, (ratio - rT) / std::max(0.05f, 0.75f - rT))), 0.7f);
            // wavering near the threshold: slow random movement of the jet / tongue
            const float waver = std::max(0.f, std::min(1.f, (1.7f * rT - ratio) / (0.7f * rT)));
            {
                v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
                const float r01 = (v.rng & 0xFFFF) / 32767.5f - 1.f;
                const float a = 1.f - std::exp(-dt * 2.f * (float)M_PI * 7.f);
                v.flutter += a * (r01 - v.flutter); v.flutterB += a * (v.flutter - v.flutterB);
            }
            const float wobble = waver * 3.f * v.flutterB;          // about +-1 at full wavering
            const float cents = bend + windCents + (v.reed ? 4.f : 12.f) * wobble;
            const float pitchMul = std::pow(2.f, -(cents + v.trim) / 1200.f);
            v.tgtW = 2.f * (float)M_PI * v.freq * std::pow(2.f, cents / 1200.f) / fs;
            v.rotC = std::cos(v.tgtW); v.rotS = std::sin(v.tgtW);
            v.cacheable = std::fabs(bend) < 0.5f && ratio > 0.9f && ratio < 1.1f;
            // The tuner holds a flue on the pressure law above; a reed's own pitch movement
            // is part of its sound, so it is only trimmed on steady wind.
            // (and not during a reed's attack, while its pitch is still rising: ~40 periods)
            const bool tunable = v.reed ? (ratio > 0.9f && ratio < 1.1f && p.sinceOn * v.freq > 50.f) : ratio > 0.5f;
            if (tunable != v.tunable) { v.tunable = tunable; v.prevOk = false; v.winLen = v.winMin; }
            const float over = 1.f + 0.1f * std::max(0.f, std::min(1.f, ratio - 1.f));
            // exciter drive: on its operating point down to near the threshold, then below
            // the point where the model can oscillate (flue ~0.84, reed ~0.55 of voiced)
            const float onset = std::max(0.f, std::min(1.f, (ratio - 0.8f * rT) / (0.5f * rT)));
            const float on = onset * onset * (3.f - 2.f * onset);
            float targetBreath, tBore = 0, tJet = 0, tA = 0, tB = 0;
            if (!v.reed) {
                targetBreath = kFlueVoicedBreath * (0.80f + 0.20f * on) * over * (1.f + 0.04f * wobble);
                tBore = v.baseBore * pitchMul;
                tJet = std::min((float)(kJetSize - 8), v.baseJet * pitchMul);
                v.curBias = v.languid * 0.04f + p.jet * 0.07f;
            } else {
                // the reed's blowing pressure follows the foot pressure through its speaking window
                const float span = std::max(0.f, std::min(1.f, (ratio - 0.8f * rT) / (1.f - 0.8f * rT)));
                targetBreath = (0.30f + (kReedVoicedBreath - 0.30f) * std::pow(span, 0.6f)) * (1.f + 0.03f * wobble);
                tA = v.baseA * pitchMul; tB = v.baseB * pitchMul;
            }
            // targets stored for per-sample ramps
            v.tBreath = targetBreath;
            v.tBore = tBore; v.tJet = tJet; v.tA = tA; v.tB = tB;
            v.tNoise = v.baseNoise * (1.f + 3.f * (1.f - bright)) + v.chiffAmt * p.chiff * p.chiffScale;
            v.blkLp = v.lpDark + (v.lpCoef - v.lpDark) * bright;
            v.blkEmph = v.emph * bright;
            v.tLevel += v.levelLag * (std::pow(ratio, 0.8f) * (1.f + 0.35f * wobble) - v.tLevel);

            // End of a pipe's life: its pallet has been let go and has shut (or its stop is
            // in) and the sound has died away. A pallet that is held but not yet open - a
            // key resting at the top of its pressure travel - keeps its pipes.
            const bool shut = v.stopClosed || (p.holds == 0 && p.aperture <= 0.f);
            if (shut && v.env < 2e-5f) { if (++v.quiet > 8) v.active = false; }
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
        outputs[kOutLid - kOutFirst] = wind.lid;
        outputs[kOutFan - kOutFirst] = wind.fan;
        outputs[kOutWindLeft - kOutFirst] = wind.secondsLeft;
        outputs[kOutPipes - kOutFirst] = (float)speaking;
        outputs[kOutSeenLo - kOutFirst] = (float)mpe.inputBits(0);
        outputs[kOutSeenHi - kOutFirst] = (float)mpe.inputBits(1);
        // channels notes are arriving on: now, and over the last few seconds
        const uint16_t now = mpe.channelsWithNotes();
        recentChannels |= now;
        if (now) idleSeconds = 0.f; else if ((idleSeconds += kControlBlock * 16 / fs) > 5.f) recentChannels = 0;
        outputs[kOutViaLo - kOutFirst] = (float)now;
        outputs[kOutViaHi - kOutFirst] = (float)(recentChannels | (mpe.maxNotesOnOneChannel() > 1 ? 1 << 16 : 0));
        outputs[kOutMembersLo - kOutFirst] = (float)mpe.lowerMembers();
        outputs[kOutMembersHi - kOutFirst] = (float)mpe.upperMembers();
        outputs[kOutBendLo - kOutFirst] = mpe.channel(1).bendRange;
        outputs[kOutBendHi - kOutFirst] = mpe.channel(14).bendRange;
        liveOutputs();
        outputs[kOutRoomStatus - kOutFirst] = (float)room.status();
        outputs[kOutRoomPos - kOutFirst] = (float)room.usedPosition();
        outputs[kOutRoomPosDist - kOutFirst] = room.usedDistance();

        // swell shutters (parameter, and whatever the matrix sends to them)
        const float sw = std::max(0.f, std::min(1.f, swellEff));
        swellGainT = 0.12f + 0.88f * std::pow(sw, 1.5f);
        swellCoefT = onePoleCoef(900.f + 17000.f * sw * sw, fs);
        blowerNoiseT = std::pow(10.f, params[kParamBlowerNoise] / 20.f) * wind.fan * wind.fan;   // the blower's hum follows its speed
    }

    // The value a matrix source has for this key's note, or false if that source does not exist
    // for it (see MpeInput). A released note no longer follows its channel (MPE §3.3).
    bool sourceValue(const ModRow& r, const Pallet& p, int key, float& x) const {
        if (r.src == kSrcStrike) { x = p.vel; return true; }
        if (r.src == kSrcRelease) { if (p.relVel < 0.f) return false; x = p.relVel; return true; }
        if (r.src == kSrcCC) { if (!ccSeen[r.cc]) return false; x = ccVal[r.cc]; return true; }
        const MpeInput::Note* n = mpe.find(p.srcChannel, key);
        if (!n) return false;
        switch (r.src) {
        case kSrcPressure: return mpe.pressure(*n, x);
        case kSrcAftertouch: return mpe.aftertouch(*n, x);
        case kSrcCC74: return mpe.cc74(*n, x);
        case kSrcBend: x = std::max(0.f, std::min(1.f, 0.5f + mpe.bendSemitones(*n) / 96.f)); return true;
        default: return false;
        }
    }
    void liveOutputs() {
        float* o = outputs;
        auto set = [&](int id, float v) { o[id - kOutFirst] = v; };
        const Pallet* p = lastPallet;
        if (!p) { set(kOutLiveKey, -1.f); return; }
        const int d = (int)(p - &pallets[0][0]) / kNumKeys;
        const int key = (int)((p - &pallets[0][0]) % kNumKeys);
        const MpeInput::Note* n = mpe.find(p->srcChannel, key);
        float v = 0.f;
        set(kOutLiveKey, (float)key);
        set(kOutLiveZone, (float)p->srcZone);
        set(kOutLiveStrike, p->vel * 127.f);
        set(kOutLivePressure, n && mpe.pressure(*n, v) ? v * 127.f : -1.f);
        set(kOutLiveAftertouch, n && mpe.aftertouch(*n, v) ? v * 127.f : -1.f);
        set(kOutLiveCC74, n && mpe.cc74(*n, v) ? v * 127.f : -1.f);
        set(kOutLiveBend, n ? mpe.bendSemitones(*n) * 100.f : 0.f);
        set(kOutLiveChannel, (float)p->srcChannel);
        set(kOutDstPallet, p->aperture * 100.f);
        set(kOutDstBoost, p->boost * (params[kParamPressMax] - 78.f));
        set(kOutDstJet, p->jet);
        set(kOutDstPitch, p->bendCents);
        set(kOutDstChiff, p->chiffScale);
        set(kOutDstFoot, p->footRatio * kChestNominal[d]);
        set(kOutSwellEff, swellEff * 100.f);
        set(kOutTremEff, wind.tremOn ? wind.tremDepth * 100.f : -1.f);
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
            const float dLevel = (v.tLevel - v.level) * inv;
            float env = v.env;
            float pc = v.phC, ps = v.phS, aRe = 0.f, aIm = 0.f, aE = 0.f;
            float rawEnv = v.rawEnv; const float rawCoef = std::min(1.f, 0.5f * v.freq / fs);   // averages over about two periods
            if (!v.reed) {
                FlueCore& c = *flues[(size_t)v.slot];
                const float dBore = (v.tBore - v.curBore) * inv, dJet = (v.tJet - v.curJet) * inv;
                c.bias = v.curBias;
                for (int i = 0; i < n; ++i) {
                    v.curBreath += dBreath; v.curBore += dBore; v.curJet += dJet; v.curNoise += dNoise; v.level += dLevel;
                    c.breath = v.curBreath; c.boreDelay = v.curBore; c.jetDelay = v.curJet; c.noiseAmt = v.curNoise;
                    float x = c.tick();
                    if (v.twice) x = 0.5f * (x + c.tick());
                    aRe += x * pc; aIm -= x * ps; aE += x * x;
                    { const float t = pc * v.rotC - ps * v.rotS; ps = pc * v.rotS + ps * v.rotC; pc = t; }
                    v.lp += v.blkLp * (x - v.lp);
                    v.emphLp += v.emphCoef * (v.lp - v.emphLp);
                    x = (v.lp + v.blkEmph * (v.lp - v.emphLp)) * v.gain * v.level;
                    bl[i] += x * v.gL; br[i] += x * v.gR;
                    const float a = std::fabs(x);
                    env = a > env ? a : env * 0.9995f;
                }
            } else {
                ReedCore& c = *reeds[(size_t)v.slot];
                const float dA = (v.tA - v.curA) * inv, dB = (v.tB - v.curB) * inv;
                for (int i = 0; i < n; ++i) {
                    v.curBreath += dBreath; v.curA += dA; v.curB += dB; v.curNoise += dNoise; v.level += dLevel;
                    c.breath = v.curBreath; c.delayA = v.curA; c.delayB = v.curB; c.noiseAmt = v.curNoise;
                    float x = c.tick();
                    rawEnv += rawCoef * (std::fabs(x) - rawEnv);
                    aRe += x * pc; aIm -= x * ps; aE += x * x;
                    { const float t = pc * v.rotC - ps * v.rotS; ps = pc * v.rotS + ps * v.rotC; pc = t; }
                    v.lp += v.blkLp * (x - v.lp);
                    v.emphLp += v.emphCoef * (v.lp - v.emphLp);
                    x = (v.lp + v.blkEmph * (v.lp - v.emphLp)) * v.gain * v.level;
                    bl[i] += x * v.gL; br[i] += x * v.gR;
                    const float a = std::fabs(x);
                    env = a > env ? a : env * 0.9995f;
                }
            }
            if (!std::isfinite(env)) { v.active = false; env = 0.f; }
            v.env = env; v.rawEnv = rawEnv;
            { const float g = 1.5f - 0.5f * (pc * pc + ps * ps); v.phC = pc * g; v.phS = ps * g; }
            v.accRe += aRe; v.accIm += aIm; v.accE += aE; v.accN += n;
            if (v.accN >= v.winLen) tunerUpdate(v);
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
        case kParamBlowerOn:
            wind.blowerOn = v >= 0.5f;
            if (!windLive) wind.reset();      // restored with a project: start in that state, no run-up or run-down
            break;
        case kParamResLoad: wind.load = std::max(0, std::min(kNumReservoirLoads - 1, (int)v)); break;
        case kParamResSize: wind.sizeSeconds = v; break;
        case kParamSpinDown: wind.spinDown = v; break;
        case kParamWindLeak: wind.leak = v * 0.01f; break;
        case kParamTuning: tuning.system = (int)v; break;
        case kParamEdoN: tuning.customN = (int)v; break;
        case kParamAnchorKey: tuning.anchor = (int)v; break;
        case kParamARef: tuning.aRef = v; break;
        case kParamRoom: room.room = (int)v; room.configure(); break;
        case kParamDistance: room.distance = v; room.configure(); break;
        case kParamMicHeight: room.height = v; room.configure(); break;
        case kParamWidth: room.width = v; room.configure(); break;
        case kParamRoomMix: room.mix = v; room.configure(); break;
        case kParamPreDelay: room.preDelayMs = v; room.configure(); break;
        case kParamMicType: room.mic = (int)v; room.configure(); break;
        case kParamQuality: room.quality = (int)v; room.configure(); break;
        case kParamIrTrim: room.trimDirect = v >= 0.5f; room.configure(); break;
        case kParamIrOrder: room.order = (int)v; room.configure(); break;
        case kParamOutputGain: outGain = std::pow(10.f, v / 20.f); break;
        case kParamLimiter: limiter = v >= 0.5f; break;
        default: break;
        }
    }
    static constexpr float kLimitCeiling = 0.944f;   // -0.5 dBFS

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
    uint16_t recentChannels = 0;
    float idleSeconds = 0.f;
    bool altSpellingCC = false;
    uint32_t orderCounter = 0, ageCounter = 0;
    Tuning tuning;
    std::unique_ptr<Voicing> voicing, pendingVoicing;
    ScalaScale pendingScl; ScalaKbm pendingKbm;
    std::mutex stateMutex;
    int silentKey = 0; float silentTimer = 0.f;
    bool windLive = false;      // the wind has run since the last reset
    float trimCache[kNumRanks][kNumKeys] = {};
    bool trimValid[kNumRanks][kNumKeys] = {};
    std::atomic<bool> voicingDirty{false}, tuningDirty{false}, scalaDirty{false};
    WindSystem wind;
    RoomProcessor room;
    ModMatrix matrix, pendingMatrix;
    std::atomic<bool> matrixDirty{false};
    float ccVal[128]; bool ccSeen[128]; int ccZone[128];
    float globalState[kNumModRows] = {}; bool globalInit[kNumModRows] = {};
    float swellEff = 0.64f;
    Pallet* lastPallet = nullptr;
    uint32_t learnCounter = 0;
    float outGain = 0.5f;
    bool limiter = true;
    float limGain = 1.f, limRelease = 0.001f, prePeak = 0.f, minLimGain = 1.f;
    double cpuAvg = 0;
    float swellGain = 1.f, swellGainT = 1.f, swellCoef = 1.f, swellCoefT = 1.f, swellLpL = 0.f, swellLpR = 0.f;
    float blowerGain = 0.f, blowerNoiseT = 0.f, blowerLp = 0.f;
    Noise blowerNoise;
};

} // namespace pipe
