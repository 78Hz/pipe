// Pipe - physically modelled pipe organ
// Presets: a plain-text format shared by factory presets, user preset files
// and the host's saved state. Each preset can carry any subset of the groups
// below, so saving "stops only" or "tuning only" works the same way.
#pragma once

#include "Params.hpp"
#include <map>
#include <string>
#include <sstream>
#include <vector>

namespace pipe {

enum PresetGroup { kGroupStops = 0, kGroupVoicing, kGroupTuning, kGroupWind, kGroupRoom, kGroupExpression, kGroupRouting, kNumGroups };
static const char* const kGroupIds[kNumGroups] = { "stops", "voicing", "tuning", "wind", "room", "expression", "routing" };
static const char* const kGroupLabels[kNumGroups] = { "STOPS & COUPLERS", "VOICING (PER PIPE)", "TUNING + PIPE OFFSETS", "WIND", "ROOM & OUTPUT", "EXPRESSION MAPPING", "INPUT ROUTING" };

// State keys (non-parameter data)
enum StateKey { kStateVoicing = 0, kStatePipeTune, kStateScalaScl, kStateScalaKbm, kStateScalaName, kStateCombos, kStatePresetName, kNumStates };
static const char* const kStateKeys[kNumStates] = { "voicing", "pipetune", "scala_scl", "scala_kbm", "scala_name", "combos", "preset_name" };

inline int paramGroup(int id) {
    if (id < kNumStops) return kGroupStops;
    switch (id) {
    case kParamSwell: case kParamTremSpeed: case kParamTremDepth: case kParamTremTarget: case kParamStability:
    case kParamReservoir: case kParamBlowerNoise: case kParamConcussion: case kParamBlowerOn: return kGroupWind;
    case kParamStrict: case kParamPressMin: case kParamPressMax: case kParamPalletFast: case kParamPalletSlow:
    case kParamReleaseMs: case kParamSlideDepth: case kParamBendStopped: case kParamBendOpen: case kParamBendString:
    case kParamBendReed: return kGroupExpression;
    case kParamTuning: case kParamEdoN: case kParamARef: case kParamMapLo: case kParamMapHi: case kParamMapPed:
    case kParamEnharmonic: case kParamPipeTemp: case kParamAgeDrift: return kGroupTuning;
    case kParamRoom: case kParamDistance: case kParamMicHeight: case kParamWidth: case kParamRoomMix: case kParamPreDelay:
    case kParamOutputGain: case kParamMicType: case kParamQuality: case kParamLimiter: return kGroupRoom;
    case kParamZoneLoDiv: case kParamZoneHiDiv: case kParamPedalSplit: return kGroupRouting;
    default: return -1;
    }
}
inline int stateGroup(int s) {
    switch (s) {
    case kStateVoicing: return kGroupVoicing;
    case kStatePipeTune: case kStateScalaScl: case kStateScalaKbm: case kStateScalaName: return kGroupTuning;
    case kStateCombos: return kGroupStops;
    default: return -1;
    }
}

struct PresetData {
    std::string name;
    unsigned groups = 0;                    // bit per PresetGroup
    std::map<int, float> params;
    std::map<int, std::string> states;
};

inline std::string encodePreset(const PresetData& p) {
    std::ostringstream o;
    o.precision(7);
    o << "PIPE PRESET 1\n";
    o << "name " << p.name << "\n";
    o << "groups";
    for (int g = 0; g < kNumGroups; ++g) if (p.groups & (1u << g)) o << " " << kGroupIds[g];
    o << "\n";
    for (const auto& kv : p.params) o << "p " << paramInfo(kv.first).symbol << " " << kv.second << "\n";
    for (const auto& kv : p.states) {
        o << "s " << kStateKeys[kv.first] << "\n";
        std::istringstream in(kv.second);
        std::string line;
        while (std::getline(in, line)) o << "| " << line << "\n";
        o << "end\n";
    }
    return o.str();
}

inline bool decodePreset(const std::string& text, PresetData& p) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("PIPE PRESET", 0) != 0) return false;
    p = PresetData();
    int curState = -1;
    std::string acc;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (curState >= 0) {
            if (line == "end") { p.states[curState] = acc; curState = -1; acc.clear(); }
            else if (line.size() >= 2 && line[0] == '|') { acc += line.size() > 2 ? line.substr(2) : std::string(); acc += "\n"; }
            continue;
        }
        if (line.rfind("name ", 0) == 0) p.name = line.substr(5);
        else if (line.rfind("groups", 0) == 0) {
            std::istringstream l(line.substr(6)); std::string g;
            while (l >> g) for (int i = 0; i < kNumGroups; ++i) if (g == kGroupIds[i]) p.groups |= 1u << i;
        } else if (line.rfind("p ", 0) == 0) {
            std::istringstream l(line.substr(2)); std::string sym; float v;
            if (l >> sym >> v) { int id = paramBySymbol(sym.c_str()); if (id >= 0 && id < kOutFirst) p.params[id] = v; }
        } else if (line.rfind("s ", 0) == 0) {
            const std::string key = line.substr(2);
            for (int i = 0; i < kNumStates; ++i) if (key == kStateKeys[i]) curState = i;
            acc.clear();
        }
    }
    return true;
}

