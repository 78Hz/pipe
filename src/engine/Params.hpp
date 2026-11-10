// Pipe - physically modelled pipe organ
// Parameter list shared by the DSP and the UI.
#pragma once

#include "Stoplist.hpp"
#include <cstring>
#include <cstdio>

namespace pipe {

enum ParamType { kPFloat = 0, kPBool, kPInt, kPEnum, kPOutput };

struct ParamInfo {
    const char* symbol;
    const char* name;
    float min, max, def;
    int type;
    const char* unit;
    int enumCount;
    const char* const* enumLabels;
};

// enum labels
static const char* const kTremTargetLabels[] = { "PEDAL", "GREAT", "SWELL" };
static const char* const kTuningLabels[] = {
    "12-TET", "WERCKMEISTER III", "1/4 MEANTONE", "KIRNBERGER III", "VALLOTTI",
    "19-EDO", "22-EDO", "24-EDO", "31-EDO", "41-EDO", "53-EDO", "CUSTOM EDO", "SCALA FILE" };
enum TuningSystem { kTun12 = 0, kTunWerck, kTunMeantone, kTunKirn, kTunVallotti,
    kTun19, kTun22, kTun24, kTun31, kTun41, kTun53, kTunCustomEdo, kTunScala, kNumTuningSystems };
static const char* const kMapLabels[] = { "SUBSET", "SEQUENTIAL", "SCALA KBM" };
enum KeyMapMode { kMapSubset = 0, kMapSequential, kMapKbm };
static const char* const kRoomLabels[] = { "ST ANDREW'S LYNDHURST", "ST MARGARET'S YORK", "YORK MINSTER", "JACK LYONS HALL",
    "CUSTOM IR", "LITE VILLAGE CHURCH", "LITE ABBEY NAVE", "LITE CATHEDRAL", "LITE CONCERT HALL", "DRY / CLOSE" };
enum RoomPreset { kRoomStAndrews = 0, kRoomStMargarets, kRoomMinster, kRoomJackLyons, kRoomCustom,
    kRoomLiteVillage, kRoomLiteAbbey, kRoomLiteCathedral, kRoomLiteHall, kRoomDry, kNumRooms };
constexpr int kNumMeasuredRooms = 4;
inline bool roomIsMeasured(int r) { return r >= kRoomStAndrews && r <= kRoomJackLyons; }
inline bool roomIsConvolution(int r) { return r <= kRoomCustom; }
static const char* const kIrOrderLabels[] = { "FUMA", "AMBIX" };
static const char* const kMicLabels[] = { "AB", "ORTF", "BINAURAL" };
static const char* const kQualityLabels[] = { "ECO", "STD", "HQ" };
static const char* const kZoneDivLabels[] = { "PEDAL", "GREAT", "SWELL", "OFF" };

enum ParamId {
    kParamStop0 = 0,
    kParamSwell = kNumStops,
    kParamTremSpeed, kParamTremDepth, kParamTremTarget,
    kParamStability, kParamReservoir, kParamBlowerNoise, kParamConcussion, kParamBlowerOn,
    kParamStrict, kParamPressMax, kParamPalletFast, kParamPalletSlow, kParamReleaseMs,
    kParamBendStopped, kParamBendOpen, kParamBendString, kParamBendReed,
    kParamTuning, kParamEdoN, kParamARef, kParamMapLo, kParamMapHi, kParamMapPed, kParamEnharmonic,
    kParamPipeTemp, kParamAgeDrift,
    kParamRoom, kParamDistance, kParamMicHeight, kParamWidth, kParamRoomMix, kParamPreDelay,
    kParamOutputGain, kParamMicType, kParamQuality, kParamLimiter, kParamIrTrim, kParamIrOrder,
    kParamZoneLoDiv, kParamZoneHiDiv, kParamPedalSplit,
    // ---- outputs (DSP -> UI meters) ----
    kOutFirst,
    kOutReservoir = kOutFirst, kOutChestPd, kOutChestGt, kOutChestSw, kOutDemand, kOutPipes, kOutCpu,
    kOutPeakL, kOutPeakR,
    kOutSeenLo, kOutSeenHi, kOutMembersLo, kOutMembersHi, kOutBendLo, kOutBendHi,
    // live view of the most recent note: sources in, mechanisms out
    kOutLiveKey, kOutLiveZone, kOutLiveStrike, kOutLivePressure, kOutLiveAftertouch, kOutLiveCC74, kOutLiveBend,
    kOutDstPallet, kOutDstBoost, kOutDstJet, kOutDstPitch, kOutDstChiff, kOutDstFoot, kOutSwellEff, kOutTremEff,
    kOutLearn,                       // last expressive message (type*128 + cc) + 1024 * counter
    kOutRoomStatus, kOutRoomPos, kOutRoomPosDist,
    kOutViaLo, kOutViaHi, kOutClip, kOutLimitDb,
    kOutNote0,                       // 5 x (key, source, foot pressure mm, cents)
    kOutNoteLast = kOutNote0 + 5 * 4 - 1,
    kNumParams
};
constexpr int kNumShownNotes = 5;

// "seen" bits per MPE zone (what the incoming data actually contains)
// kSeenPressure: key-travel pressure (any message); kSeenAftertouch: pressure that starts after the key is down
enum SeenBits { kSeenVelocity = 1, kSeenPressure = 2, kSeenBend = 4, kSeenSlide = 8, kSeenRelease = 16, kSeenAftertouch = 32, kSeenMcm = 64 };
// How each zone's expression actually arrives (kOutViaLo / kOutViaHi), for the Expression screen
enum ViaBits { kViaTravelChan = 1, kViaTravelPoly = 2, kViaAfterChan = 4, kViaAfterPoly = 8, kViaCC74 = 16, kViaHiRes = 32,
               kViaOneChannel = 64, kViaMemberChannels = 128 };

inline const ParamInfo& paramInfo(int id) {
    static ParamInfo table[kNumParams];
    static bool init = false;
    if (!init) {
        init = true;
        // stop defaults: preset P07 "Plein jeu GT+PED"
        static const bool stopDef[kNumStops] = {
            1,1,1,1,0,0, 1,0,                 // pedal + couplers
            1,1,0,1,0,1,1,1,0, 0,             // great
            1,0,0,1,1,1,1,0,0, 0 };           // swell
        static char stopNames[kNumStops][48];
        for (int s = 0; s < kNumStops; ++s) {
            std::snprintf(stopNames[s], sizeof(stopNames[s]), "%s %s %s",
                divisionName(kStops[s].division), kStops[s].label, kStops[s].foot);
            table[s] = { kStops[s].id, stopNames[s], 0.f, 1.f, stopDef[s] ? 1.f : 0.f, kPBool, "", 0, nullptr };
        }
        auto set = [](int i, ParamInfo p) { table[i] = p; };
        set(kParamSwell,       { "swell", "Swell Shutters", 0.f, 1.f, 0.64f, kPFloat, "", 0, nullptr });
        set(kParamTremSpeed,   { "trem_speed", "Tremulant Speed", 3.f, 9.f, 5.8f, kPFloat, "Hz", 0, nullptr });
        set(kParamTremDepth,   { "trem_depth", "Tremulant Depth", 0.f, 0.5f, 0.22f, kPFloat, "", 0, nullptr });
        set(kParamTremTarget,  { "trem_target", "Tremulant Division", 0.f, 2.f, 2.f, kPEnum, "", 3, kTremTargetLabels });
        set(kParamStability,   { "wind_flex", "Wind Flexibility", 0.f, 1.f, 0.7f, kPFloat, "", 0, nullptr });
        set(kParamReservoir,   { "reservoir", "Reservoir Pressure", 60.f, 140.f, 96.f, kPFloat, "mm", 0, nullptr });
        set(kParamBlowerNoise, { "blower_noise", "Blower Noise", -90.f, -30.f, -62.f, kPFloat, "dB", 0, nullptr });
        set(kParamConcussion,  { "concussion", "Concussion Bellows", 0.f, 1.f, 0.4f, kPFloat, "", 0, nullptr });
        set(kParamBlowerOn,    { "blower", "Blower", 0.f, 1.f, 1.f, kPBool, "", 0, nullptr });
        set(kParamStrict,      { "strict", "Strict Physical Limits", 0.f, 1.f, 1.f, kPBool, "", 0, nullptr });
        set(kParamPressMax,    { "boost_max", "Wind Boost Max Pressure", 80.f, 160.f, 110.f, kPFloat, "mm", 0, nullptr });
        set(kParamPalletFast,  { "pallet_fast", "Pallet Open (hard strike)", 2.f, 40.f, 8.f, kPFloat, "ms", 0, nullptr });
        set(kParamPalletSlow,  { "pallet_slow", "Pallet Open (soft strike)", 10.f, 200.f, 60.f, kPFloat, "ms", 0, nullptr });
        set(kParamReleaseMs,   { "pallet_close", "Pallet Close (default)", 5.f, 80.f, 20.f, kPFloat, "ms", 0, nullptr });
        set(kParamBendStopped, { "bend_stopped", "Bend Limit Stopped", 0.f, 400.f, 200.f, kPFloat, "ct", 0, nullptr });
        set(kParamBendOpen,    { "bend_open", "Bend Limit Open Flue", 0.f, 200.f, 50.f, kPFloat, "ct", 0, nullptr });
        set(kParamBendString,  { "bend_string", "Bend Limit String", 0.f, 200.f, 30.f, kPFloat, "ct", 0, nullptr });
        set(kParamBendReed,    { "bend_reed", "Bend Limit Reed", 0.f, 100.f, 15.f, kPFloat, "ct", 0, nullptr });
        set(kParamTuning,      { "tuning", "Tuning System", 0.f, (float)(kNumTuningSystems - 1), (float)kTunWerck, kPEnum, "", kNumTuningSystems, kTuningLabels });
        set(kParamEdoN,        { "edo_n", "Custom EDO Steps", 5.f, 72.f, 31.f, kPInt, "", 0, nullptr });
        set(kParamARef,        { "a_ref", "A4 Reference", 400.f, 480.f, 440.f, kPFloat, "Hz", 0, nullptr });
        set(kParamMapLo,       { "map_lo", "Key Map Lower Zone", 0.f, 2.f, 0.f, kPEnum, "", 3, kMapLabels });
        set(kParamMapHi,       { "map_hi", "Key Map Upper Zone", 0.f, 2.f, 1.f, kPEnum, "", 3, kMapLabels });
        set(kParamMapPed,      { "map_ped", "Key Map Pedal Split", 0.f, 2.f, 0.f, kPEnum, "", 3, kMapLabels });
        set(kParamEnharmonic,  { "enharmonic", "Flats Instead Of Sharps", 0.f, 1.f, 0.f, kPBool, "", 0, nullptr });
        set(kParamPipeTemp,    { "pipe_temp", "Pipe Temperature", 5.f, 35.f, 18.f, kPFloat, "C", 0, nullptr });
        set(kParamAgeDrift,    { "age_drift", "Age Drift", 0.f, 10.f, 1.5f, kPFloat, "ct", 0, nullptr });
        set(kParamRoom,        { "room", "Building", 0.f, (float)(kNumRooms - 1), (float)kRoomStMargarets, kPEnum, "", kNumRooms, kRoomLabels });
        set(kParamDistance,    { "distance", "Listener Distance", 2.f, 40.f, 18.f, kPFloat, "m", 0, nullptr });
        set(kParamMicHeight,   { "mic_height", "Listener Height", 1.f, 8.f, 3.f, kPFloat, "m", 0, nullptr });
        set(kParamWidth,       { "width", "Stereo Width", 0.f, 2.f, 1.1f, kPFloat, "", 0, nullptr });
        set(kParamRoomMix,     { "room_mix", "Room Mix", 0.f, 1.f, 0.55f, kPFloat, "", 0, nullptr });
        set(kParamPreDelay,    { "predelay", "Pre-delay", 0.f, 80.f, 22.f, kPFloat, "ms", 0, nullptr });
        set(kParamOutputGain,  { "output", "Output Gain", -24.f, 12.f, -6.f, kPFloat, "dB", 0, nullptr });
        set(kParamMicType,     { "mics", "Microphones", 0.f, 2.f, 0.f, kPEnum, "", 3, kMicLabels });
        set(kParamQuality,     { "quality", "Quality", 0.f, 2.f, 1.f, kPEnum, "", 3, kQualityLabels });
        set(kParamLimiter,     { "limiter", "Output Limiter", 0.f, 1.f, 1.f, kPBool, "", 0, nullptr });
        set(kParamIrTrim,      { "ir_trim_direct", "IR Direct Sound From Model", 0.f, 1.f, 1.f, kPBool, "", 0, nullptr });
        set(kParamIrOrder,     { "ir_order", "Custom IR 4-Channel Order", 0.f, 1.f, 0.f, kPEnum, "", 2, kIrOrderLabels });
        set(kParamZoneLoDiv,   { "zone_lo", "Lower Zone Plays", 0.f, 3.f, 1.f, kPEnum, "", 4, kZoneDivLabels });
        set(kParamZoneHiDiv,   { "zone_hi", "Upper Zone Plays", 0.f, 3.f, 2.f, kPEnum, "", 4, kZoneDivLabels });
        set(kParamPedalSplit,  { "pedal_split", "Pedal Split Key (0=off)", 0.f, 72.f, 0.f, kPInt, "", 0, nullptr });
        set(kOutReservoir,     { "out_reservoir", "Reservoir", 0.f, 200.f, 0.f, kPOutput, "mm", 0, nullptr });
        set(kOutChestPd,       { "out_chest_pd", "Pedal Chest", 0.f, 200.f, 0.f, kPOutput, "mm", 0, nullptr });
        set(kOutChestGt,       { "out_chest_gt", "Great Chest", 0.f, 200.f, 0.f, kPOutput, "mm", 0, nullptr });
        set(kOutChestSw,       { "out_chest_sw", "Swell Chest", 0.f, 200.f, 0.f, kPOutput, "mm", 0, nullptr });
        set(kOutDemand,        { "out_demand", "Wind Demand", 0.f, 2.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutPipes,         { "out_pipes", "Pipes Speaking", 0.f, 1024.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutCpu,           { "out_cpu", "DSP Load", 0.f, 100.f, 0.f, kPOutput, "%", 0, nullptr });
        set(kOutPeakL,         { "out_peak_l", "Peak L", 0.f, 2.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutPeakR,         { "out_peak_r", "Peak R", 0.f, 2.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutSeenLo,        { "out_seen_lo", "Lower Zone Data", 0.f, 127.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutSeenHi,        { "out_seen_hi", "Upper Zone Data", 0.f, 127.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutMembersLo,     { "out_members_lo", "Lower Zone Channels", 0.f, 15.f, 15.f, kPOutput, "", 0, nullptr });
        set(kOutMembersHi,     { "out_members_hi", "Upper Zone Channels", 0.f, 15.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutBendLo,        { "out_bend_lo", "Lower Zone Bend Range", 0.f, 96.f, 48.f, kPOutput, "st", 0, nullptr });
        set(kOutBendHi,        { "out_bend_hi", "Upper Zone Bend Range", 0.f, 96.f, 48.f, kPOutput, "st", 0, nullptr });
        static const char* liveSyms[] = { "out_live_key", "out_live_zone", "out_live_strike", "out_live_pressure", "out_live_polyat",
            "out_live_cc74", "out_live_bend", "out_dst_pallet", "out_dst_boost", "out_dst_jet", "out_dst_pitch", "out_dst_chiff",
            "out_dst_foot", "out_swell_eff", "out_trem_eff", "out_learn", "out_room_status", "out_room_pos", "out_room_pos_dist" };
        for (int i = kOutLiveKey; i <= kOutRoomPosDist; ++i)
            table[i] = { liveSyms[i - kOutLiveKey], liveSyms[i - kOutLiveKey] + 4, -100000.f, 100000.f, 0.f, kPOutput, "", 0, nullptr };
        set(kOutViaLo,         { "out_via_lo", "Lower Zone Expression Sources", 0.f, 255.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutViaHi,         { "out_via_hi", "Upper Zone Expression Sources", 0.f, 255.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutClip,          { "out_clip", "Peak Before Limiter", 0.f, 16.f, 0.f, kPOutput, "", 0, nullptr });
        set(kOutLimitDb,       { "out_limit_db", "Limiter Gain Reduction", 0.f, 60.f, 0.f, kPOutput, "dB", 0, nullptr });
        static char noteSyms[kNumShownNotes * 4][24];
        static char noteNames[kNumShownNotes * 4][32];
        static const char* fields[4] = { "key", "src", "mm", "ct" };
        for (int i = 0; i < kNumShownNotes * 4; ++i) {
            std::snprintf(noteSyms[i], 24, "out_note%d_%s", i / 4, fields[i % 4]);
            std::snprintf(noteNames[i], 32, "Note %d %s", i / 4 + 1, fields[i % 4]);
            table[kOutNote0 + i] = { noteSyms[i], noteNames[i], -2000.f, 2000.f, -1.f, kPOutput, "", 0, nullptr };
        }
    }
    return table[id];
}

inline int paramBySymbol(const char* sym) {
    for (int i = 0; i < kNumParams; ++i)
        if (std::strcmp(paramInfo(i).symbol, sym) == 0) return i;
    return -1;
}

} // namespace pipe
