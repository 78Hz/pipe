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
#include "OrganPipes.hpp"
#include "PipeEdit.hpp"
#include "Air.hpp"
#include "WorkerPool.hpp"
#include <atomic>
#include <cstdlib>
#include <thread>
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
constexpr float kMmToPa = 9.80665f;
// output scale of the physical flue pipes: radiated pressure at 1 m (Pa) to the mix
constexpr float kFlueGain = 1.0f;
// The air the organ was tuned in (its tuning conditions): a church at 18 C, 50 % humidity, sea level.
constexpr double kTunedTempC = 18.0, kTunedHumidity = 50.0, kTunedPressureKPa = 101.325;
constexpr float kReedGain = 1.0f;
// Provisional balance between ranks (2026-11-18). The voicer regulates each rank only against itself; nothing
// set one rank against another, and every reed came out 7 - 28 dB under the principals (measured dry, each
// stop alone at C2 - C5, median of C3 - C5 manuals / C2 - C4 pedal, against the Octave 8' c1). Until the
// ranks are balanced properly, each is lifted to a target: chorus reeds level with the Octave 8', Dulzian and
// Clairons 2 dB under, colour reeds (Vox Humana, Oboe, Trichterregal, pedal Cornet) 6 dB under, Gamba 4 and
// Celeste 6 dB under. A trim of the output only: the pipe's physics and wind are unchanged. (The Octavin and
// the Harmonic Flutes are uneven within the rank and are not trimmed: a rank trim would make their loud
// notes louder still.)
inline float rankBalanceDb(int rank) {
    static float t[kNumRanks];
    static bool init = false;
    if (!init) {
        struct B { const char* stop; float db; };
        static const B b[] = {
            { "i_trumpet_16", 10.9f }, { "i_trumpet_8", 14.2f }, { "i_trompette_8", 14.8f }, { "i_vox_humana_8", 19.4f },
            { "ii_dulzian_16", 16.2f }, { "ii_trichterregal_8", 17.4f },
            { "iii_gamba_8", 3.6f }, { "iii_celeste_8", 4.2f }, { "iii_bassoon_16", 16.1f }, { "iii_trompette_8", 17.6f },
            { "iii_oboe_8", 21.9f }, { "iii_clairon_4", 19.0f },
            { "p_trombone_32", 6.3f }, { "p_posaune_16", 8.1f }, { "p_trumpet_8", 10.5f }, { "p_trompette_8", 7.1f },
            { "p_clairon_4", 12.9f }, { "p_cornet_2", 20.1f } };
        for (float& v : t) v = 0.f;
        for (const B& x : b) for (int s = 0; s < kNumStops; ++s) if (std::strcmp(kStops[(size_t)s].id, x.stop) == 0) t[kStops[(size_t)s].rankFirst] = x.db;
        init = true;
    }
    return rank >= 0 && rank < kNumRanks ? t[rank] : 0.f;
}
// wind of the Octave 8' c1 (flue area x jet speed), the unit of the wind system's demand
constexpr double kRefFlow = 4.5e-4;
// The slider chest (Opus 14 has slider chests throughout, Vogt): under each key one groove (channel), fed
// from the wind chest through that key's pallet, and every drawn stop's pipe on that key standing on it,
// fed through its slider and toe. The pipes share the groove's wind: drawing more stops on a key takes more
// from it, and what one pipe draws the others feel. ASSUMPTIONS (no Opus 14 drawings at hand; typical
// slider-chest sizes, Audsley / Dom Bedos): a groove of 0.8 l (about 800 x 20 x 50 mm); the laminar seal of a
// 10 mm leather face. The pallet as Pykett measured and designs it ("The Calculation of Pallet Sizes", 2001/2009):
// its windway d (l + w) at a descent d of 8 mm (his 6 - 10), sized to the pipes it feeds (sizePallets), and a
// discharge coefficient 0.56: on his rig a pallet passed 0.93 +- 0.14 of the flow of a sharp-edged load aperture
// of the same area (fitted to his Tables 2 - 4, the pallets not self-throttled, w >= 2d), x 0.61 (the toe's).
constexpr double kGrooveVolume = 0.8e-3, kGroovePalletW = 0.150, kGroovePalletLift = 0.008, kGroovePalletCd = 0.61 * 0.925, kGrooveSeal = 0.010;
// A manual groove narrows up the compass as its pipes do: about 3/4 inch at the bottom C, 1/4 inch at the top,
// the widths between them set by the pipes' scale (Dickson, Practical Organ Building, ch. on the sound-board),
// so geometrically; same depth and length. (One 0.8 l groove for every key left a small treble pipe a groove
// three times too big to empty when its pallet shut.) Pedal grooves keep the bass size.
inline double grooveVolume(int division, int key) {
    if (division == kPedal) return kGrooveVolume;
    const double t = std::max(0.0, std::min(1.0, (double)(key - kManualLow) / (double)(kManualHigh - kManualLow)));
    return kGrooveVolume * std::pow(1.0 / 3.0, t);
}
// The passage each pipe's wind climbs from its groove: bored through the table (3/4 in), the slider (3/8 in)
// and the upper board (1 1/4 in), 60 mm (Dickson, Practical Organ Building, ch. III), the bores "as large as the
// grooves will allow" in the bass and small in the treble (ch. IV): here as wide as the key's groove (3/4 in at
// the bottom, 1/4 in at the top), and never narrower than the pipe's own toe hole. ASSUMPTION: Opus 14's own
// board thicknesses are not known; Dickson's are a builder's ordinary ones. What matters is the air in it: at a
// pipe's frequencies its inertance rho (l + 2 x 0.85 r) / (pi r^2) (an end correction at each end) stands far
// above the groove's own impedance, so a pipe neither feels the others' pulses through the groove nor sends its
// own (a reed's boot pulses at its note) into it; steady wind it does not touch. Without it the lumped groove
// passed a Trumpet's boot pulses straight to the open-footed flues on its key, and drew a Principal +-50 c.
constexpr double kPassageLength = 0.060, kGrooveWidthBass = 0.020;
inline double grooveWidth(int division, int key) { return kGrooveWidthBass * grooveVolume(division, key) / kGrooveVolume; }
inline double passageL(int division, int key, double toe, double rho = 1.2) {
    const double r = 0.5 * std::max(grooveWidth(division, key), toe > 0 ? toe : 0.0);
    return rho * (kPassageLength + 1.7 * r) / (M_PI * r * r);
}
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
    bool groove = false;        // stands on its key's groove (slider chest); false: a chest of its own
    float gm[kControlBlock], gt[kControlBlock];   // this block's sound, rendered with its groove
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
    // physical flue pipe: frequency ratio to the pipe as cut, and the pan of its top (the mouth uses gL/gR)
    float pitchRatio = 1.f, curPitch = 0.f, tL = 0.f, tR = 0.f;
};