// ------------------------------------------------------------ factory set
struct FactoryPreset { const char* name; const char* tag; const char* tuningLabel; };
constexpr int kNumFactoryPresets = 12;
static const FactoryPreset kFactory[kNumFactoryPresets] = {
    { "PLEIN JEU GT+PED", "BAROQUE", "WERCK III" },
    { "PRINCIPAL CHORUS", "BAROQUE", "12-TET" },
    { "FLUTES 8+4", "BAROQUE", "WERCK III" },
    { "TRIO SONATA", "BAROQUE", "WERCK III" },
    { "CORNET SEPARE", "BAROQUE", "1/4 MEANTONE" },
    { "GRAND JEU", "BAROQUE", "1/4 MEANTONE" },
    { "STRINGS + CELESTE", "ROMANTIC", "12-TET" },
    { "FULL SWELL", "ROMANTIC", "12-TET" },
    { "FOKKER 31 CHORUS", "MICROTONAL", "31-EDO" },
    { "GEDACKT 19", "MICROTONAL", "19-EDO" },
    { "BREATH FLUTE", "EXPRESSIVE", "12-TET" },
    { "PRESSURE REEDS", "EXPRESSIVE", "12-TET" },
};
// display order -> recipe below (P01 is the plugin's default sound)
static const int kFactoryRecipe[kNumFactoryPresets] = { 6, 0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11 };

inline int stopIndex(const char* id) {
    for (int s = 0; s < kNumStops; ++s) if (std::strcmp(kStops[s].id, id) == 0) return s;
    return -1;
}

// Builds a factory preset: every group except routing, from the defaults.
inline PresetData factoryPreset(int index) {
    PresetData p;
    p.name = kFactory[index].name;
    p.groups = 0;
    for (int g = 0; g < kNumGroups; ++g) if (g != kGroupRouting) p.groups |= 1u << g;
    for (int i = 0; i < kOutFirst; ++i) {
        const int g = paramGroup(i);
        if (g >= 0 && g != kGroupRouting) p.params[i] = paramInfo(i).def;
    }
    for (int s = 0; s < kNumStops; ++s) p.params[s] = 0.f;
    auto on = [&](std::initializer_list<const char*> ids) { for (auto id : ids) { int s = stopIndex(id); if (s >= 0) p.params[s] = 1.f; } };
    auto set = [&](int id, float v) { p.params[id] = v; };
    p.states[kStateVoicing] = "";
    p.states[kStatePipeTune] = "";
    p.states[kStateScalaScl] = "";
    p.states[kStateScalaKbm] = "";
    p.states[kStateScalaName] = "";
    p.states[kStateCombos] = "";
    switch (kFactoryRecipe[index]) {
    case 0:
        on({ "gt_principal", "gt_octave", "gt_superoct", "gt_mixtur", "pd_subbass", "pd_octavbass", "pd_choralbass", "cp_gt_pd" });
        set(kParamTuning, kTun12); break;
    case 1:
        on({ "gt_rohrflote", "gt_spitzflote", "pd_subbass", "pd_gedacktbass" });
        set(kParamTuning, kTunWerck); break;
    case 2:
        on({ "gt_principal", "sw_gedackt", "sw_flute", "sw_nasard", "pd_subbass", "pd_octavbass" });
        set(kParamTuning, kTunWerck); break;
    case 3:
        on({ "sw_gedackt", "sw_flute", "sw_nasard", "sw_doublette", "sw_tierce", "gt_rohrflote", "pd_subbass" });
        set(kParamTuning, kTunMeantone); set(kParamSwell, 1.f); break;
    case 4:
        on({ "gt_bourdon", "gt_principal", "gt_octave", "gt_trompete", "sw_gedackt", "sw_flute", "sw_nasard", "sw_tierce", "sw_hautbois",
             "cp_sw_gt", "pd_contrabass", "pd_octavbass", "pd_posaune", "cp_gt_pd" });
        set(kParamTuning, kTunMeantone); set(kParamSwell, 1.f); break;
    case 5:
        on({ "sw_salicional", "sw_celeste", "sw_tremulant", "gt_rohrflote", "pd_gedacktbass" });
        set(kParamTuning, kTun12); set(kParamSwell, 0.45f); set(kParamTremDepth, 0.12f); break;
    case 6:
        for (int s = 0; s < kNumStops; ++s) p.params[s] = paramInfo(s).def;
        set(kParamTuning, kTunWerck); break;
    case 7:
        on({ "sw_gedackt", "sw_salicional", "sw_principal", "sw_flute", "sw_nasard", "sw_doublette", "sw_tierce", "sw_hautbois", "cp_sw_gt",
             "gt_principal", "pd_subbass", "pd_octavbass", "cp_sw_pd" });
        set(kParamTuning, kTun12); set(kParamSwell, 1.f); break;
    case 8:
        on({ "gt_principal", "gt_octave", "gt_quinte", "gt_superoct", "pd_subbass", "pd_octavbass", "cp_gt_pd" });
        set(kParamTuning, kTun31); set(kParamMapLo, kMapSubset); set(kParamMapHi, kMapSequential); break;
    case 9:
        on({ "gt_rohrflote", "sw_gedackt", "pd_subbass" });
        set(kParamTuning, kTun19); set(kParamMapLo, kMapSubset); set(kParamMapHi, kMapSequential); break;
    case 10:
        on({ "gt_rohrflote", "sw_flute" });
        set(kParamTuning, kTun12); set(kParamPressMin, 25.f); set(kParamPressMax, 130.f); set(kParamSlideDepth, 1.f);
        set(kParamPalletSlow, 120.f); set(kParamStability, 0.9f); break;
    case 11:
        on({ "gt_trompete", "sw_hautbois", "pd_posaune" });
        set(kParamTuning, kTun12); set(kParamPressMin, 45.f); set(kParamPressMax, 140.f); set(kParamSwell, 1.f); break;
    default: break;
    }
    return p;
}

} // namespace pipe
