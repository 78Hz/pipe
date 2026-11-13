// Pipe - physically modelled pipe organ
// Tuning: historical temperaments, equal divisions of the octave (EDO),
// Scala .scl/.kbm files, and the key-to-pipe mapping for each.
#pragma once

#include "Params.hpp"
#include <cmath>
#include <string>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstdio>

namespace pipe {

// A sounding key resolved to a pitch and a pipe identity.
struct KeyPitch {
    double hz = 0;        // frequency of the 8' pipe for this key
    int pipeIndex = -1;   // identity of the pipe position (for per-pipe tuning), 0..1023
    bool valid = false;
    float keyFloat = 0;   // 12-TET-equivalent key (for compass checks)
    char name[12] = {0};   // note name in the tuning's spelling
};

struct ScalaScale {
    std::vector<double> cents;    // degrees 1..n in cents (last = period)
    std::string description;
    bool valid() const { return !cents.empty(); }
};
struct ScalaKbm {
    int size = 0, first = 0, last = 127, middle = 60, refNote = 69;
    double refFreq = 440.0;
    int octaveDegree = 0;
    std::vector<int> map;         // -1 = unmapped
    bool valid = false;
};

inline bool parseScl(const std::string& text, ScalaScale& out, std::string* err = nullptr) {
    std::istringstream in(text);
    std::string line;
    int stage = 0, count = 0;
    out = ScalaScale();
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '!') continue;
        if (stage == 0) { out.description = line; stage = 1; continue; }
        if (stage == 1) { count = std::atoi(line.c_str()); stage = 2; continue; }
        // pitch line
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        std::string tok = line.substr(p);
        tok = tok.substr(0, tok.find_first_of(" \t"));
        double c;
        if (tok.find('.') != std::string::npos) c = std::atof(tok.c_str());
        else {
            size_t sl = tok.find('/');
            double num = std::atof(tok.substr(0, sl).c_str());
            double den = sl == std::string::npos ? 1.0 : std::atof(tok.substr(sl + 1).c_str());
            if (num <= 0 || den <= 0) { if (err) *err = "bad ratio"; return false; }
            c = 1200.0 * std::log2(num / den);
        }
        out.cents.push_back(c);
        if ((int)out.cents.size() == count) break;
    }
    if (count <= 0 || (int)out.cents.size() != count) { if (err) *err = "wrong number of pitches"; out.cents.clear(); return false; }
    return true;
}

inline bool parseKbm(const std::string& text, ScalaKbm& out) {
    std::istringstream in(text);
    std::string line;
    std::vector<std::string> vals;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] == '!') continue;
        vals.push_back(line.substr(p));
    }
    if (vals.size() < 7) return false;
    out.size = std::atoi(vals[0].c_str());
    out.first = std::atoi(vals[1].c_str());
    out.last = std::atoi(vals[2].c_str());
    out.middle = std::atoi(vals[3].c_str());
    out.refNote = std::atoi(vals[4].c_str());
    out.refFreq = std::atof(vals[5].c_str());
    out.octaveDegree = std::atoi(vals[6].c_str());
    out.map.clear();
    for (int i = 0; i < out.size && 7 + i < (int)vals.size(); ++i)
        out.map.push_back(vals[7 + i][0] == 'x' || vals[7 + i][0] == 'X' ? -1 : std::atoi(vals[7 + i].c_str()));
    out.valid = out.refFreq > 0;
    return out.valid;
}

class Tuning {
public:
    int system = kTun31;
    int customN = 31;
    int anchor = 60;            // SEQUENTIAL (and linear Scala): the key that plays C4
    double aRef = 440.0;

    ScalaScale scl;
    ScalaKbm kbm;

    // offsets from 12-TET in cents, C..B
    static const double* temperament(int sys) {
        static const double et[12]     = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        static const double werck[12]  = { 0, -9.8, -7.8, -5.9, -9.8, -2.0, -11.7, -3.9, -7.8, -11.7, -3.9, -7.8 };
        static const double mean[12]   = { 10.3, -13.7, 3.4, 20.5, -3.4, 13.7, -10.3, 6.8, -17.1, 0.0, 17.1, -6.8 };
        static const double kirn[12]   = { 0, -9.8, -6.8, -5.9, -13.7, -2.0, -11.7, -3.4, -7.8, -10.3, -3.9, -11.7 };
        static const double vall[12]   = { 5.9, 0.0, 2.0, 3.9, -2.0, 7.8, -2.0, 3.9, 2.0, 0.0, 5.9, -3.9 };
        switch (sys) {
            case kTunWerck: return werck;
            case kTunMeantone: return mean;
            case kTunKirn: return kirn;
            case kTunVallotti: return vall;
            default: return et;
        }
    }

