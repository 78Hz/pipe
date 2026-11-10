// Pipe - physically modelled pipe organ
// The instrument: three divisions, 27 ranks of pipes and 28 stop controls.
#pragma once

#include "PipeModels.hpp"

namespace pipe {

enum Division { kPedal = 0, kGreat = 1, kSwell = 2, kNumDivisions = 3 };
inline const char* divisionName(int d) {
    static const char* n[] = { "PEDAL", "GREAT", "SWELL" };
    return (d >= 0 && d < kNumDivisions) ? n[d] : "-";
}

// Keyboard compass (MIDI key numbers of the 8' pitch)
constexpr int kManualLow = 36, kManualHigh = 93;  // C2 .. A6 (C - a''')
constexpr int kPedalLow = 36, kPedalHigh = 65;    // C2 .. F4 (C - f')

enum PipeKind { kKindOpen = 0, kKindStopped, kKindString, kKindReed };

struct RankDef {
    const char* id;        // short code used in presets/state
    const char* name;      // display name
    int division;
    int kind;              // PipeKind
    int family;            // FluePipeFamily or ReedPipeFamily
    float ratio;           // frequency ratio to 8' pitch (0 = mixture row, see mixtureRatio)
    int mixRow;            // mixture row 0..3, -1 otherwise
    // default voicing
    float scaleHT;         // diameter vs. normal (Toepfer) scale, in half-tones (+ = wider)
    float mouth;           // mouth width, fraction of circumference
    float cutup;           // cut-up, fraction of mouth width
    float toeMM;           // toe (foot) pressure, mm water column
    float chiff;           // attack transient 0..1
    int nicking;           // 0 none, 1 light, 2 heavy
    float regulationDb;    // loudness trim
    float detuneCents;     // celeste
    int lowestKey;         // first key that has a pipe (celestes start at tenor C)
    float metalTin;        // % tin (display only); 0 = wood
};

constexpr int kNumRanks = 27;
constexpr RankDef kRanks[kNumRanks] = {
    // Pedal
    { "pd_contrabass", "CONTRABASS 16'",  kPedal, kKindOpen,    kFamilyOpen,    0.5f, -1,  3.f, 0.25f, 0.30f, 94.f, 0.35f, 1, -1.f, 0.f, 0, 0.f },
    { "pd_subbass",    "SUBBASS 16'",     kPedal, kKindStopped, kFamilyStopped, 0.5f, -1,  6.f, 0.25f, 0.33f, 94.f, 0.25f, 1,  0.f, 0.f, 0, 0.f },
    { "pd_octavbass",  "OCTAVBASS 8'",    kPedal, kKindOpen,    kFamilyOpen,    1.0f, -1,  2.f, 0.25f, 0.27f, 94.f, 0.35f, 1, -2.f, 0.f, 0, 75.f },
    { "pd_gedacktbass","GEDACKTBASS 8'",  kPedal, kKindStopped, kFamilyStopped, 1.0f, -1,  4.f, 0.25f, 0.32f, 94.f, 0.25f, 1, -3.f, 0.f, 0, 0.f },
    { "pd_choralbass", "CHORALBASS 4'",   kPedal, kKindOpen,    kFamilyOpen,    2.0f, -1,  0.f, 0.25f, 0.25f, 94.f, 0.40f, 1, -3.f, 0.f, 0, 75.f },
    { "pd_posaune",    "POSAUNE 16'",     kPedal, kKindReed,    kReedTrumpet,   0.5f, -1,  0.f, 0.25f, 0.25f, 94.f, 0.0f,  0, -1.f, 0.f, 0, 50.f },
    // Great
    { "gt_bourdon",    "BOURDON 16'",     kGreat, kKindStopped, kFamilyStopped, 0.5f, -1,  3.f, 0.25f, 0.32f, 78.f, 0.25f, 1, -3.f, 0.f, 0, 0.f },
    { "gt_principal",  "PRINCIPAL 8'",    kGreat, kKindOpen,    kFamilyOpen,    1.0f, -1,  0.f, 0.25f, 0.25f, 78.f, 0.45f, 1,  0.f, 0.f, 0, 75.f },
    { "gt_rohrflote",  "ROHRFLOTE 8'",    kGreat, kKindStopped, kFamilyStopped, 1.0f, -1,  3.f, 0.25f, 0.30f, 78.f, 0.40f, 0, -3.f, 0.f, 0, 40.f },
    { "gt_octave",     "OCTAVE 4'",       kGreat, kKindOpen,    kFamilyOpen,    2.0f, -1,  0.f, 0.25f, 0.25f, 78.f, 0.45f, 1, -1.f, 0.f, 0, 75.f },
    { "gt_spitzflote", "SPITZFLOTE 4'",   kGreat, kKindOpen,    kFamilyOpen,    2.0f, -1,  2.f, 0.22f, 0.30f, 78.f, 0.40f, 0, -4.f, 0.f, 0, 40.f },
    { "gt_quinte",     "QUINTE 2-2/3'",   kGreat, kKindOpen,    kFamilyOpen,    3.0f, -1, -1.f, 0.25f, 0.25f, 78.f, 0.40f, 1, -6.f, 0.f, 0, 75.f },
    { "gt_superoct",   "SUPEROCTAVE 2'",  kGreat, kKindOpen,    kFamilyOpen,    4.0f, -1, -1.f, 0.25f, 0.25f, 78.f, 0.45f, 1, -4.f, 0.f, 0, 75.f },
    { "gt_mixtur1",    "MIXTUR IV a",     kGreat, kKindOpen,    kFamilyOpen,    0.0f,  0, -2.f, 0.25f, 0.24f, 78.f, 0.45f, 1, -8.f, 0.f, 0, 75.f },
    { "gt_mixtur2",    "MIXTUR IV b",     kGreat, kKindOpen,    kFamilyOpen,    0.0f,  1, -2.f, 0.25f, 0.24f, 78.f, 0.45f, 1, -8.f, 0.f, 0, 75.f },
    { "gt_mixtur3",    "MIXTUR IV c",     kGreat, kKindOpen,    kFamilyOpen,    0.0f,  2, -2.f, 0.25f, 0.24f, 78.f, 0.45f, 1, -9.f, 0.f, 0, 75.f },
    { "gt_mixtur4",    "MIXTUR IV d",     kGreat, kKindOpen,    kFamilyOpen,    0.0f,  3, -2.f, 0.25f, 0.24f, 78.f, 0.45f, 1, -9.f, 0.f, 0, 75.f },
    { "gt_trompete",   "TROMPETE 8'",     kGreat, kKindReed,    kReedTrumpet,   1.0f, -1,  0.f, 0.25f, 0.25f, 78.f, 0.0f,  0, -2.f, 0.f, 0, 50.f },
    // Swell
    { "sw_gedackt",    "GEDACKT 8'",      kSwell, kKindStopped, kFamilyStopped, 1.0f, -1,  3.f, 0.25f, 0.32f, 85.f, 0.35f, 0, -2.f, 0.f, 0, 0.f },
    { "sw_salicional", "SALICIONAL 8'",   kSwell, kKindString,  kFamilyString,  1.0f, -1, -6.f, 0.20f, 0.22f, 85.f, 0.20f, 2, -5.f, 0.f, 0, 75.f },
    { "sw_celeste",    "VOIX CELESTE 8'", kSwell, kKindString,  kFamilyString,  1.0f, -1, -6.f, 0.20f, 0.22f, 85.f, 0.20f, 2, -5.f, 4.f, 48, 75.f },
    { "sw_principal",  "PRINCIPAL 4'",    kSwell, kKindOpen,    kFamilyOpen,    2.0f, -1,  0.f, 0.25f, 0.25f, 85.f, 0.40f, 1, -2.f, 0.f, 0, 75.f },
    { "sw_flute",      "FLUTE HARM. 4'",  kSwell, kKindOpen,    kFamilyOpen,    2.0f, -1,  3.f, 0.25f, 0.30f, 85.f, 0.30f, 1, -2.f, 0.f, 0, 40.f },
    { "sw_nasard",     "NASARD 2-2/3'",   kSwell, kKindOpen,    kFamilyOpen,    3.0f, -1,  3.f, 0.22f, 0.30f, 85.f, 0.35f, 0, -6.f, 0.f, 0, 40.f },
    { "sw_doublette",  "DOUBLETTE 2'",    kSwell, kKindOpen,    kFamilyOpen,    4.0f, -1, -1.f, 0.25f, 0.25f, 85.f, 0.45f, 1, -5.f, 0.f, 0, 75.f },
    { "sw_tierce",     "TIERCE 1-3/5'",   kSwell, kKindOpen,    kFamilyOpen,    5.0f, -1,  2.f, 0.22f, 0.30f, 85.f, 0.35f, 0, -8.f, 0.f, 0, 40.f },
    { "sw_hautbois",   "HAUTBOIS 8'",     kSwell, kKindReed,    kReedOboe,      1.0f, -1,  0.f, 0.25f, 0.25f, 85.f, 0.0f,  0, -4.f, 0.f, 0, 50.f },
};

// Mixture IV 1-1/3': harmonic numbers above 8' for each row, breaking back
// (repeating) every octave so the top pipes stay a buildable size.
inline float mixtureRatio(int row, int key) {
    static const float rows[4][4] = {
        { 6.f, 8.f, 12.f, 16.f },   // C2..B2 : 1-1/3' 1' 2/3' 1/2'
        { 4.f, 6.f, 8.f, 12.f },    // C3..B3 : 2' 1-1/3' 1' 2/3'
        { 3.f, 4.f, 6.f, 8.f },     // C4..B4 : 2-2/3' 2' 1-1/3' 1'
        { 2.f, 3.f, 4.f, 6.f },     // C5..   : 4' 2-2/3' 2' 1-1/3'
    };
    int b = key < 48 ? 0 : key < 60 ? 1 : key < 72 ? 2 : 3;
    return rows[b][row];
}

inline float rankRatio(int rank, int key) {
    const RankDef& r = kRanks[rank];
    return r.mixRow >= 0 ? mixtureRatio(r.mixRow, key) : r.ratio;
}

// Stop controls (drawknobs / tabs)
enum StopType { kStopRank = 0, kStopMixture, kStopCoupler, kStopTremulant };
struct StopDef {
    const char* id;
    const char* label;
    const char* foot;
    int division;
    int type;
    int rankFirst, rankCount;   // ranks this stop opens
    int couplerFrom;            // coupler: source division whose pipes are added
    bool reed;
};
constexpr int kNumStops = 28;
constexpr StopDef kStops[kNumStops] = {
    { "pd_contrabass", "CONTRABASS",  "16'",    kPedal, kStopRank, 0, 1, -1, false },
    { "pd_subbass",    "SUBBASS",     "16'",    kPedal, kStopRank, 1, 1, -1, false },
    { "pd_octavbass",  "OCTAVBASS",   "8'",     kPedal, kStopRank, 2, 1, -1, false },
    { "pd_gedacktbass","GEDACKTBASS", "8'",     kPedal, kStopRank, 3, 1, -1, false },
    { "pd_choralbass", "CHORALBASS",  "4'",     kPedal, kStopRank, 4, 1, -1, false },
    { "pd_posaune",    "POSAUNE",     "16'R",   kPedal, kStopRank, 5, 1, -1, true },
    { "cp_gt_pd",      "GT > PED",    "CPL",    kPedal, kStopCoupler, 0, 0, kGreat, false },
    { "cp_sw_pd",      "SW > PED",    "CPL",    kPedal, kStopCoupler, 0, 0, kSwell, false },
    { "gt_bourdon",    "BOURDON",     "16'",    kGreat, kStopRank, 6, 1, -1, false },
    { "gt_principal",  "PRINCIPAL",   "8'",     kGreat, kStopRank, 7, 1, -1, false },
    { "gt_rohrflote",  "ROHRFLOTE",   "8'",     kGreat, kStopRank, 8, 1, -1, false },
    { "gt_octave",     "OCTAVE",      "4'",     kGreat, kStopRank, 9, 1, -1, false },
    { "gt_spitzflote", "SPITZFLOTE",  "4'",     kGreat, kStopRank, 10, 1, -1, false },
    { "gt_quinte",     "QUINTE",      "2-2/3'", kGreat, kStopRank, 11, 1, -1, false },
    { "gt_superoct",   "SUPEROCTAVE", "2'",     kGreat, kStopRank, 12, 1, -1, false },
    { "gt_mixtur",     "MIXTUR IV",   "1-1/3'", kGreat, kStopMixture, 13, 4, -1, false },
    { "gt_trompete",   "TROMPETE",    "8'R",    kGreat, kStopRank, 17, 1, -1, true },
    { "cp_sw_gt",      "SW > GT",     "CPL",    kGreat, kStopCoupler, 0, 0, kSwell, false },
    { "sw_gedackt",    "GEDACKT",     "8'",     kSwell, kStopRank, 18, 1, -1, false },
    { "sw_salicional", "SALICIONAL",  "8'",     kSwell, kStopRank, 19, 1, -1, false },
    { "sw_celeste",    "VOIX CELESTE","8'",     kSwell, kStopRank, 20, 1, -1, false },
    { "sw_principal",  "PRINCIPAL",   "4'",     kSwell, kStopRank, 21, 1, -1, false },
    { "sw_flute",      "FLUTE HARM.", "4'",     kSwell, kStopRank, 22, 1, -1, false },
    { "sw_nasard",     "NASARD",      "2-2/3'", kSwell, kStopRank, 23, 1, -1, false },
    { "sw_doublette",  "DOUBLETTE",   "2'",     kSwell, kStopRank, 24, 1, -1, false },
    { "sw_tierce",     "TIERCE",      "1-3/5'", kSwell, kStopRank, 25, 1, -1, false },
    { "sw_hautbois",   "HAUTBOIS",    "8'R",    kSwell, kStopRank, 26, 1, -1, true },
    { "sw_tremulant",  "TREMULANT",   "WIND",   kSwell, kStopTremulant, 0, 0, -1, false },
};

inline int stopForRank(int rank) {
    for (int s = 0; s < kNumStops; ++s)
        if ((kStops[s].type == kStopRank || kStops[s].type == kStopMixture) &&
            rank >= kStops[s].rankFirst && rank < kStops[s].rankFirst + kStops[s].rankCount)
            return s;
    return -1;
}

inline bool keyInCompass(int division, float keyFloat) {
    // keyFloat: 12-TET-equivalent key of the 8' pitch (allows microtonal keys)
    if (division == kPedal) return keyFloat > kPedalLow - 0.5f && keyFloat < kPedalHigh + 0.5f;
    return keyFloat > kManualLow - 0.5f && keyFloat < kManualHigh + 0.5f;
}

} // namespace pipe
