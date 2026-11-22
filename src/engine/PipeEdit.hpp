// Pipe - physically modelled pipe organ
// Editing one pipe's dimensions (the Workshop): the pipe is rebuilt from its geometry with the same
// models the voicer used, so an edit changes what the physics sees, not a sound parameter.
//
// A flue pipe is rebuilt from the Pasi scale (designAt), matched to the pipe as voiced (flue, cut-up,
// mouth, languid, toe, foot pressure, body length as cut, the harmonic hole), its resonances found again
// from the body (Resonator.hpp), and the jet at the lip recomputed (specOf / FlueSim). A reed pipe keeps
// its tongue as voiced; tongue edits follow the cantilever (frequency ~ thickness / length^2), the
// resonator's modes are recomputed from its bore, and a length edit is the tuning scroll (as the voicer
// tunes). Before any edit is applied the rebuild of the unedited pipe must reproduce the voiced pipe
// (first resonance within 1 cent, jet transit within 1 %); a pipe that does not is not editable.
#pragma once

#include "FlueModel.hpp"
#include "ReedModel.hpp"
#include "OrganPipes.hpp"
#include "Opus14Organ.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace pipe {

enum PipeEditField {
    // flue
    kEdBodyLength = 0, kEdBodyDiameter, kEdMouthWidth, kEdCutup, kEdFlue, kEdLanguid, kEdFootPressure, kEdBeard,
    // reed
    kEdTongueThick, kEdTongueLength, kEdTongueWidth, kEdTongueCurve, kEdResLength, kEdResDiameter, kEdBootPressure,
    // both: the toe hole (the flue's foot, the reed's boot)
    kEdToe,
    kNumEditFields
};
// state text names, one per field
static const char* const kEditFieldIds[kNumEditFields] = {
    "length", "diameter", "mouth", "cutup", "flue", "languid", "foot", "beard",
    "tongue_t", "tongue_l", "tongue_w", "curve", "res_length", "res_diameter", "boot", "toe" };

// An edit is a ratio to the pipe as voiced (1 = unchanged), except the beard: its value in cut-ups
// (negative = unchanged).
struct PipeEdit {
    float v[kNumEditFields];
    PipeEdit() { for (int i = 0; i < kNumEditFields; ++i) v[i] = 1.f; v[kEdBeard] = -1.f; }
    bool any() const {
        for (int i = 0; i < kNumEditFields; ++i) if (i == kEdBeard ? v[i] >= 0.f : std::fabs(v[i] - 1.f) > 1e-6f) return true;
        return false;
    }
};

// "rank key field value" lines
inline std::map<int, PipeEdit> parsePipeEdits(const std::string& text) {
    std::map<int, PipeEdit> m;
    std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line); int r = -1, k = -1; std::string f; float val = 0;
        if (!(l >> r >> k >> f >> val) || r < 0 || r >= organ::kNumOrganRanks || k < 0 || k > 127) continue;
        for (int i = 0; i < kNumEditFields; ++i)
            if (f == kEditFieldIds[i]) m[r * 128 + k].v[i] = val;
    }
    return m;
}
inline std::string writePipeEdits(const std::map<int, PipeEdit>& m) {
    std::string out;
    for (const auto& e : m)
        for (int i = 0; i < kNumEditFields; ++i) {
            const float val = e.second.v[i];
            if (i == kEdBeard ? val < 0.f : std::fabs(val - 1.f) < 1e-6f) continue;
            char b[96]; std::snprintf(b, sizeof b, "%d %d %s %.6g\n", e.first / 128, e.first % 128, kEditFieldIds[i], (double)val);
            out += b;
        }
    return out;
}