    int edoN() const {
        switch (system) {
            case kTun19: return 19; case kTun22: return 22; case kTun24: return 24;
            case kTun31: return 31; case kTun41: return 41; case kTun53: return 53;
            case kTunCustomEdo: return std::max(5, std::min(72, customN));
            default: return 0;
        }
    }
    bool isEdo() const { return edoN() > 0; }

    // SUBSET picks the 12 keys' notes along the chain of fifths. That only gives
    // 12 different notes when the division's fifth generates them: in 5-, 10-,
    // 15-, 20-EDO and others, several keys would land on one pitch. Those
    // divisions are played SEQUENTIAL instead.
    static bool subsetUsable(int n) {
        if (n <= 0) return true;
        const int F = fifthSteps(n);
        bool seen[128] = {};
        int distinct = 0;
        for (int pc = 0; pc < 12; ++pc) { const int s = ((fifthPos(pc, false) * F) % n + n) % n; if (!seen[s]) { seen[s] = true; ++distinct; } }
        return distinct == 12;
    }
    // the map a zone actually plays with this tuning
    int effectiveMap(int mapMode) const {
        if (system == kTunScala) return mapMode;
        const int n = edoN();
        if (n <= 0) return mapMode;                         // 12-note temperaments: every map is the same
        if (mapMode == kMapSubset && subsetUsable(n)) return kMapSubset;
        return kMapSequential;                              // a .kbm only applies to a Scala scale
    }

    // Steps of the best fifth in n-EDO
    static int fifthSteps(int n) { return (int)std::lround(n * std::log2(1.5)); }

    // Position of each piano key on the chain of fifths (C=0). Default spelling
    // uses C# Eb F# G# Bb; the alternate spelling gives Db D# Gb Ab A#.
    static int fifthPos(int pc, bool alt) {
        static const int norm[12] = { 0, 7, 2, -3, 4, -1, 6, 1, 8, 3, -2, 5 };
        static const int flat[12] = { 0, -5, 2, 9, 4, -1, -6, 1, -4, 3, 10, 5 };
        return alt ? flat[pc] : norm[pc];
    }
    static const char* spelling(int pc, bool alt) {
        static const char* norm[12] = { "C", "C#", "D", "EB", "E", "F", "F#", "G", "G#", "A", "BB", "B" };
        static const char* flat[12] = { "C", "DB", "D", "D#", "E", "F", "GB", "G", "AB", "A", "A#", "B" };
        return alt ? flat[pc] : norm[pc];
    }

    // Frequency of C4 (MIDI 60) so that A4 lands on aRef in this tuning
    double c4() const {
        const int n = edoN();
        if (n > 0) {
            const int F = fifthSteps(n);
            int a = ((3 * F) % n + n) % n;
            return aRef / std::pow(2.0, (double)a / n);
        }
        if (system == kTunScala) return 261.6255653 * aRef / 440.0;
        const double* t = temperament(system);
        return aRef * std::pow(2.0, (-9.0 + (t[0] - t[9]) / 100.0) / 12.0);
    }

