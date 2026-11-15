// Pipe - physically modelled pipe organ
// The instrument: Pasi Opus 14 (St. Cecilia Cathedral, Omaha, 2003). Four divisions, the stop list
// as published (Vogt 2010 App. 1; Pasi Organ Builders), mixtures expanded into their ranks. The
// stops and ranks come from the generated organ (Opus14Organ.hpp, tools/opus14_organ.cpp); the
// couplers and the tremulant are added here.
#pragma once

#include "PipeModels.hpp"
#include "Opus14Organ.hpp"
#include <array>
#include <cstring>
#include <string>

namespace pipe {

// Divisions. The engine's numbering keeps Pedal 0 and puts the enclosed division at 2.
//   I = Hauptwerk, II = Oberwerk, III = Unterwerk (enclosed, swell box)
enum Division { kPedal = 0, kGreat = 1, kSwell = 2, kPositive = 3, kNumDivisions = 4 };
inline const char* divisionName(int d) {
    static const char* n[] = { "PEDAL", "I HAUPTWERK", "III UNTERWERK", "II OBERWERK" };
    return (d >= 0 && d < kNumDivisions) ? n[d] : "-";
}
inline const char* divisionShort(int d) {
    static const char* n[] = { "PED", "I", "III", "II" };
    return (d >= 0 && d < kNumDivisions) ? n[d] : "-";
}

// Keyboard compass (MIDI key numbers of the 8' pitch): manuals C-a3, pedal C-f1 (Pasi)
constexpr int kManualLow = 36, kManualHigh = 93;
constexpr int kPedalLow = 36, kPedalHigh = 65;

enum PipeKind { kKindOpen = 0, kKindStopped, kKindString, kKindReed };

struct RankDef {
    const char* id;        // short code used in presets/state
    const char* name;      // display name
    int division;
    int kind;              // PipeKind
    int family;            // FluePipeFamily or ReedPipeFamily (reeds: the interim reed model)
    float ratio;           // nominal frequency ratio to 8' pitch (per key: rankRatio)
    int mixRow;            // mixture row, -1 otherwise
    // voicer's defaults (the flue pipes' sound comes from their Pasi dimensions; these remain for reeds)
    float scaleHT, mouth, cutup, toeMM, chiff;
    int nicking;
    float regulationDb;    // loudness trim
    float detuneCents;     // undulating stops (Celeste, Suavial)
    int lowestKey;         // first key that has a pipe
    float metalTin;        // % tin (display); 0 = wood
};

constexpr int kNumRanks = organ::kNumOrganRanks;

namespace detail {
inline int jetKind(int jf) { return jf == 1 ? kKindString : jf == 3 ? kKindStopped : kKindOpen; }
inline int reedFamilyOf(const char* name) {
    const char* oboes[] = { "Oboe", "Vox", "Regal", "Dulzian", "Bassoon" };
    for (auto o : oboes) if (std::strstr(name, o)) return kReedOboe;
    return kReedTrumpet;
}
struct RankIds { std::array<std::string, kNumRanks> s; };
inline const RankIds& rankIds() {
    static const RankIds ids = [] {
        RankIds r;
        for (int i = 0; i < kNumRanks; ++i) {
            const auto& R = organ::kOrganRanks[i];
            int n = 0; for (int j = 0; j < i; ++j) if (organ::kOrganRanks[j].stop == R.stop) ++n;
            r.s[(size_t)i] = std::string(organ::kOrganStops[R.stop].id) + (organ::kOrganStops[R.stop].rankCount > 1 ? "_" + std::to_string(n + 1) : "");
        }
        return r;
    }();
    return ids;
}
inline std::array<RankDef, kNumRanks> buildRanks() {
    std::array<RankDef, kNumRanks> a{};
    for (int i = 0; i < kNumRanks; ++i) {
        const auto& R = organ::kOrganRanks[i];
        RankDef d{};
        d.id = rankIds().s[(size_t)i].c_str();
        d.name = R.name;
        d.division = R.division;
        d.kind = R.reed ? kKindReed : jetKind(R.jetFamily);
        d.family = R.reed ? reedFamilyOf(R.name) : (d.kind == kKindStopped ? kFamilyStopped : d.kind == kKindString ? kFamilyString : kFamilyOpen);
        float ratio = 0.f; for (int k = 0; k < 128 && ratio == 0.f; ++k) ratio = organ::kOrganRatio[i][k];
        d.ratio = ratio;
        d.mixRow = organ::kOrganStops[R.stop].rankCount > 1 && std::strstr(R.name, "MIXTURE") ? 0 : -1;
        d.scaleHT = 0.f; d.mouth = 0.25f; d.cutup = 0.25f; d.toeMM = 79.f; d.chiff = 0.f; d.nicking = 0;
        d.regulationDb = 0.f;
        d.detuneCents = R.detuneCents;
        d.lowestKey = R.lowestKey;
        d.metalTin = std::strstr(R.name, "wood") || std::strstr(R.name, "Subbass") ? 0.f : 3.f;   // 97 % lead (Pasi)
        a[(size_t)i] = d;
    }
    return a;
}
} // namespace detail

static const std::array<RankDef, kNumRanks> kRanks = detail::buildRanks();

// Frequency ratio to 8' of the pipe under this rank and key (0: the rank has no pipe there).
inline float rankRatio(int rank, int key) {
    return (key >= 0 && key < 128) ? organ::kOrganRatio[rank][key] : 0.f;
}

// Stop controls (drawknobs)
enum StopType { kStopRank = 0, kStopMixture, kStopCoupler, kStopTremulant };
struct StopDef {
    const char* id;
    const char* label;
    const char* foot;
    int division;               // the keyboard it belongs to (couplers: the one that plays)
    int type;
    int rankFirst, rankCount;   // ranks this stop opens
    int couplerFrom;            // coupler: division whose pallets the key also opens
    bool reed;
};
// Couplers (V1): III/P, II/P, I/P, III/II, III/I, II/I; one tremulant.
constexpr int kNumCouplers = 6;
constexpr int kNumStops = organ::kNumOrganStops + kNumCouplers + 1;

namespace detail {
inline std::array<StopDef, kNumStops> buildStops() {
    std::array<StopDef, kNumStops> a{};
    for (int s = 0; s < organ::kNumOrganStops; ++s) {
        const auto& o = organ::kOrganStops[s];
        a[(size_t)s] = { o.id, o.label, o.foot, o.division, o.rankCount > 1 && std::strstr(o.label, "MIXTURE") ? kStopMixture : kStopRank,
                         o.rankFirst, o.rankCount, -1, o.reed };
    }
    int s = organ::kNumOrganStops;
    a[(size_t)s++] = { "cp_iii_p", "III / PEDAL", "CPL", kPedal, kStopCoupler, 0, 0, kSwell, false };
    a[(size_t)s++] = { "cp_ii_p", "II / PEDAL", "CPL", kPedal, kStopCoupler, 0, 0, kPositive, false };
    a[(size_t)s++] = { "cp_i_p", "I / PEDAL", "CPL", kPedal, kStopCoupler, 0, 0, kGreat, false };
    a[(size_t)s++] = { "cp_iii_ii", "III / II", "CPL", kPositive, kStopCoupler, 0, 0, kSwell, false };
    a[(size_t)s++] = { "cp_iii_i", "III / I", "CPL", kGreat, kStopCoupler, 0, 0, kSwell, false };
    a[(size_t)s++] = { "cp_ii_i", "II / I", "CPL", kGreat, kStopCoupler, 0, 0, kPositive, false };
    a[(size_t)s++] = { "tremulant", "TREMULANT", "WIND", kSwell, kStopTremulant, 0, 0, -1, false };   // must stay last
    return a;
}
} // namespace detail
static const std::array<StopDef, kNumStops> kStops = detail::buildStops();

inline int stopById(const char* id) {
    for (int s = 0; s < kNumStops; ++s) if (!std::strcmp(kStops[s].id, id)) return s;
    return -1;
}

inline int stopForRank(int rank) {
    return (rank >= 0 && rank < kNumRanks) ? organ::kOrganRanks[rank].stop : -1;
}

inline bool keyInCompass(int division, float keyFloat) {
    // keyFloat: 12-TET-equivalent key of the 8' pitch (allows microtonal keys)
    if (division == kPedal) return keyFloat > kPedalLow - 0.5f && keyFloat < kPedalHigh + 0.5f;
    return keyFloat > kManualLow - 0.5f && keyFloat < kManualHigh + 0.5f;
}

} // namespace pipe
