// Pipe - physically modelled pipe organ
// Presets: a plain-text format shared by factory presets, user preset files
// and the host's saved state. Each preset can carry any subset of the groups
// below, so saving "stops only" or "tuning only" works the same way.
#pragma once

#include "Params.hpp"
#include "Matrix.hpp"
#include <map>
#include <string>
#include <sstream>
#include <vector>

namespace pipe {

enum PresetGroup { kGroupStops = 0, kGroupVoicing, kGroupTuning, kGroupWind, kGroupRoom, kGroupExpression, kGroupRouting, kNumGroups };
static const char* const kGroupIds[kNumGroups] = { "stops", "voicing", "tuning", "wind", "room", "expression", "routing" };
static const char* const kGroupLabels[kNumGroups] = { "STOPS & COUPLERS", "VOICING (PER PIPE)", "TUNING + PIPE OFFSETS", "WIND", "ROOM & OUTPUT", "EXPRESSION MAPPING", "INPUT ROUTING" };

// State keys (non-parameter data)
enum StateKey { kStateVoicing = 0, kStatePipeTune, kStateScalaScl, kStateScalaKbm, kStateScalaName, kStateCombos, kStatePresetName,
                kStateMatrix, kStateCustomIr, kNumStates };
static const char* const kStateKeys[kNumStates] = { "voicing", "pipetune", "scala_scl", "scala_kbm", "scala_name", "combos", "preset_name",
                "matrix", "custom_ir" };

inline int paramGroup(int id) {
    if (id < kNumStops) return kGroupStops;
    switch (id) {
    case kParamSwell: case kParamTremSpeed: case kParamTremDepth: case kParamTremTarget: case kParamStability:
    case kParamReservoir: case kParamBlowerNoise: case kParamConcussion: case kParamBlowerOn:
    case kParamResLoad: case kParamResSize: case kParamSpinDown: case kParamWindLeak: return kGroupWind;
    case kParamStrict: case kParamPressMax: case kParamPalletFast: case kParamPalletSlow:
    case kParamReleaseMs: case kParamBendStopped: case kParamBendOpen: case kParamBendString:
    case kParamBendReed: return kGroupExpression;
    case kParamTuning: case kParamEdoN: case kParamARef: case kParamMapLo: case kParamMapHi: case kParamMapPed:
    case kParamAnchorKey: case kParamEnharmonic: case kParamPipeTemp: case kParamAgeDrift: return kGroupTuning;
    case kParamRoom: case kParamDistance: case kParamMicHeight: case kParamWidth: case kParamRoomMix: case kParamPreDelay:
    case kParamOutputGain: case kParamMicType: case kParamQuality: case kParamLimiter: case kParamIrTrim: case kParamIrOrder: return kGroupRoom;
    case kParamZoneLoDiv: case kParamZoneHiDiv: case kParamPedalSplit: return kGroupRouting;
    default: return -1;
    }
}
inline int stateGroup(int s) {
    switch (s) {
    case kStateVoicing: return kGroupVoicing;
    case kStatePipeTune: case kStateScalaScl: case kStateScalaKbm: case kStateScalaName: return kGroupTuning;
    case kStateCombos: return kGroupStops;
    case kStateMatrix: return kGroupExpression;
    case kStateCustomIr: return kGroupRoom;
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
    { "OCTAVE 8 + PEDAL", "OPUS 14", "WEGSCHEIDER" },
    { "ORGANO PLENO", "OPUS 14", "WEGSCHEIDER" },
    { "PRINCIPALS 8+4", "OPUS 14", "WEGSCHEIDER" },
    { "FLUTES 8+4", "OPUS 14", "WEGSCHEIDER" },
    { "TRIO", "OPUS 14", "1/4 MEANTONE" },
    { "CORNET", "OPUS 14", "1/4 MEANTONE" },
    { "GRAND JEU", "OPUS 14", "WEGSCHEIDER" },
    { "GAMBA + CELESTE", "ROMANTIC", "12-TET" },
    { "FULL UNTERWERK", "ROMANTIC", "12-TET" },
    { "FOKKER 31 CHORUS", "MICROTONAL", "31-EDO" },
    { "BREATH FLUTE", "EXPRESSIVE", "WEGSCHEIDER" },
    { "PRESSURE REEDS", "EXPRESSIVE", "WEGSCHEIDER" },
};
// display order -> recipe below (P01 is the plugin's default sound)
static const int kFactoryRecipe[kNumFactoryPresets] = { 6, 0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 12 };

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
    p.states[kStateMatrix] = "";
    p.states[kStateCustomIr] = "";
    switch (kFactoryRecipe[index]) {
    case 0:
        on({ "i_praestant_16", "i_octave_8", "i_octave_4", "i_quinte_223", "i_superoctave_2", "i_mixture_v_113",
             "p_principal_16", "p_subbass_16", "p_octave_8", "p_octave_4", "p_mixture_v_223", "p_posaune_16" });
        set(kParamTuning, kTunWegscheider); break;
    case 1:
        on({ "ii_principal_8", "ii_octave_4", "p_subbass_16", "p_octave_8" });
        set(kParamTuning, kTunWegscheider); break;
    case 2:
        on({ "i_rohrflote_8", "i_spitzflote_4", "p_subbass_16", "p_bourdon_8" });
        set(kParamTuning, kTunWegscheider); break;
    case 3:
        on({ "i_rohrflote_8", "i_octave_4", "ii_gedeckt_8", "ii_rohrflote_4", "ii_sesquialtera_ii_223", "p_subbass_16", "p_bourdon_8" });
        set(kParamTuning, kTunMeantone); break;
    case 4:
        on({ "i_rohrflote_8", "i_cornet_v_8", "ii_gedeckt_8", "p_subbass_16" });
        set(kParamTuning, kTunMeantone); break;
    case 5:
        on({ "i_octave_8", "i_octave_4", "i_trumpet_8", "i_cornet_v_8", "ii_principal_8", "ii_octave_4", "ii_trichterregal_8",
             "iii_trompette_8", "iii_clairon_4", "cp_ii_i", "cp_iii_i", "p_posaune_16", "p_trumpet_8", "p_octave_8", "cp_i_p" });
        set(kParamTuning, kTunWegscheider); set(kParamSwell, 1.f); break;
    case 6:
        for (int s = 0; s < kNumStops; ++s) p.params[s] = paramInfo(s).def;
        set(kParamTuning, kTunWegscheider); break;
    case 7:
        on({ "iii_gamba_8", "iii_celeste_8", "tremulant", "p_subbass_16" });
        set(kParamTuning, kTun12); set(kParamSwell, 0.45f); set(kParamTremDepth, 0.12f); break;
    case 8:
        on({ "iii_bourdon_16", "iii_principal_8", "iii_gamba_8", "iii_flute_harmonique_8", "iii_principal_4", "iii_flute_harmonique_4",
             "iii_octavin_2", "iii_mixture_v_2", "iii_bassoon_16", "iii_trompette_8", "iii_oboe_8", "iii_clairon_4", "p_subbass_16", "p_octave_8", "cp_iii_p" });
        set(kParamTuning, kTun12); set(kParamSwell, 1.f); break;
    case 9:
        on({ "i_octave_8", "i_octave_4", "i_quinte_223", "i_superoctave_2", "p_subbass_16", "p_octave_8" });
        set(kParamTuning, kTun31); set(kParamMapLo, kMapSequential); set(kParamMapHi, kMapSequential); break;
    case 10:
        on({ "ii_gedeckt_8", "p_subbass_16" });
        set(kParamTuning, kTun19); set(kParamMapLo, kMapSequential); set(kParamMapHi, kMapSequential); break;
    case 11:
        on({ "iii_flute_harmonique_8" });
        set(kParamTuning, kTunWegscheider); set(kParamPressMax, 130.f);
        {   // breath: the key is the pallet all the way, aftertouch aims the jet
            ModMatrix m; m.setDefaults();
            m.rows[0].hi = 1.f; m.rows[0].curve = kCurveSoft; m.rows[0].smoothMs = 20.f;
            m.rows[1].dest = kDstJet; m.rows[1].restMid = false; m.rows[1].amount = 0.8f;
            m.rows[2].dest = kDstJet; m.rows[2].amount = 0.8f;
            p.states[kStateMatrix] = m.serialize();
        }
        set(kParamPalletSlow, 120.f); set(kParamStability, 0.9f); break;
    case 12:
        on({ "i_trumpet_8", "iii_oboe_8", "p_posaune_16" });
        set(kParamTuning, kTunWegscheider); set(kParamPressMax, 140.f); set(kParamSwell, 1.f);
        {   // reeds: aftertouch pushes the wind hard
            ModMatrix m; m.setDefaults();
            m.rows[1].zone = kZoneAll; m.rows[1].curve = kCurveLin;
            p.states[kStateMatrix] = m.serialize();
        }
        break;
    default: break;
    }
    return p;
}

} // namespace pipe