class Engine {
public:
    Engine() {
        sink.e = this;
        flues.reserve(kMaxFlueVoices);
        for (int i = 0; i < kMaxFlueVoices; ++i) flues.emplace_back(new FluePipeRT());
        reeds.reserve(kMaxReedVoices);
        for (int i = 0; i < kMaxReedVoices; ++i) reeds.emplace_back(new ReedPipeRT());
        voices.resize(kMaxFlueVoices + kMaxReedVoices);
        {   // the pipes' worker threads: one fewer than the machine's cores (the host's audio thread is the other), at most 7
            int w = (int)std::thread::hardware_concurrency() - 1;
            if (const char* e = std::getenv("OPUSXIV_THREADS")) w = std::atoi(e) - 1;
            pool.start(std::max(0, std::min(7, w)));
        }
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
        outputs[kOutReservoir - kOutFirst] = 79.f;
    }

    void prepare(double sampleRate) {
        fs = (float)sampleRate;
        limRelease = 1.f - std::exp(-1.f / (0.08f * fs));   // 80 ms
        limGain = 1.f;
        room.prepare(sampleRate);
        applyAllParams();
        wind.reset();
        sizePallets();
        windLive = false;
        for (auto& v : voices) v.active = false;
        for (int d = 0; d < kNumDivisions; ++d) for (int k = 0; k < kNumKeys; ++k) pallets[d][k] = Pallet();
        std::memset(trimValid, 0, sizeof(trimValid));
        // every note's breath of air is drawn from its rank, key and the count of notes since the engine was
        // prepared: a prepared engine plays the same notes the same way (the pipe test and the voicer hear the
        // same pipe; before, the count ran on from whatever had been played before)
        ageCounter = 0; orderCounter = 0;
        // ... and from the start of a control block, with every smoothed value at its target: a note struck after
        // a reset falls on the same sample of the block, so it plays exactly as on a new engine (before, the
        // block's phase ran on, and the same pipe spoke in 29 or 34 periods depending on what came before)
        blockPos = 0; idleSeconds = 0.f; recentChannels = 0; silentTimer = 0.f;
        for (int i = 0; i < kNumModRows; ++i) { globalState[i] = 0.f; globalInit[i] = false; }
        swellGain = swellGainT = 1.f; swellCoef = swellCoefT = 1.f;   // as a new engine: the box follows its pedal from there
        blowerLp = 0.f; blowerGain = 0.f; blowerNoise = Noise();
        prePeak = 0.f; minLimGain = 1.f;
        // the grooves empty, as on a new organ (a groove held the last note's wind into the next test)
        std::memset(grooveP, 0, sizeof grooveP); std::memset(grooveMean, 0, sizeof grooveMean); std::memset(grooveAp, 0, sizeof grooveAp);
        std::memset(grooveFlow, 0, sizeof grooveFlow); std::memset(divInflow, 0, sizeof divInflow); std::memset(grooveStep, 0, sizeof grooveStep);
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
    // Pipes edited in the Workshop (PipeEdit.hpp): rebuilt here, off the audio thread, then handed over.
    // The voicer's hook (tools/engine_voicer.cpp): play these pipes in place of the organ's, at once.
    // Call between notes, from the thread that renders. The tuner forgets what it learnt.
    // The pipe test's and the voicer's hook: only this rank of a drawn stop sounds (a mixture's rank on its
    // own); -1 = all.
    void setSoloRank(int r) { soloRank = r; }
    void setVoicingOverride(std::unique_ptr<PipeEditSet> set) {
        pipeEdits = std::move(set); pendingEdits.reset(); editsDirty = false;
        std::memset(trimValid, 0, sizeof(trimValid));
    }
    void setPipeEditsText(const std::string& text) {
        std::unique_ptr<PipeEditSet> set = buildPipeEdits(text);
        std::lock_guard<std::mutex> lk(stateMutex);
        pendingEdits = std::move(set);   // a set handed back by the audio thread (the previous one) is freed here
        editsDirty = true;
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
    double debugGroovePa(int division, int key) const { return grooveP[division][key]; }
    double debugGrooveVolume(int division, int key) const { return grooveVolume(division, key); }
    // the wind a sounding pipe drew over the last block (m^3/s)
    double debugPipeInflow(int rank, int key) const {
        for (const auto& v : voices) if (v.active && v.rank == rank && v.key == key) return v.reed ? reeds[(size_t)v.slot]->inflow() : flues[(size_t)v.slot]->inflow();
        return 0.0;
    }
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

    struct ActiveNote { int ch, key; int div[kNumDivisions]; int nDiv; };
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
        ActiveNote an{ ch, key, { -1, -1, -1, -1 }, 0 };
        const float v = vel / 127.f;
        auto press = [&](int d, bool coupled) {
            if (pressPallet(d, key, kp, ch, zone, v, coupled)) an.div[an.nDiv++] = d;
        };
        press(div, false);
        // couplers: the key also opens the pallet of the coupled division (one level, as the
        // mechanical couplers of Opus 14 do not pass on what another coupler has added)
        for (int s = 0; s < kNumStops; ++s)
            if (kStops[s].type == kStopCoupler && kStops[s].division == div && stopOn(s)) press(kStops[s].couplerFrom, true);
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
        // the rank's pipe for the pitch the key plays (in a 41-EDO octave, key 100 plays a pipe of the compass)
        const int kk = std::max(0, std::min(127, (int)std::lround(p.pitch.keyFloat)));
        (void)key;
        double f = p.pitch.hz * rankRatio(rank, kk);
        // tuning offsets: rank, pipe, celeste, age, temperature
        double cents = voicing->pipeTune[rank][p.pitch.pipeIndex] + rd.detuneCents;
        const float drift = params[kParamAgeDrift];
        if (drift > 0) cents += drift * ((hash32((uint32_t)(rank * 4096 + p.pitch.pipeIndex)) & 0xFFFF) / 32767.5f - 1.f);
        // (the air's temperature, humidity and pressure act on the pipes themselves: Air.hpp, updateAir)
        return f * std::pow(2.0, cents / 1200.0);
    }

    // The smallest pipes are only a few samples long at the host's rate, too coarse
    // for the jet model; they run at twice the rate.
    float modelRate(const Voice& v) const { return v.twice ? 2.f * fs : fs; }

    // Resonator lengths for the voice's frequency.
    void tuneVoice(Voice& v) {
        const double f = v.freq;
        // Physical pipes are cut and tuned offline; this tuner finishes the job once, as a tuner on a
        // ladder would: it listens only while the pipe stands in its voiced state (pallet open, chest at
        // its pressure, no bend) and remembers the correction for that pipe (trimCache).
        // short windows catch a large error, long ones settle to a fraction of a cent
        v.winMin = std::max(64, (int)(6.0 * fs / f));
        v.winMax = std::max((int)(0.020f * fs), (int)(24.0 * fs / f));
        v.winLen = v.winMin;
        v.levelLag = 1.f - std::exp(-(kControlBlock / fs) / std::max(0.003f, 3.f / (float)f));
        // until a pipe has been heard once, the usual residual of its model stands in
        v.trim = trimValid[v.rank][v.key] ? trimCache[v.rank][v.key] : 0.f;
        v.prevOk = false; v.accRe = v.accIm = v.accE = 0; v.accN = 0;
    }

    void startVoice(int rank, int d, int key) {
        if (soloRank >= 0 && rank != soloRank) return;
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
        const float reg = std::pow(10.f, (voicing->rank[rank].key[kVRegulation][k] + rankBalanceDb(rank)) / 20.f);
        const uint32_t seed = hash32((uint32_t)(rank * 131071 + key * 31 + d));

        if (!isReed) {
            // An Opus 14 flue pipe, as built from the Pasi scales and cut to pitch (OrganPipes.hpp).
            const FluePipeSpec* sp = pipeEdits ? pipeEdits->flueAt(rank, k) : nullptr;
            if (!sp) sp = organFlueSpec(rank, k);
            if (!sp) { v.active = false; return; }
            v.family = rd.family;
            FluePipeRT& core = *flues[(size_t)v.slot];
            core.init(*sp, fs, seed); core.setAir(pipeAir);
            // a pipe on an offset chest of its own (sp->valve) keeps its own valve; the others stand on the groove
            v.groove = sp->valve <= 0.f;
#ifdef PIPE_NO_GROOVE
            v.groove = false;   // diagnostics: every pipe on a chest of its own, as the voicer voiced it
#endif
            core.setGrooveFed(v.groove);
            core.setPassage(v.groove ? passageL(rd.division, k, sp->toe) : 0.0);
            core.setPallet(0.002, 0.002);   // its valve (the slider and toe, or its own) follows in 2 ms
            core.setChestPa(wind.chest[d] * kMmToPa);
            v.pitchRatio = (float)(f / sp->target);
            tuneVoice(v);
            v.curPitch = 1e9f;   // pitch set at the first control block (with the tuner's trim)
            core.setPitch(v.pitchRatio * std::pow(2.f, v.trim / 1200.f));
            v.gain = kFlueGain * reg;
            // wind used, relative to the Octave 8' c1 (flue area x jet speed at the voiced pressure)
            v.flowWeight = (float)(sp->h * sp->Wm * std::sqrt(2.0 * sp->footPa / 1.2) / kRefFlow);
            v.voicedRatio = 1.f;
            v.speakMin = 0.2f;
            v.bendLimit = rd.kind == kKindStopped ? params[kParamBendStopped]
                        : rd.kind == kKindString ? params[kParamBendString] : params[kParamBendOpen];
            v.twice = false;
        } else {
            // An Opus 14 reed pipe: Pasi resonator and tongue, tuned by wire and scroll (OrganPipes.hpp).
            const ReedPipeSpec* sp = pipeEdits ? pipeEdits->reedAt(rank, k) : nullptr;
            if (!sp) sp = organReedSpec(rank, k);
            if (!sp) { v.active = false; return; }
            v.family = rd.family;
            ReedPipeRT& core = *reeds[(size_t)v.slot];
            core.init(*sp, fs, seed); core.setAir(pipeAir);
            v.groove = true;
#ifdef PIPE_NO_GROOVE
            v.groove = false;
#endif
            // its toe hole, as the voicer bored it (0: an open boot)
            core.setToe(sp->toe);
            core.setPassage(v.groove ? passageL(rd.division, k, sp->toe) : 0.0);
            core.setPallet(0.002, 0.002);
            core.setChestPa(wind.chest[d] * kMmToPa);
            v.pitchRatio = (float)(f / sp->target);
            tuneVoice(v);
            v.curPitch = 1e9f;
            core.setPitch(v.pitchRatio * std::pow(2.f, v.trim / 1200.f));
            v.gain = kReedGain * reg;
            v.flowWeight = (float)(0.5 * sp->w * sp->x0 * std::sqrt(2.0 * sp->footPa / 1.2) / kRefFlow);
            v.voicedRatio = 1.f;
            v.bendLimit = params[kParamBendReed];
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
        // the top of a flue pipe is a second source, higher and a little to the side of its mouth
        const float panTop = std::max(-1.f, std::min(1.f, pan + (side > 0 ? 0.12f : -0.12f)));
        v.tL = std::cos((panTop + 1.f) * (float)M_PI * 0.25f);
        v.tR = std::sin((panTop + 1.f) * (float)M_PI * 0.25f);
    }

    // The pipe models are self-oscillating loops whose pitch is pulled a few
    // cents by the exciter, by the interpolated delays and by voicing. Each
    // voice therefore measures its own fundamental against the frequency it is
    // meant to sound (phase drift of a quadrature detector running at the
    // target) and trims its resonator until the two agree. The trim is
    // remembered per pipe, so a pipe that has spoken once starts in tune.
    // The grooves of the slider chests, sample by sample with the pipes standing on them: each sample every
    // pipe on a groove speaks from the groove's pressure, then the groove is solved (implicitly, with the
    // pipes' own flow and conductance from that sample) for its pallet's inflow against what they took.
    // The pipes, the grooves and the chests, one control block (n samples). Each groove (a key's channel in a
    // division's slider chest) is solved sample by sample with the pipes standing on it: its pallet lets wind
    // in from the chest, the pipes take it out, and each pipe hears its groove's pressure. The grooves are
    // independent of one another within a block (they meet only in the chest), so they run in parallel on
    // the worker threads, with the pipes that stand on chests of their own. Each groove sees its chest's
    // pressure one block late (0.67 ms at 48 kHz): the chest is a box 2.4 m long, and a pressure wave takes
    // 7 ms to cross it, so a lumped chest that answered every groove at once would be the less faithful.
    // Then each division's chest, trunk, regulator and tremulant take the grooves' flow sample by sample.
    void renderGrooves(int n) {
        for (int d = 0; d < kNumDivisions; ++d) grooveFlow[d] = 0.0;
        int nG = 0;
        for (int i = 0; i < (int)voices.size(); ++i) {
            Voice& v = voices[(size_t)i];
            if (!v.active || !v.groove) continue;
            const int g = v.division * 128 + v.key;
            int j = 0; while (j < nG && gId[j] != g) ++j;
            if (j == nG) { if (nG >= kMaxGrooves) { v.groove = false; continue; } gId[nG] = g; gN[nG] = 0; ++nG; }
            if (gN[j] < kMaxPerGroove) gV[j][gN[j]++] = i; else v.groove = false;
        }
        int nO = 0;
        for (int i = 0; i < (int)voices.size(); ++i) if (voices[(size_t)i].active && !voices[(size_t)i].groove) ownV[nO++] = i;
        const double rho = 1.2041 * pipeAir.rhoRatio, c = 343.21 * pipeAir.cRatio, mu = 1.807e-5 * pipeAir.muRatio;
        const double dt = 1.0 / fs, kUnit = dt * rho * c * c;   // / the groove's volume
        const float boostSpan = (params[kParamPressMax] - 78.f) / 78.f;
        wind.trem.unitFlowM3 = kTrunkUnitFlow * kRefFlow;

        auto task = [&](int idx) {
            if (idx >= nG) {   // a pipe on a chest of its own
                Voice& v = voices[(size_t)ownV[idx - nG]];
                if (v.reed) reeds[(size_t)v.slot]->render(v.gm, n);
                else flues[(size_t)v.slot]->render(v.gm, v.gt, n);
                return;
            }
            const int j = idx, d = gId[j] / 128, key = gId[j] % 128;
            const Pallet& pl = pallets[d][key];
            const double ap0 = grooveAp[d][key], ap1 = pl.aperture, boostK = std::max(0.f, 1.f + pl.boost * boostSpan);
            const double W = grooveW[d][key], k = kUnit / grooveVolume(d, key);
            const int nPrev = pcPrevN[d];
            double P = grooveP[d][key], sum = 0, lastStep = grooveStep[d][key];
            for (int t = 0; t < n; ++t) {
                const double chestPa = (nPrev > 0 ? pcPrev[d][std::min(t, nPrev - 1)] : wind.div[d].Pc) * boostK;
                const double ap = ap0 + (ap1 - ap0) * (t + 1) / n;   // the pallet's travel across the block
                // The groove and the feet of its pipes solved together: at each trial pressure of the groove every
                // pipe previews what it would draw over this sample from its state now (FluePipeRT::previewInflow),
                // then each pipe draws at the pressure found, so the wind the groove gives is the wind the pipes take.
                // (Rendered first at the old pressure, the pipes took one flow and the groove gave another, the
                // difference air from nowhere: a groove under a pipe with a large foot fell at a third of the rate
                // its pipe drew it down. A straight line through the toe's sqrt law misses it too, the foot all but
                // at the groove's pressure.)
                auto pipesAt = [&](double Pg, double& gSum) {
                    double q = 0; gSum = 0;
                    for (int m = 0; m < gN[j]; ++m) {
                        const Voice& v = voices[(size_t)gV[j][m]]; double gg = 0;
                        q += v.reed ? reeds[(size_t)v.slot]->previewInflow(Pg, gg) : flues[(size_t)v.slot]->previewInflow(Pg, gg);
                        gSum += gg;
                    }
                    return q;
                };
                // the pallet: an orifice (Bernoulli) in series with the laminar seal, both ways
                // the lift under the key's travel: as a pipe's own pallet (FluePipeRT::wind), slow off its seat and
                // into its stop, so the first part of the travel throttles the wind
                const double gLift = kGroovePalletLift * (0.5 - 0.5 * std::cos(M_PI * std::max(0.0, std::min(1.0, ap))));
                const double Ap = kGroovePalletCd * W * std::max(gLift, 1e-12), aa = rho / (2 * Ap * Ap);
                const double bb = gLift > 1e-9 ? 12.0 * mu * kGrooveSeal / (W * gLift * gLift * gLift) : 0.0;
                auto palletAt = [&](double Pg, double& dq) {
                    if (gLift <= 1e-9) { dq = 0; return 0.0; }
                    const double dP = chestPa - Pg, r = std::sqrt(bb * bb + 4 * aa * std::fabs(dP));
                    dq = 1.0 / r; return (dP >= 0 ? 1.0 : -1.0) * (-bb + r) / (2 * aa);
                };
                // F(Pn) = Pn - P - k (pallet(Pn) - pipes(Pn)) rises with Pn and has one root, between no wind and
                // the most there is (the chest, or the groove itself): Newton kept inside that bracket, halving
                // it when a step would leave it (unguarded, a step through the toe's sqrt law could throw the groove
                // to many times its chest)
                // started where the last step would carry it, and stepped along the secant of the last two trials once
                // there are two (the pipes' own slopes are not exactly the slope of what an oversampled pipe draws
                // over the sample, and Newton on them took three or four trials a sample)
                double lo = 0.0, hi = std::max(P, chestPa) + 1.0, Pn = std::min(std::max(P + lastStep, lo + 1e-9), hi - 1e-9), q = 0;
                double prevX = 0, prevF = 0; bool havePrev = false;
                for (int it = 0; it < 12; ++it) {
                    double dq, gp; const double qq = palletAt(Pn, dq), qp = pipesAt(Pn, gp);
                    const double F = Pn - P - k * (qq - qp);
                    double dF = 1.0 + k * (dq + gp);
                    if (havePrev && Pn != prevX) { const double sec = (F - prevF) / (Pn - prevX); if (sec > 0) dF = sec; }
                    prevX = Pn; prevF = F; havePrev = true;
                    if (F > 0) hi = Pn; else lo = Pn;
                    double nx = Pn - F / dF;
                    if (!(nx > lo && nx < hi)) nx = 0.5 * (lo + hi);
                    const bool done = std::fabs(nx - Pn) < 1e-3;   // a thousandth of a pascal: the air the groove and its pipes disagree on
                    Pn = nx;
                    if (done) break;
                }
                { double dq; q = palletAt(Pn, dq); }
                lastStep = std::max(0.0, Pn) - P;
                P = std::max(0.0, Pn); sum += P;
                qG[j][t] = q;
                for (int m = 0; m < gN[j]; ++m) {
                    Voice& v = voices[(size_t)gV[j][m]];
                    if (v.reed) { ReedPipeRT& r = *reeds[(size_t)v.slot]; r.setChestPa(P); r.render(&v.gm[t], 1); }
                    else { FluePipeRT& f = *flues[(size_t)v.slot]; f.setChestPa(P); f.render(&v.gm[t], &v.gt[t], 1); }
#ifdef PIPE_VOICE_DEBUG
                    PIPE_VOICE_DEBUG(d, v, m);
#endif
                }
#ifdef PIPE_GROOVE_DEBUG
                PIPE_GROOVE_DEBUG(d, key, t, chestPa, Pn, q, 0.0, 0.0, j);
#endif
            }
            grooveP[d][key] = P; grooveStep[d][key] = lastStep; grooveMean[d][key] = sum / n; grooveAp[d][key] = ap1;
        };
        if (nG + nO >= kParallelMin) pool.run(nG + nO, task);
        else for (int i = 0; i < nG + nO; ++i) task(i);

        // the chests: what the pallets took, sample by sample; the wind lines refill them from the bellows, and the
        // bellows feel the lines sample by sample
        int gsd[kNumDivisions][kMaxGrooves], ngd[kNumDivisions] = {};
        double qBlockD[kNumDivisions] = {};
        for (int j = 0; j < nG; ++j) { const int d = gId[j] / 128; gsd[d][ngd[d]++] = j; }
        for (int d = 0; d < kNumDivisions; ++d) if (d == wind.tremTarget) { wind.trem.rate = wind.tremSpeed; wind.trem.depth = wind.tremDepth; }
        for (int t = 0; t < n; ++t) {
            // the bellows first, against what each line will draw at their pressure; then the lines at that pressure
            double G = 0, J = 0;
            for (int d = 0; d < kNumDivisions; ++d) { double g, j; wind.div[d].bellowsNorton(dt, wind.airPa, rho, c, g, j); G += g; J += j; }
            wind.stepBellows(dt, G, J);
            const double Pres = wind.airPa;
            for (int d = 0; d < kNumDivisions; ++d) {
                DivisionWind& dw = wind.div[d];
                const Tremulant* trem = d == wind.tremTarget ? &wind.trem : nullptr;
                double qOut = 0;
                for (int u = 0; u < ngd[d]; ++u) qOut += qG[gsd[d][u]][t];
                dw.step(dt, Pres, qOut, rho, c, trem, wind.tremOn);
                pcPrev[d][t] = dw.Pc;
                qBlockD[d] += qOut;
            }
        }
        for (int d = 0; d < kNumDivisions; ++d) {
            DivisionWind& dw = wind.div[d];
            const double qBlock = qBlockD[d];
            pcPrevN[d] = n;
            grooveFlow[d] = qBlock / n;
            divInflow[d] = dw.takeInflow();
            wind.chest[d] = (float)(dw.pcMean / kMmToPa);   // its mean over the block (the pipes' ripple is in it sample by sample)
        }
        // a groove with no pipe on it: the pallet fills it or it empties (no pipe to hear it)
        for (int d = 0; d < kNumDivisions; ++d)
            for (int key = 0; key < 128; ++key) {
                bool used = false; for (int j = 0; j < nG && !used; ++j) used = gId[j] == d * 128 + key;
                if (!used) { grooveP[d][key] = grooveMean[d][key] = pallets[d][key].aperture > 0.f ? wind.div[d].Pc : 0.0; grooveAp[d][key] = pallets[d][key].aperture; }
            }
    }
    // Each key's pallet sized to the pipes it feeds, by Pykett's rule ("The Calculation of Pallet Sizes"): on his rig a
    // pallet feeds a load aperture of only about a third of its geometric windway at a 10 % drop across it, the most
    // he allows with every stop drawn, so its windway is made 3.5 x the load. His load apertures were sharp-edged
    // slots (discharge 0.61); he estimates a note's load by the smaller of each pipe's toe hole and flue area, but a
    // flue passes its wind as a nozzle (the jet's speed from the whole foot pressure, the toe in series:
    // Af / sqrt(1 + (Af / (0.61 At))^2) of free area, within a few % of what each pipe here draws), half as much
    // again as a slot of its area. So each pipe counts as the slot that passes its wind: that free area / 0.61.
    // (A reed: its toe hole, a slot, or what an open boot's tongue lets through; "the total wind demand is usually
    // dominated by the large flues".) At least 50 mm (a treble pallet, Dickson's smallest groove with its pallet's
    // length: ASSUMPTION, it only keeps the smallest pallets from vanishing); above his largest (300 x 22 mm) as
    // many pallets as the load needs ("more than one pallet was used if necessary"; a pedal bass under a 32' takes
    // several).
    static constexpr double kPalletLoadFactor = 3.5, kPalletWMin = 0.05, kSlotCd = 0.61;
    static double pipeLoad(const FluePipeSpec& f) {
        const double flue = (double)f.h * f.Wm;
        double free = flue;
        if (f.toe > 0) { const double r = flue / (kSlotCd * M_PI * 0.25 * f.toe * f.toe); free = flue / std::sqrt(1 + r * r); }
        return free / kSlotCd;
    }
    void sizePallets() {
        for (int d = 0; d < kNumDivisions; ++d)
            for (int key = 0; key < 128; ++key) {
                double a = 0;
                for (int r = 0; r < kNumRanks; ++r) {
                    if (kRanks[r].division != d) continue;
                    if (const FluePipeSpec* f = organFlueSpec(r, key)) a += pipeLoad(*f);
                    else if (const ReedPipeSpec* q = organReedSpec(r, key)) a += q->toe > 0 ? M_PI * 0.25 * q->toe * q->toe : 3.0 * (double)q->w * q->x0;
                }
                grooveLoad[d][key] = a;
                grooveW[d][key] = std::max(kPalletWMin, kPalletLoadFactor * a / kGroovePalletLift);
            }
    }
    static constexpr int kMaxGrooves = 128, kMaxPerGroove = 64;
    int gId[kMaxGrooves], gN[kMaxGrooves], gV[kMaxGrooves][kMaxPerGroove];
    double grooveStep[kNumDivisions][128] = {};   // the groove's last step (Pa per sample): where its next solve starts
    double grooveP[kNumDivisions][128] = {}, grooveMean[kNumDivisions][128] = {}, grooveAp[kNumDivisions][128] = {}, grooveFlow[kNumDivisions] = {}, divInflow[kNumDivisions] = {};
    double grooveW[kNumDivisions][128];   // each key's pallet windway / descent, l + w (sizePallets)
    double grooveLoad[kNumDivisions][128] = {};   // each key's load aperture (m^2, sizePallets)
    double qG[kMaxGrooves][kControlBlock];   // each groove's pallet flow, sample by sample, this block
    double pcPrev[kNumDivisions][kControlBlock] = {}; int pcPrevN[kNumDivisions] = {};   // each chest's pressure, last block
    int ownV[kMaxFlueVoices + kMaxReedVoices];   // pipes on chests of their own, this block
    static constexpr int kParallelMin = 6;   // fewer tasks than this: not worth waking the workers
    WorkerPool pool;

    void tunerUpdate(Voice& v) {
        const double re = v.accRe, im = v.accIm, e = v.accE; const int n = v.accN;
        v.accRe = v.accIm = v.accE = 0; v.accN = 0;
        const double mag2 = re * re + im * im;
        const bool ok = e / n > 1e-12 && 2.0 * mag2 > 0.1 * (double)n * e && v.tgtW > 0.f;
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
        return q == 0 ? 96 : q == 1 ? 192 : 320;   // physical flue pipes: ~0.3 % of a core each
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
    // The air in the organ (Air.hpp), from the room's temperature, humidity and barometric pressure: every
    // pipe speaks with it. The organ was tuned in the air of its tuning conditions (kTunedAir); the tuner holds
    // a pipe to its tuning there, so what the air does to the pipes is heard.
    void updateAir() {
        const double t = params[kParamPipeTemp], h = params[kParamHumidity], pk = params[kParamAirPressure];
        if (t == airT && h == airH && pk == airP) return;
        airT = t; airH = h; airP = pk;
        air = AirState::make(t, h, pk);
        pipeAir = PipeAir::of(air);
        const AirState tuned = AirState::make(kTunedTempC, kTunedHumidity, kTunedPressureKPa);
        cTunedRatio = air.c / tuned.c;
        airAtTuning = std::fabs(cTunedRatio - 1.0) < 2e-5 && std::fabs(air.rho / tuned.rho - 1.0) < 2e-4;
        for (auto& v : voices) {
            if (!v.active) continue;
            if (v.reed) reeds[(size_t)v.slot]->setAir(pipeAir); else flues[(size_t)v.slot]->setAir(pipeAir);
        }
    }
    void controlUpdate() {
        updateAir();
        // pick up state from the host/UI thread without blocking
        if ((voicingDirty || tuningDirty || scalaDirty || matrixDirty || editsDirty) && stateMutex.try_lock()) {
            if (voicingDirty) { std::memcpy(voicing->rank, pendingVoicing->rank, sizeof(voicing->rank)); voicingDirty = false; }
            if (tuningDirty) { std::memcpy(voicing->pipeTune, pendingVoicing->pipeTune, sizeof(voicing->pipeTune)); tuningDirty = false; }
            if (scalaDirty) { std::swap(tuning.scl, pendingScl); std::swap(tuning.kbm, pendingKbm); scalaDirty = false; }
            if (matrixDirty) { matrix = pendingMatrix; matrixDirty = false; }
            if (editsDirty) {
                std::swap(pipeEdits, pendingEdits); editsDirty = false;
                // an edited pipe sounds at a new pitch: the tuner listens to every pipe again
                std::memset(trimValid, 0, sizeof(trimValid));
            }
            stateMutex.unlock();
        }
        const float dt = kControlBlock / fs;
        if (silentTimer > 0.f) silentTimer -= dt;
        outputs[kOutSilentKey - kOutFirst] = silentTimer > 0.f ? (float)silentKey : 0.f;

        // wind
        float flow = 0.f, divFlow[kNumDivisions] = {}; int speaking = 0;
        for (const auto& v : voices) {
            if (!v.active) continue;
            ++speaking;
            const Pallet& p = pallets[v.division][v.key];
            if (!v.groove && !v.stopClosed) {   // a pipe on its own chest: what it actually drew
                const double q = v.reed ? reeds[(size_t)v.slot]->inflow() : flues[(size_t)v.slot]->inflow();
                const float f = (float)(q / kRefFlow); flow += f; divFlow[v.division] += f;
            }
            (void)p;
        }
        // the grooves: what their pallets actually let in (renderGrooves, the last block)
        // the regulators: what they drew from the main reservoir (renderGrooves, the last block)
        wind.directM3 = (double)flow * kRefFlow;
        for (int d = 0; d < kNumDivisions; ++d) { const float f = (float)(divInflow[d] / kRefFlow); flow += f; divFlow[d] += f; }
        wind.update(dt, flow, divFlow);
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
        // which stop speaks on each groove (-1 none, -2 more than one): a tuner draws one stop at a time, so it
        // tunes a pipe only when its own stop has the groove (a mixture's ranks together, as it tunes them)
        int16_t grooveStop[kNumDivisions][kNumKeys];
        for (auto& row : grooveStop) for (auto& x : row) x = -1;
        for (const auto& v : voices) if (v.active && v.groove && !v.stopClosed) {
            int16_t& s = grooveStop[v.division][v.key]; const int16_t mine = (int16_t)stopForRank(v.rank);
            s = (s == -1 || s == mine) ? mine : (int16_t)-2;
        }
        for (auto& v : voices) {
            if (!v.active) continue;
            const Pallet& p = pallets[v.division][v.key];
            {
                // Physical pipe: the wind in its chest (with the expression boost), the pallet
                // opening, and its pitch (tuning x bend) go to the pipe itself; it answers them.
                const float chestMm = wind.chest[v.division] * std::max(0.f, 1.f + p.boost * boostSpan);
                // on the groove the key's pallet is the groove's (renderGrooves): the pipe's own valve is its slider
                const float pal = v.stopClosed ? 0.f : (v.groove ? 1.f : p.aperture);
                float bend = p.bendCents;
                if (strict) bend = std::max(-v.bendLimit, std::min(v.bendLimit, bend));
                // the tuner listens only in the voiced state, and only to a pipe that has settled
                // ... and with only its own stop on its groove, as on the tuner's ladder: other stops drawn on the key
                // share its wind and sag it (the tuner had "corrected" a full chord's bass for that sag)
                const bool alone = !v.groove || grooveStop[v.division][v.key] == (int16_t)stopForRank(v.rank);
                const bool voiced = !v.stopClosed && p.aperture >= 0.999f && std::fabs(bend) < 0.5f && alone
                                    && std::fabs(chestMm - kChestNominal[v.division]) < 0.5f && p.sinceOn * v.freq > 60.f;
                if ((voiced && (!v.reed || airAtTuning)) != v.tunable) { v.tunable = voiced && (!v.reed || airAtTuning); v.prevOk = false; v.winLen = v.winMin; }
                // in tune at the tuning conditions: a flue's pitch follows the speed of sound (its body is fixed), so
                // its target moves with it; a reed's does not follow simply, so it is only tuned in that air
                const bool airOk = !v.reed || airAtTuning;
                v.cacheable = voiced && airOk;
                if (!airOk && v.tunable) { v.tunable = false; v.prevOk = false; }
                v.tgtW = 2.f * (float)M_PI * v.freq * std::pow(2.f, bend / 1200.f) * (float)(v.reed ? 1.0 : cTunedRatio) / fs;
                v.rotC = std::cos(v.tgtW); v.rotS = std::sin(v.tgtW);
                const float cents = bend + v.trim;
                const bool newPitch = cents != v.curPitch;
                v.curPitch = cents;
                const double ratio = v.pitchRatio * std::pow(2.f, cents / 1200.f);
                if (!v.reed) {
                    FluePipeRT& c = *flues[(size_t)v.slot];
                    if (!v.groove) c.setChestPa(chestMm * kMmToPa);
                    c.setPalletTarget(pal);
                    if (newPitch) c.setPitch(ratio);
                } else {
                    ReedPipeRT& c = *reeds[(size_t)v.slot];
                    c.setPalletTarget(pal);
                    if (newPitch) c.setPitch(ratio);
                }
                const bool shut = v.stopClosed || (p.holds == 0 && p.aperture <= 0.f);
                if (shut && v.env < 2e-5f) { if (++v.quiet > 8) v.active = false; }
                else v.quiet = 0;
                continue;
            }
            // foot pressure relative to the pressure the pipe was voiced on
            const float ratio = v.stopClosed ? 0.f : std::max(0.f, p.footRatio);
            float bend = p.bendCents;
            if (strict) bend = std::max(-v.bendLimit, std::min(v.bendLimit, bend));
            // How a pipe answers its wind. More wind than it was voiced for: louder and, for a
            // flue, a few cents sharp. Less: quieter, flatter, duller and breathier, and near
            // its own threshold it wavers, then stops. A flue falls towards a semitone flat
            // just before it stops (Pykett: "towards a semitone (around 6% in frequency) before the sound
            // ceases altogether"; the physical flues: FluePipeSpec::speak); a reed
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
                // the wind under the pipes' toes: the groove's pressure (the key's pallet, the stops drawn on it)
                const int kk = (int)((&p - &pallets[0][0]) % kNumKeys);
                o[2] = grooveMean[d][kk] > 0.0 ? (float)(grooveMean[d][kk] / kMmToPa) : p.footRatio * kChestNominal[d];
                o[3] = p.bendCents;
            } else { o[0] = -1.f; o[1] = o[2] = o[3] = 0.f; }
        }

        outputs[kOutReservoir - kOutFirst] = wind.reservoir * (1.f - wind.sag);
        outputs[kOutChestPd - kOutFirst] = wind.chest[kPedal];
        outputs[kOutChestGt - kOutFirst] = wind.chest[kGreat];
        outputs[kOutChestSw - kOutFirst] = wind.chest[kSwell];
        outputs[kOutChestPos - kOutFirst] = wind.chest[kPositive];
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
        renderGrooves(n);
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
                FluePipeRT& c = *flues[(size_t)v.slot];
                const float* m = v.gm; const float* t = v.gt;   // rendered in renderGrooves (with its groove, or on its own chest)
                const float g = v.gain;
                for (int i = 0; i < n; ++i) {
                    const float xl = (m[i] * v.gL + t[i] * v.tL) * g, xr = (m[i] * v.gR + t[i] * v.tR) * g;
                    bl[i] += xl; br[i] += xr;
                    const float a = std::max(std::fabs(xl), std::fabs(xr));
                    env = a > env ? a : env * 0.9995f;
                    const float x = m[i] + t[i];
                    aRe += x * pc; aIm -= x * ps; aE += x * x;
                    { const float tt = pc * v.rotC - ps * v.rotS; ps = pc * v.rotS + ps * v.rotC; pc = tt; }
                }
                if (!std::isfinite(env)) { v.active = false; env = 0.f; }
                v.env = env;
                { const float gg = 1.5f - 0.5f * (pc * pc + ps * ps); v.phC = pc * gg; v.phS = ps * gg; }
                v.accRe += aRe; v.accIm += aIm; v.accE += aE; v.accN += n;
                if (v.accN >= v.winLen) tunerUpdate(v);
                continue;
            } else {
                ReedPipeRT& c = *reeds[(size_t)v.slot];
                const float* o = v.gm;
                const float g = v.gain;
                for (int i = 0; i < n; ++i) {
                    const float x = o[i] * g;
                    bl[i] += x * v.gL; br[i] += x * v.gR;
                    const float a = std::fabs(x);
                    env = a > env ? a : env * 0.9995f;
                    aRe += o[i] * pc; aIm -= o[i] * ps; aE += o[i] * o[i];
                    { const float tt = pc * v.rotC - ps * v.rotS; ps = pc * v.rotS + ps * v.rotC; pc = tt; }
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
            L[i] = busL[kPedal][i] + busL[kGreat][i] + busL[kPositive][i] + swellLpL * swellGain + hum;
            R[i] = busR[kPedal][i] + busR[kGreat][i] + busR[kPositive][i] + swellLpR * swellGain + hum;
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
        case kParamTremTarget: wind.tremTarget = std::max(0, std::min(kNumDivisions - 1, (int)v)); break;
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
        case kParamTremKind: wind.trem.kind = std::max(0, std::min(kNumTremKinds - 1, (int)v)); break;
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
    std::vector<std::unique_ptr<FluePipeRT>> flues;   // Opus 14 flue pipes (physical model, FlueRT.hpp)
    std::vector<std::unique_ptr<ReedPipeRT>> reeds;   // Opus 14 reed pipes (physical model, ReedModel.hpp)
    std::vector<Voice> voices;
    Pallet pallets[kNumDivisions][kNumKeys];
    uint16_t recentChannels = 0;
    float idleSeconds = 0.f;
    bool altSpellingCC = false;
    uint32_t orderCounter = 0, ageCounter = 0;
    Tuning tuning;
    std::unique_ptr<Voicing> voicing, pendingVoicing;
    std::unique_ptr<PipeEditSet> pipeEdits, pendingEdits;
    int soloRank = -1;
    AirState air = AirState::make(kTunedTempC, kTunedHumidity, kTunedPressureKPa);
    PipeAir pipeAir = PipeAir::of(AirState::make(kTunedTempC, kTunedHumidity, kTunedPressureKPa));
    double airT = -999, airH = -999, airP = -999, cTunedRatio = 1.0;
    bool airAtTuning = true;
    std::atomic<bool> editsDirty{false};
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
