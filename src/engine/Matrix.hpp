// Pipe - physically modelled pipe organ
// Expression modulation matrix: what a controller sends -> which buildable
// mechanism it moves. Shared by the DSP (evaluation) and the UI (editing).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

namespace pipe {

enum ModSource { kSrcStrike = 0, kSrcPressure, kSrcPolyAT, kSrcCC74, kSrcBend, kSrcRelease, kSrcCC, kNumModSources };
static const char* const kModSourceLabels[kNumModSources] = { "STRIKE", "PRESSURE", "POLY AT", "CC74", "BEND", "RELEASE VEL", "CC" };

enum ModDest { kDstPallet = 0, kDstBoost, kDstJet, kDstPitch, kDstChiff, kDstOpenSpeed, kDstCloseSpeed,
               kDstSwell, kDstTremDepth, kDstTremSpeed, kNumModDests };
static const char* const kModDestLabels[kNumModDests] = { "PALLET OPEN", "WIND BOOST", "JET AIM", "PITCH", "CHIFF",
    "PALLET SPEED", "CLOSE SPEED", "SWELL BOX", "TREM DEPTH", "TREM SPEED" };
inline bool modDestGlobal(int d) { return d >= kDstSwell; }

enum ModCurve { kCurveLin = 0, kCurveSoft, kCurveHard, kCurveS, kNumCurves };
static const char* const kModCurveLabels[kNumCurves] = { "LIN", "SOFT", "HARD", "S" };
enum ModZone { kZoneLo = 0, kZoneHi, kZoneAll, kNumZones };
static const char* const kModZoneLabels[kNumZones] = { "LO", "HI", "ALL" };

constexpr int kNumModRows = 10;

struct ModRow {
    bool on = false;
    int src = kSrcPressure;
    int cc = 1;             // controller number when src == kSrcCC
    int zone = kZoneAll;
    float lo = 0.f, hi = 1.f;   // part of the source's travel that is used
    bool restMid = false;   // source rests at its middle (64) instead of 0
    int curve = kCurveLin;
    float smoothMs = 0.f;
    float amount = 1.f;     // -1 .. +1
    int dest = kDstPallet;

    // Source value in 0..1 (bend: -1..1 mapped to 0..1) -> output -amount..+amount
    float eval(float x) const {
        float u, sgn = 1.f;
        if (restMid) {
            const float c = (x - 0.5f) * 2.f;
            sgn = c < 0.f ? -1.f : 1.f;
            u = std::fabs(c);
        } else u = x;
        const float span = std::max(1e-4f, hi - lo);
        u = std::max(0.f, std::min(1.f, (u - lo) / span));
        switch (curve) {
            case kCurveSoft: u = std::sqrt(u); break;
            case kCurveHard: u = u * u; break;
            case kCurveS: u = u * u * (3.f - 2.f * u); break;
            default: break;
        }
        return sgn * u * amount;
    }
};

struct ModMatrix {
    ModRow rows[kNumModRows];

    void setDefaults() {
        for (auto& r : rows) r = ModRow();
        auto set = [&](int i, bool on, int src, int zone, float lo, float hi, bool mid, int curve, float smooth, float amt, int dest, int cc = 1) {
            ModRow& r = rows[i];
            r.on = on; r.src = src; r.zone = zone; r.lo = lo; r.hi = hi; r.restMid = mid;
            r.curve = curve; r.smoothMs = smooth; r.amount = amt; r.dest = dest; r.cc = cc;
        };
        set(0, true, kSrcPressure, kZoneAll, 0.f, 0.9f, false, kCurveLin, 5.f, 1.f, kDstPallet);
        set(1, true, kSrcCC74, kZoneLo, 0.f, 1.f, false, kCurveSoft, 10.f, 1.f, kDstBoost);
        set(2, true, kSrcPolyAT, kZoneAll, 0.f, 1.f, false, kCurveSoft, 10.f, 1.f, kDstBoost);
        set(3, true, kSrcStrike, kZoneAll, 0.f, 1.f, false, kCurveLin, 0.f, 0.6f, kDstChiff);
        set(4, true, kSrcStrike, kZoneAll, 0.f, 1.f, false, kCurveLin, 0.f, 1.f, kDstOpenSpeed);
        set(5, true, kSrcBend, kZoneAll, 0.f, 1.f, true, kCurveLin, 0.f, 1.f, kDstPitch);
        set(6, true, kSrcRelease, kZoneAll, 0.f, 1.f, false, kCurveLin, 0.f, 1.f, kDstCloseSpeed);
        set(7, true, kSrcCC74, kZoneHi, 0.f, 1.f, true, kCurveS, 20.f, 0.8f, kDstJet);
        set(8, true, kSrcCC, kZoneAll, 0.f, 1.f, false, kCurveLin, 30.f, 1.f, kDstSwell, 11);
        set(9, false, kSrcCC, kZoneAll, 0.f, 1.f, false, kCurveLin, 50.f, 1.f, kDstTremDepth, 1);
    }

    std::string serialize() const {
        std::ostringstream o;
        o.precision(5);
        for (int i = 0; i < kNumModRows; ++i) {
            const ModRow& r = rows[i];
            o << "R " << i << " " << (r.on ? 1 : 0) << " " << r.src << " " << r.cc << " " << r.zone << " " << r.lo << " " << r.hi
              << " " << (r.restMid ? 1 : 0) << " " << r.curve << " " << r.smoothMs << " " << r.amount << " " << r.dest << "\n";
        }
        return o.str();
    }

    // Empty text keeps the defaults.
    void parse(const std::string& text) {
        setDefaults();
        if (text.empty()) return;
        for (auto& r : rows) r = ModRow();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream l(line);
            std::string tag; int i, on, mid;
            ModRow r;
            if (!(l >> tag >> i) || tag != "R" || i < 0 || i >= kNumModRows) continue;
            if (!(l >> on >> r.src >> r.cc >> r.zone >> r.lo >> r.hi >> mid >> r.curve >> r.smoothMs >> r.amount >> r.dest)) continue;
            r.on = on != 0; r.restMid = mid != 0;
            r.src = std::max(0, std::min(kNumModSources - 1, r.src));
            r.dest = std::max(0, std::min(kNumModDests - 1, r.dest));
            r.zone = std::max(0, std::min(kNumZones - 1, r.zone));
            r.curve = std::max(0, std::min(kNumCurves - 1, r.curve));
            r.cc = std::max(0, std::min(127, r.cc));
            r.amount = std::max(-1.f, std::min(1.f, r.amount));
            r.lo = std::max(0.f, std::min(1.f, r.lo)); r.hi = std::max(0.f, std::min(1.f, r.hi));
            rows[i] = r;
        }
    }

    static std::string sourceLabel(const ModRow& r) {
        if (r.src == kSrcCC) {
            char b[16];
            std::snprintf(b, sizeof(b), "CC%d%s", r.cc, r.cc == 1 ? " MOD" : r.cc == 11 ? " EXPR" : r.cc == 2 ? " BREATH" : r.cc == 4 ? " FOOT" : "");
            return b;
        }
        return kModSourceLabels[r.src];
    }
};

} // namespace pipe