namespace edit {
using namespace resonator;

// The Pasi stop a rank's pipes were built from, and the place on its scale for this key.
inline const opus14::Stop* stopFor(int rank) {
    static const opus14::Division div[4] = { opus14::kPed, opus14::kHW, opus14::kUW, opus14::kOW };   // engine order P, I, III, II
    const auto& rd = organ::kOrganRanks[rank];
    for (int i = 0; i < opus14::kNumStops; ++i) {
        const opus14::Stop& s = opus14::kStops[i];
        if (s.div == div[rd.division] && std::strcmp(s.name, rd.dataStop) == 0) return &s;
    }
    return nullptr;
}
inline double scaleKeyFor(const opus14::Stop& s, int key, double targetHz) {
    if (!std::strstr(s.name, "Mixture") && !std::strstr(s.name, "Sesquialtera")) return key;
    // a mixture's scales are printed for its base stop: the place on that scale with this pipe's pitch
    const double et8 = 440.0 * std::pow(2.0, (key - 69) / 12.0);   // the key at 8' pitch (12-TET, as the voicer cut them)
    const double feet = 8.0 * et8 / targetHz;
    const double base = std::strstr(s.note, "2 2/3") ? 2.6666666666666665 : 2.0;
    return key + 12.0 * std::log2(base / feet);
}

inline double centsBetween(double a, double b) { return 1200.0 * std::log2(a / b); }

// The voiced flue pipe's geometry, rebuilt. false (and why) if the rebuild does not reproduce it.
// Matched to the pipe as voiced: its flue, cut-up, mouth, languid, toe and wind, its body radius (the
// voicer may have widened the scale) and length as cut, and for a harmonic pipe the hole the voicer bored
// (found by its effect: the size whose resonances are the voiced pipe's).
inline bool flueDesign(int rank, int key, const FluePipeSpec& base, flue::Design& out, std::string& why) {
    const opus14::Stop* s = stopFor(rank);
    if (!s) { why = "no Pasi scale for this rank"; return false; }
    if (base.nModes < 1) { why = "no resonances"; return false; }
    flue::Voicing v;
    flue::Design d0 = flue::designAt(*s, scaleKeyFor(*s, key, base.target), base.target, v);
    d0.flue = base.h; d0.cutup = base.W; d0.g.cutup = base.W;
    d0.mouthWidth = base.Wm; d0.g.mouthWidth = base.Wm;
    d0.languid = base.languid; d0.toe = base.toe; d0.footPa = base.footPa; d0.valve = base.valve;
    if (d0.radius > 0 && std::fabs(base.radius / d0.radius - 1.0) > 1e-4) {
        const double rr = base.radius / d0.radius;
        for (auto& sg : d0.g.body) { sg.r1 *= rr; sg.r2 *= rr; }
        d0.g.chimneyR *= rr; d0.radius = base.radius;
    }
    auto build = [&](double hf, flue::Design& d) {
        d = d0;
        if (hf > 0) { const double L = d.g.body[0].L, dia = 2 * d.radius; d.g.holes = { { 0.5 * L, M_PI * 0.25 * (hf * dia) * (hf * dia), d.g.wall } }; }
        flue::recut(d, base.length / d.g.body[0].L);
        if (d.modes.empty()) return 1e9;
        double e = 0;
        const int n = std::min({ (int)d.modes.size(), base.nModes, d.speakMode == 2 ? 2 : 1 });
        for (int i = 0; i < n; ++i) e = std::max(e, std::fabs(centsBetween(d.modes[(size_t)i].f, base.f[i])));
        return e;
    };
    double bestErr = 1e9, bestH = 0;
    if (d0.speakMode != 2) { bestErr = build(0, out); }
    else {
        flue::Design d;
        for (double hf = 0.05; hf <= 0.5001; hf += 0.05) { const double e = build(hf, d); if (e < bestErr) { bestErr = e; bestH = hf; } }
        for (double hf = bestH - 0.045; hf <= bestH + 0.0451; hf += 0.005) if (hf > 0.0) { const double e = build(hf, d); if (e < bestErr) { bestErr = e; bestH = hf; } }
        for (double hf = bestH - 0.0045; hf <= bestH + 0.00451; hf += 0.0005) if (hf > 0.0) { const double e = build(hf, d); if (e < bestErr) { bestErr = e; bestH = hf; } }
        bestErr = build(bestH, out);
    }
    if (bestErr > 1.0) { char b[96]; std::snprintf(b, sizeof b, "rebuilt resonance %.1f cents from the voiced pipe", bestErr); why = b; return false; }
    return true;
}

inline flue::JetParams jetOf(const FluePipeSpec& b) {
    flue::JetParams jp;
    jp.y0b = b.y0b; jp.lpk = b.lpk; jp.gain = -(double)b.gscale; jp.kd = b.kd; jp.noise = b.noise; jp.sign = b.sign; jp.vortex = b.vortex; jp.beard = b.beard;
    return jp;
}

// The flue pipe with the edit applied.
inline bool editFlue(int rank, int key, const FluePipeSpec& base, const PipeEdit& e, FluePipeSpec& out, std::string& why) {
    flue::Design d;
    if (!flueDesign(rank, key, base, d, why)) return false;
    // check the jet as well: the rebuilt pipe unedited
    {
        const FluePipeSpec same = flue::specOf(d, jetOf(base));
        if (std::fabs(same.transit / base.transit - 1.0) > 0.01) { why = "rebuilt jet differs from the voiced pipe"; return false; }
    }
    const double dr = e.v[kEdBodyDiameter];
    if (std::fabs(dr - 1.0) > 1e-9) {
        for (auto& sg : d.g.body) { sg.r1 *= dr; sg.r2 *= dr; }
        d.g.chimneyR *= dr; d.radius *= dr;
        for (auto& h : d.g.holes) h.area *= dr * dr;
    }
    d.mouthWidth *= e.v[kEdMouthWidth]; d.g.mouthWidth = d.mouthWidth;
    d.cutup *= e.v[kEdCutup]; d.g.cutup = d.cutup;
    d.flue *= e.v[kEdFlue];
    d.languid *= e.v[kEdLanguid];
    d.footPa *= e.v[kEdFootPressure];
    if (d.toe > 0) d.toe *= e.v[kEdToe];   // an open foot (no toe hole) has nothing to bore
    flue::recut(d, e.v[kEdBodyLength]);   // also finds the modes again after a diameter change
    flue::JetParams jp = jetOf(base);
    if (e.v[kEdBeard] >= 0.f) jp.beard = e.v[kEdBeard];
    out = flue::specOf(d, jp);
    out.target = base.target; out.trim = base.trim; out.valve = base.valve;
    return true;
}

// Reed: the resonator's bore as built, and the scroll the voicer tuned it with.
inline bool editReed(int rank, int key, const ReedPipeSpec& base, const PipeEdit& e, ReedPipeSpec& out, std::string& why) {
    const opus14::Stop* s = stopFor(rank);
    if (!s) { why = "no Pasi scale for this rank"; return false; }
    std::vector<Segment> g = reed::resonatorOf(*s, key);
    if (g.empty()) { why = "no resonator on the scale"; return false; }
    double len = 0; for (auto& x : g) len += x.L;
    const double scroll = base.length / len;        // the voicer's scroll: modes / scroll
    auto modesOf = [&](const std::vector<Segment>& gg, int& n, float* f, float* Q, float* pr, float* pi, float* tr, float* ti) {
        Air air; air.tempC = 20.0;
        const double fTop = std::min(8000.0, 0.45 * 48000.0);
        auto modes = findModes([&](double w) { return reed::inputImpedance(w, gg, air); }, std::max(20.0, base.target * 0.3), std::max(fTop, 3.0 * base.target), kReedMaxModes, 6000);
        size_t k = 0; while (k < modes.size() && modes[k].Q >= 2.0) ++k;
        n = (int)std::min<size_t>(k, kReedMaxModes);
        for (int i = 0; i < n; ++i) {
            f[i] = (float)modes[(size_t)i].f; Q[i] = (float)modes[(size_t)i].Q;
            pr[i] = (float)modes[(size_t)i].peak.real(); pi[i] = (float)modes[(size_t)i].peak.imag();
            const cd T = reed::topFlowRatio(2 * M_PI * modes[(size_t)i].f, gg, air) / modes[(size_t)i].peak;
            tr[i] = (float)T.real(); ti[i] = (float)T.imag();
        }
    };
    // the unedited rebuild must give the voiced resonator
    {
        ReedPipeSpec t = base; modesOf(g, t.nModes, t.f, t.Q, t.pkRe, t.pkIm, t.topRe, t.topIm);
        if (t.nModes < 1 || base.nModes < 1) { why = "no resonances"; return false; }
        const double err = std::fabs(centsBetween(t.f[0] / scroll, base.f[0]));
        if (err > 1.0) { char b[96]; std::snprintf(b, sizeof b, "rebuilt resonator %.1f cents from the voiced pipe", err); why = b; return false; }
    }
    out = base;
    const double dr = e.v[kEdResDiameter];
    if (std::fabs(dr - 1.0) > 1e-9) { for (auto& x : g) { x.r1 *= dr; x.r2 *= dr; } out.lowerR = (float)(base.lowerR * dr); out.upperR = (float)(base.upperR * dr); }
    modesOf(g, out.nModes, out.f, out.Q, out.pkRe, out.pkIm, out.topRe, out.topIm);
    const double sc = scroll * e.v[kEdResLength];
    for (int i = 0; i < out.nModes; ++i) out.f[i] = (float)(out.f[i] / sc);
    out.length = (float)(base.length * e.v[kEdResLength]);
    // tongue: a cantilever, frequency ~ t / L^2
    const double rt = e.v[kEdTongueThick], rl = e.v[kEdTongueLength];
    out.t = (float)(base.t * rt); out.L = (float)(base.L * rl);
    out.fr = (float)(base.fr * rt / (rl * rl));
    out.w = (float)(base.w * e.v[kEdTongueWidth]);
    out.x0 = (float)(base.x0 * e.v[kEdTongueCurve]); out.xs = (float)(base.xs * e.v[kEdTongueCurve]);
    out.footPa = (float)(base.footPa * e.v[kEdBootPressure]);
    // the toe hole: an open boot (0) narrowed is first given the hole that passes what the open boot does
    // (ten times the tongue's largest opening, so the tongue, not the toe, limits the flow)
    if (std::fabs(e.v[kEdToe] - 1.f) > 1e-6f) {
        const double open = std::sqrt(4.0 * 10.0 * base.w * base.x0 / M_PI);
        out.toe = (float)((base.toe > 0 ? base.toe : open) * e.v[kEdToe]);
    }
    out.passR = (float)reedPassiveR(out);
    return true;
}

} // namespace edit

