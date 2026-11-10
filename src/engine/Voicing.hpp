// Pipe - physically modelled pipe organ
// Voicing data (what a voicer adjusts at the organ) and the physical pipe
// dimensions derived from it. The DSP turns these dimensions into model
// parameters, and the UI shows them and checks they could be built.
#pragma once

#include "Stoplist.hpp"
#include <cmath>
#include <cstdio>
#include <string>
#include <sstream>

namespace pipe {

constexpr int kNumKeys = 128;
constexpr int kNumPipeSlots = 1024;

enum VoicingField { kVRegulation = 0, kVCutup, kVMouth, kVLanguid, kVNicking, kVChiff, kVToe, kNumVoicingFields };
static const char* const kVoicingFieldIds[kNumVoicingFields] = { "reg", "cut", "mouth", "lang", "nick", "chiff", "toe" };

struct RankVoicing {
    float scaleHT = 0.f;      // diameter vs normal scale, half-tones
    float halving = 16.f;     // diameter halves on this note (16th = Toepfer normal)
    float key[kNumVoicingFields][kNumKeys];
};

struct Voicing {
    RankVoicing rank[kNumRanks];
    float pipeTune[kNumRanks][kNumPipeSlots];   // per-pipe tuning offsets, cents

    void setDefaults() {
        for (int r = 0; r < kNumRanks; ++r) {
            const RankDef& d = kRanks[r];
            RankVoicing& v = rank[r];
            v.scaleHT = d.scaleHT;
            v.halving = 16.f;
            for (int k = 0; k < kNumKeys; ++k) {
                v.key[kVRegulation][k] = d.regulationDb;
                v.key[kVCutup][k] = d.cutup;
                v.key[kVMouth][k] = d.mouth;
                v.key[kVLanguid][k] = 0.f;
                v.key[kVNicking][k] = (float)d.nicking;
                v.key[kVChiff][k] = d.chiff;
                v.key[kVToe][k] = d.toeMM;
            }
            for (int p = 0; p < kNumPipeSlots; ++p) pipeTune[r][p] = 0.f;
        }
    }

    // Compact text form: only values that differ from the factory voicing.
    std::string serialize() const {
        std::ostringstream o;
        o.precision(5);
        for (int r = 0; r < kNumRanks; ++r) {
            const RankDef& d = kRanks[r];
            const RankVoicing& v = rank[r];
            if (v.scaleHT != d.scaleHT) o << "S " << d.id << " " << v.scaleHT << "\n";
            if (v.halving != 16.f) o << "H " << d.id << " " << v.halving << "\n";
            const float defs[kNumVoicingFields] = { d.regulationDb, d.cutup, d.mouth, 0.f, (float)d.nicking, d.chiff, d.toeMM };
            for (int f = 0; f < kNumVoicingFields; ++f)
                for (int k = 0; k < kNumKeys; ++k)
                    if (std::fabs(v.key[f][k] - defs[f]) > 1e-6f)
                        o << "V " << d.id << " " << kVoicingFieldIds[f] << " " << k << " " << v.key[f][k] << "\n";
        }
        return o.str();
    }
    std::string serializeTuning() const {
        std::ostringstream o;
        o.precision(5);
        for (int r = 0; r < kNumRanks; ++r)
            for (int p = 0; p < kNumPipeSlots; ++p)
                if (pipeTune[r][p] != 0.f) o << "T " << kRanks[r].id << " " << p << " " << pipeTune[r][p] << "\n";
        return o.str();
    }
    static int rankById(const std::string& id) {
        for (int r = 0; r < kNumRanks; ++r) if (id == kRanks[r].id) return r;
        return -1;
    }
    // Applies voicing lines (S/H/V). Resets voicing to defaults first.
    void parseVoicing(const std::string& text) {
        for (int r = 0; r < kNumRanks; ++r) {
            const RankDef& d = kRanks[r];
            RankVoicing& v = rank[r];
            v.scaleHT = d.scaleHT; v.halving = 16.f;
            const float defs[kNumVoicingFields] = { d.regulationDb, d.cutup, d.mouth, 0.f, (float)d.nicking, d.chiff, d.toeMM };
            for (int f = 0; f < kNumVoicingFields; ++f) for (int k = 0; k < kNumKeys; ++k) v.key[f][k] = defs[f];
        }
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream l(line);
            std::string tag, id; l >> tag >> id;
            int r = rankById(id); if (r < 0) continue;
            if (tag == "S") l >> rank[r].scaleHT;
            else if (tag == "H") l >> rank[r].halving;
            else if (tag == "V") {
                std::string field; int k; float val; l >> field >> k >> val;
                if (k < 0 || k >= kNumKeys) continue;
                for (int f = 0; f < kNumVoicingFields; ++f) if (field == kVoicingFieldIds[f]) rank[r].key[f][k] = val;
            }
        }
    }
    void parseTuning(const std::string& text) {
        for (int r = 0; r < kNumRanks; ++r) for (int p = 0; p < kNumPipeSlots; ++p) pipeTune[r][p] = 0.f;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream l(line);
            std::string tag, id; int p; float c; l >> tag >> id >> p >> c;
            int r = rankById(id);
            if (tag == "T" && r >= 0 && p >= 0 && p < kNumPipeSlots) pipeTune[r][p] = c;
        }
    }
};