    KeyPitch keyPitch(int key, int mapMode, bool altSpelling) const {
        KeyPitch kp;
        const int n = edoN();
        const double C4 = c4();
        if (system == kTunScala && scl.valid()) return scalaPitch(key, mapMode);
        if (n > 0) {
            if (effectiveMap(mapMode) == kMapSequential) {
                int step = key - anchor;
                kp.hz = C4 * std::pow(2.0, (double)step / n);
                int oct = (int)std::floor((double)step / n);
                int s = step - oct * n;
                kp.pipeIndex = (oct + 5) * n + s;
                std::snprintf(kp.name, sizeof(kp.name), "%d", s);
            } else {
                const int F = fifthSteps(n);
                const int pc = ((key % 12) + 12) % 12;
                const int oct = key / 12 - 1;
                const int pos = fifthPos(pc, altSpelling);
                int s = ((pos * F) % n + n) % n;
                // keep the step in the same octave as the key (B# / Cb style wraps)
                double cents = 1200.0 * s / n;
                double expect = 100.0 * pc;
                int octAdj = 0;
                if (cents - expect > 600) octAdj = -1;
                else if (expect - cents > 600) octAdj = 1;
                kp.hz = C4 * std::pow(2.0, (oct - 4 + octAdj) + (double)s / n);
                kp.pipeIndex = (oct + 1 + octAdj) * n + s;
                std::snprintf(kp.name, sizeof(kp.name), "%s", spelling(pc, altSpelling));
            }
        } else {
            const double* t = temperament(system);
            const int pc = ((key % 12) + 12) % 12;
            kp.hz = aRef * std::pow(2.0, ((key - 69) + (t[pc] - t[9]) / 100.0) / 12.0);
            kp.pipeIndex = key;
            std::snprintf(kp.name, sizeof(kp.name), "%s", spelling(pc, false));
        }
        kp.keyFloat = (float)(69.0 + 12.0 * std::log2(kp.hz / 440.0));
        kp.pipeIndex = std::max(0, std::min(1023, kp.pipeIndex));
        kp.valid = kp.hz > 0;
        return kp;
    }

    // Number of distinct pipes a rank needs between two 8' frequencies
    int pipesInRange(double fLo, double fHi) const {
        const int n = edoN();
        if (n > 0) return (int)std::floor(n * std::log2(fHi / fLo) + 1e-6) + 1;
        if (system == kTunScala && scl.valid()) {
            const double period = scl.cents.back();
            return (int)std::floor(scl.cents.size() * 1200.0 * std::log2(fHi / fLo) / period + 1e-6) + 1;
        }
        return (int)std::lround(12.0 * std::log2(fHi / fLo)) + 1;
    }

    double stepCents() const {
        const int n = edoN();
        if (n > 0) return 1200.0 / n;
        if (system == kTunScala && scl.valid()) return scl.cents.back() / scl.cents.size();
        return 100.0;
    }

private:
    KeyPitch scalaPitch(int key, int mapMode) const {
        KeyPitch kp;
        const int deg = (int)scl.cents.size();
        const double period = scl.cents.back();
        auto degreeCents = [&](int d) {
            int oct = (int)std::floor((double)d / deg);
            int r = d - oct * deg;
            return oct * period + (r == 0 ? 0.0 : scl.cents[r - 1]);
        };
        if (mapMode == kMapKbm && kbm.valid && kbm.size >= 0) {
            if (key < kbm.first || key > kbm.last) return kp;
            int degree;
            if (kbm.size == 0) degree = key - kbm.middle;
            else {
                int rel = key - kbm.middle;
                int oct = (int)std::floor((double)rel / kbm.size);
                int idx = rel - oct * kbm.size;
                int m = idx < (int)kbm.map.size() ? kbm.map[idx] : -1;
                if (m < 0) return kp;
                int octDeg = kbm.octaveDegree > 0 ? kbm.octaveDegree : deg;
                degree = oct * octDeg + m;
            }
            // reference: refNote sounds refFreq
            int refDegree;
            {
                int rel = kbm.refNote - kbm.middle;
                if (kbm.size == 0) refDegree = rel;
                else {
                    int oct = (int)std::floor((double)rel / kbm.size);
                    int idx = rel - oct * kbm.size;
                    int m = idx < (int)kbm.map.size() ? std::max(0, kbm.map[idx]) : 0;
                    int octDeg = kbm.octaveDegree > 0 ? kbm.octaveDegree : deg;
                    refDegree = oct * octDeg + m;
                }
            }
            kp.hz = kbm.refFreq * std::pow(2.0, (degreeCents(degree) - degreeCents(refDegree)) / 1200.0);
            kp.pipeIndex = degree + 4 * deg + 64;
        } else {
            // linear: the anchor key = degree 0 at C4 scaled by A reference
            int degree = key - anchor;
            kp.hz = c4() * std::pow(2.0, degreeCents(degree) / 1200.0);
            kp.pipeIndex = degree + 4 * deg + 64;
        }
        std::snprintf(kp.name, sizeof(kp.name), "%d", ((kp.pipeIndex - 64) % deg + deg) % deg);
        kp.keyFloat = (float)(69.0 + 12.0 * std::log2(kp.hz / 440.0));
        kp.pipeIndex = std::max(0, std::min(1023, kp.pipeIndex));
        kp.valid = kp.hz > 0;
        return kp;
    }
};

} // namespace pipe