// The edited pipes: specs rebuilt off the audio thread, looked up by the voices.
struct PipeEditSet {
    std::map<int, FluePipeSpec> flue;
    std::map<int, ReedPipeSpec> reed;
    std::map<int, std::string> refused;   // pipes that could not be rebuilt, and why
    const FluePipeSpec* flueAt(int rank, int key) const { auto it = flue.find(rank * 128 + key); return it == flue.end() ? nullptr : &it->second; }
    const ReedPipeSpec* reedAt(int rank, int key) const { auto it = reed.find(rank * 128 + key); return it == reed.end() ? nullptr : &it->second; }
};

inline std::unique_ptr<PipeEditSet> buildPipeEdits(const std::string& text) {
    auto set = std::make_unique<PipeEditSet>();
    for (const auto& e : parsePipeEdits(text)) {
        if (!e.second.any()) continue;
        const int rank = e.first / 128, key = e.first % 128;
        std::string why;
        if (const FluePipeSpec* b = organFlueSpec(rank, key)) {
            FluePipeSpec s; if (edit::editFlue(rank, key, *b, e.second, s, why)) set->flue[e.first] = s; else set->refused[e.first] = why;
        } else if (const ReedPipeSpec* b = organReedSpec(rank, key)) {
            ReedPipeSpec s; if (edit::editReed(rank, key, *b, e.second, s, why)) set->reed[e.first] = s; else set->refused[e.first] = why;
        }
    }
    return set;
}

} // namespace pipe