// Physical description of one pipe.
struct PipeGeometry {
    double freq;          // sounding frequency, Hz
    double diameterMM;    // body diameter (inside)
    double lengthM;       // speaking length
    double mouthWidthMM;
    double cutupMM;
    double toeMM;         // foot pressure, mm water
    double jetSpeed;      // m/s
    double jetTransitMs;
    double jetRatio;      // transit time / period (physical)
    double flowWeight;    // wind consumption relative to Principal 8' C4
    bool stopped, reed;
};

constexpr double kSpeedOfSound = 343.0;   // m/s at 20 C
constexpr double kAirDensity = 1.2;

inline PipeGeometry pipeGeometry(const Voicing& v, int rank, int key, double freq, double toeOverride = -1) {
    const RankDef& d = kRanks[rank];
    const RankVoicing& rv = v.rank[rank];
    PipeGeometry g{};
    g.freq = freq;
    g.stopped = d.kind == kKindStopped;
    g.reed = d.kind == kKindReed;
    // pitch of the pipe in half-tones above 8' C (65.4 Hz); stopped pipes are
    // scaled by the pitch they sound, as builders do.
    const double semis = 12.0 * std::log2(freq / 65.406);
    const int k = std::max(0, std::min(kNumKeys - 1, key));
    g.diameterMM = 155.0 * std::pow(2.0, -(semis - rv.scaleHT) / std::max(12.f, rv.halving));
    const double r = g.diameterMM * 0.0005;
    if (g.reed) g.lengthM = kSpeedOfSound / (2.0 * freq) * 0.9;           // resonator, slightly short
    else if (g.stopped) g.lengthM = kSpeedOfSound / (4.0 * freq) - 0.8 * r;
    else g.lengthM = kSpeedOfSound / (2.0 * freq) - 1.6 * r;
    g.mouthWidthMM = rv.key[kVMouth][k] * M_PI * g.diameterMM;
    g.cutupMM = rv.key[kVCutup][k] * g.mouthWidthMM;
    g.toeMM = toeOverride > 0 ? toeOverride : rv.key[kVToe][k];
    const double pa = g.toeMM * 9.80665;
    g.jetSpeed = std::sqrt(2.0 * pa / kAirDensity);
    g.jetTransitMs = 1000.0 * (g.cutupMM * 0.001) / (0.4 * g.jetSpeed);
    g.jetRatio = g.jetTransitMs * 0.001 * freq;
    // flue flow ~ mouth width * jet thickness (~cut-up/4) * jet speed
    const double refArea = (0.25 * M_PI * 54.8) * (0.25 * 0.25 * M_PI * 54.8);
    const double area = g.reed ? 0.6 * g.diameterMM * g.diameterMM * 0.25 : g.mouthWidthMM * g.cutupMM;
    g.flowWeight = (area / refArea) * (g.jetSpeed / 35.8);
    return g;
}

// Jet ratio used by the model for a given physical jet ratio (jet transit time
// over the period). The model's mouth is idealised, so the physical value is
// mapped (compressively, keeping its direction) into the range where the model
// speaks its fundamental; see CalibrationTables.hpp.
constexpr double kJetModelMin = 0.34, kJetModelMax = 0.62;
inline double modelJetRatio(double physical) {
    return 0.44 * std::pow(std::max(1e-4, physical) / 0.197, 0.4);
}

struct BuildCheck {
    bool mouthOk, cutupOk, sizeOk, pressureOk;
    double speechMs;
};
inline BuildCheck checkPipe(const PipeGeometry& g) {
    BuildCheck c{};
    c.mouthOk = g.reed || g.mouthWidthMM <= (2.0 / 7.0) * M_PI * g.diameterMM + 1e-6;
    const double jr = modelJetRatio(g.jetRatio);
    c.cutupOk = g.reed || (jr >= kJetModelMin && jr <= kJetModelMax);
    c.sizeOk = g.lengthM <= 10.5 && g.diameterMM >= 3.0 && g.diameterMM <= 700.0;
    c.pressureOk = g.toeMM >= 30.0 && g.toeMM <= (g.reed ? 400.0 : 200.0);
    c.speechMs = 1000.0 * (12.0 / g.freq) + 15.0;
    return c;
}

} // namespace pipe
