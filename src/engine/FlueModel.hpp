// Pipe - physically modelled pipe organ
// Flue pipes of Pasi Opus 14: geometry from the builder's scales, voicing by
// the Ising rule, sound by the jet-drive model on the computed resonator.
//
// Design (offline, per pipe)
//   * Diameter, cut-up, mouth width, chimney and taper come from the Pasi
//     tables (Opus14Data.hpp), interpolated geometrically between the printed
//     notes (scales progress by a constant ratio within each octave).
//   * The body is cut to length: its length is solved so that the resonance the
//     pipe speaks on lies at the target frequency (Pasi: "pipes cut to exact
//     length"); the sounding frequency is then checked on the simulated sound.
//   * Flue width D comes from the Ising number I = sqrt(2 P D / (rho H^3)) / F
//     (Ising 1971): D = I^2 F^2 rho H^3 / (2 P), with P the foot pressure, H the
//     cut-up, F the sounding frequency. Target I per family is an ASSUMPTION
//     (no Pasi voicing data): 2.0 principals and flutes ("I = 2 gives maximum
//     efficiency"), 2.8 strings (more harmonics, still below overblowing at ~3).
//   * Foot pressure is the chest pressure (79 mm, Vogt ch. 3) for open toes - an
//     ASSUMPTION - and for the toe holes Pasi gives (Salicional, Gamba, Celeste)
//     follows from continuity through toe and flue: Pf = Pc / (1 + (Af/(Cd At))^2),
//     Cd = 0.61 (sharp-edged orifice).
//
// Sound (real time, per sample)  - the jet-drive model of Verge et al. (1997),
// Fabre & Hirschberg (2000), as used in the time-domain simulations of Auvray,
// Fabre & Lagree (2012):
//   Uj   = sqrt(2 Pf / rho)                                 jet speed
//   eta  = (h / Uj) G v(t - tau),  tau = W / cv, cv = 0.4 Uj    jet deflection at the labium
//   Qin  = Wm b Uj (1 + tanh((eta - y0) / b)),  b = 2h/5        flow of the Bickley jet into the pipe
//   dp   = -(rho dd / Sm) dQin/dt - (rho/2)(v/0.6)^2 sgn(v)     jet drive + vortex shedding loss
//          dd = (4/pi) sqrt(2 h W)
//   q    = Y(dp), Y = 1/(Zmouth + Zbody) as a sum of resonant modes
//   v    = q / Sm (acoustic velocity in the mouth)
// h = flue width, W = cut-up, Wm = mouth width, Sm = W Wm.
// Jet amplification G = exp(mu W) with mu = omega/cv (long-wave growth, the jet
// wave grows by about e per wavelength - Fletcher & Rossing 1998 sec. 16.3);
// evaluated at the sounding frequency. Jet offset y0 is a voicing ASSUMPTION.
// Radiation: monopoles at mouth and top, p(1 m) = rho/(4 pi) d/dt(Qtop - Qmouth),
// listener broadside and far; at the mouth the room also receives the part of the jet that
// does not enter the pipe (mass conservation in the mouth: outflow = Qj - Qin - q).
#pragma once

#include "Opus14Data.hpp"
#include "Resonator.hpp"
#include <cstring>
#include <string>

namespace pipe {
namespace flue {

using namespace resonator;

constexpr double kChestPa = 79.0 * 9.80665;   // Vogt ch. 3: voiced on 79 mm
constexpr double kCd = 0.61;

enum Family { kPrincipal = 0, kString, kFlute };
enum Shape { kOpenCyl = 0, kStopped, kChimney, kConical };

struct Voicing {
    double ising = 2.0;
    double y0OverB = 0.25;   // lip offset from the jet centre, in jet half-widths at the lip (ASSUMPTION)
};

struct Design {
    // geometry (m)
    FlueGeometry g;
    Shape shape = kOpenCyl;
    Family family = kPrincipal;
    int speakMode = 1;          // 2 for harmonic (overblowing) pipes
    double radius = 0;          // body (lower) radius
    double mouthWidth = 0, cutup = 0, flue = 0, toe = 0;   // toe = toe-hole diameter (0 = open)
    double languid = 0.002;     // languid thickness = length of the windway (m)
    double footPa = kChestPa;
    double target = 0;          // Hz
    double ising = 0;
    bool wood = false;
    // resonator
    std::vector<Mode> modes;
    std::vector<cd> topRatio;
    std::string note;           // what was assumed
};

// -- table access ----------------------------------------------------------
inline const opus14::Row* findRow(const opus14::Stop& s, const char* label) {
    for (int i = 0; i < s.nRows; ++i) if (!std::strcmp(s.rows[i].label, label)) return &s.rows[i];
    return nullptr;
}
// Geometric interpolation of a printed row at a key; alt = use the second value of
// a pair (rectangular depth). Outside the printed points, the nearest octave's ratio
// continues. Returns NaN if fewer than one valid point.
inline double interp(const opus14::Stop& s, const opus14::Row* r, int key, bool alt = false) {
    if (!r) return NAN;
    int idx[7], n = 0;
    for (int i = 0; i < s.nCols; ++i) {
        const float v = alt ? r->alt[i] : r->v[i];
        if (!std::isnan(v) && v > 0) idx[n++] = i;
    }
    if (n == 0) return NAN;
    auto val = [&](int i) { return (double)(alt ? r->alt[i] : r->v[i]); };
    if (n == 1) return val(idx[0]);
    int j = 0;
    while (j + 2 < n && s.key[idx[j + 1]] < key) ++j;
    const int a = idx[j], b = idx[j + 1];
    const double t = (double)(key - s.key[a]) / (s.key[b] - s.key[a]);
    return val(a) * std::pow(val(b) / val(a), t);
}
inline bool hasValueAt(const opus14::Stop& s, const opus14::Row* r, int key) {
    // the row has a printed value at or below this key
    if (!r) return false;
    for (int i = 0; i < s.nCols; ++i) if (s.key[i] <= key && !std::isnan(r->v[i])) return true;
    return false;
}

inline bool nameHas(const opus14::Stop& s, const char* w) { return std::strstr(s.name, w) != nullptr; }

inline Family familyOf(const opus14::Stop& s) {
    if (nameHas(s, "Salicional") || nameHas(s, "Gamba") || nameHas(s, "Celeste")) return kString;
    const char* flutes[] = { "flote", "Flute", "Gedeckt", "Bourdon", "Subbass", "Octavin", "Nazard", "Tierce" };
    for (auto f : flutes) if (nameHas(s, f)) return kFlute;
    return kPrincipal;
}

// Key from which a harmonic stop overblows (Pasi notes), or 999.
inline int overblowFrom(const opus14::Stop& s) {
    if (!std::strstr(s.note, "Overblowing from")) return 999;
    if (std::strstr(s.note, "from c2")) return 72;
    if (std::strstr(s.note, "from c1")) return 60;
    if (std::strstr(s.note, "from c0")) return 48;
    return 999;
}

// Sounding frequency: a1 = 440 Hz at 20 C (ASSUMPTION; Opus 14 pitch not published),
// in a temperament given as cents above C.
inline double soundingHz(int key, double feet, const double* centsFromC) {
    const int pc = ((key % 12) + 12) % 12, oct = key / 12 - 1;     // MIDI 60 = c1 (octave 4)
    const double cA = centsFromC[9];
    const double c4 = 440.0 / std::pow(2.0, cA / 1200.0);           // c1 so that a1 = 440
    const double f8 = c4 * std::pow(2.0, (oct - 4) + centsFromC[pc] / 1200.0);
    return f8 * 8.0 / feet;
}
constexpr double kWegscheider[12] = { 0.00, 93.55, 193.16, 297.13, 390.69, 499.04, 592.60, 696.58, 794.51, 889.74, 998.09, 1091.65 };
constexpr double kMeantone[12] = { 0.00, 76.05, 193.16, 310.26, 386.31, 503.42, 579.47, 696.58, 772.63, 889.74, 1006.84, 1082.89 };
constexpr double kEqual[12] = { 0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100 };

// -- resonance of a body of given length -------------------------------------
// Peak of the flue admittance near f (the mode the pipe speaks on).
inline double peakNear(const FlueGeometry& g, double f, const Air& air) {
    auto m = findModes([&](double w) { return flueAdmittance(w, g, air); }, f * 0.45, f * 1.6, 1, 1500);   // lowest resonance
    return m.empty() ? 0.0 : m[0].f;
}

// Build the body for length L.
struct BodySpec { Shape shape; double r1, r2ratio, chimR, chimL; };
inline void buildBody(FlueGeometry& g, const BodySpec& b, double L) {
    g.body.clear();
    g.body.push_back({ b.r1, b.shape == kConical ? b.r1 * b.r2ratio : b.r1, L });
    g.top = b.shape == kStopped ? Top::Stopped : b.shape == kChimney ? Top::Chimney : Top::Open;
    g.chimneyR = b.chimR; g.chimneyL = b.chimL;
}

// Set flue width and foot pressure for an Ising number I. Ising fixes D P = K. Through a
// toe hole, continuity gives P = Pc / (1 + (Af/(Cd At))^2) with Af = D Wm = K Wm / P, so
// P^2 - Pc P + c^2 = 0, c = K Wm / (Cd At); the larger root is the one that becomes Pc as
// the toe opens. Returns false if the toe cannot pass that much wind at any flue width.
inline bool applyIsing(Design& d, double I, double rho) {
    const double H = d.cutup, K = I * I * d.target * d.target * rho * H * H * H / 2.0;
    d.footPa = kChestPa;
    if (d.toe > 0) {
        const double At = M_PI * 0.25 * d.toe * d.toe, cc = K * d.mouthWidth / (kCd * At);
        const double disc = kChestPa * kChestPa - 4 * cc * cc;
        if (disc < 0) return false;
        d.footPa = 0.5 * (kChestPa + std::sqrt(disc));
    }
    d.ising = I;
    d.flue = K / d.footPa;
    return true;
}
// The highest Ising number a toe hole allows (flue area = Cd At, foot at half the chest).
inline double maxIsing(const Design& d, double rho) {
    if (d.toe <= 0) return 1e9;
    const double At = M_PI * 0.25 * d.toe * d.toe, H = d.cutup;
    const double Kmax = 0.5 * kChestPa * kCd * At / d.mouthWidth;
    return std::sqrt(2.0 * Kmax / (d.target * d.target * rho * H * H * H));
}

// Keep the resonances that stand as resonances: above some frequency the mouth radiates so
// strongly that the admittance no longer has distinct peaks (Q below 5); the modal sum stops there.
inline void keepResonances(std::vector<Mode>& m) {
    size_t n = 0; while (n < m.size() && m[n].Q >= 5.0) ++n;
    m.resize(n);
}

inline Design design(const opus14::Stop& s, int key, const Voicing& v, const double* temperament = kWegscheider,
                     double tempC = 20.0, double fs = 48000.0) {
    Design d;
    Air air; air.tempC = tempC;
    d.family = familyOf(s);
    d.target = soundingHz(key, s.feet, temperament);
    const int ob = overblowFrom(s);
    d.speakMode = key >= ob ? 2 : 1;

    // diameters
    const opus14::Row* dia = findRow(s, "Diameter");
    const opus14::Row* lower = findRow(s, "Lower Diameter");
    const opus14::Row* upper = findRow(s, "Upper Diameter");
    BodySpec b{ kOpenCyl, 0, 1, 0, 0 };
    double mouthW = 0;
    if (dia) {
        const double dmm = interp(s, dia, key);
        const double depth = interp(s, dia, key, true);
        // A rectangular (wood) pipe printed "width x depth" at this key: use it while the
        // key is at or below the last rectangular point.
        bool rect = false;
        for (int i = 0; i < s.nCols; ++i) if (!std::isnan(dia->alt[i]) && s.key[i] >= key) rect = true;
        if (rect && !std::isnan(depth)) {
            d.wood = true;
            b.r1 = eqRadius(dmm * depth * 1e-6);
            mouthW = dmm * 1e-3;
        } else {
            b.r1 = 0.5 * dmm * 1e-3;
        }
    } else if (lower && upper) {
        b.shape = kConical;
        const double lo = interp(s, lower, key), up = interp(s, upper, key);
        b.r1 = 0.5 * lo * 1e-3; b.r2ratio = up / lo;
    }
    d.radius = b.r1;
    // top
    const bool stopped = nameHas(s, "Gedeckt") || nameHas(s, "Bourdon") || nameHas(s, "Subbass");
    const opus14::Row* rohrD = findRow(s, "Rohr Diameter");
    const opus14::Row* rohrL = findRow(s, "Rohr Length");
    if (b.shape != kConical) {
        if (stopped) b.shape = kStopped;
        if (rohrD) {
            if (hasValueAt(s, rohrD, key)) {
                b.shape = kChimney;
                b.chimR = 0.5e-3 * interp(s, rohrD, key);
                b.chimL = 1e-3 * interp(s, rohrL, key);
            } else {
                b.shape = kStopped;
                d.note += "rohr not printed below its first note: stopped without chimney; ";
            }
        }
    }
    // OW Rohrflote 4': "Tapered open from c#3 as in HW 4'" (HW Spitzflote 4')
    if (s.div == opus14::kOW && nameHas(s, "Rohrflote 4'") && key >= 85) {
        for (int i = 0; i < opus14::kNumStops; ++i) {
            const auto& o = opus14::kStops[i];
            if (o.div == opus14::kHW && !std::strcmp(o.name, "Spitzflote 4'")) {
                const double lo = interp(o, findRow(o, "Lower Diameter"), key), up = interp(o, findRow(o, "Upper Diameter"), key);
                b.shape = kConical; b.r2ratio = up / lo;
            }
        }
        d.note += "open tapered from c#3 with HW Spitzflote taper; ";
    }
    d.shape = b.shape;

    // mouth
    const opus14::Row* mw = findRow(s, "Mouth Width");
    if (mw) {
        // value at this key: fractions (<= 1) of the circumference, else mm
        int col = 0; for (int i = 0; i < s.nCols; ++i) if (s.key[i] <= key) col = i;
        const double raw = mw->v[col];
        if (!std::isnan(raw) && raw > 1.0) mouthW = raw * 1e-3;
        else if (mouthW == 0) mouthW = (std::isnan(raw) ? 0.25 : raw) * 2.0 * M_PI * b.r1;
    }
    if (mouthW == 0) { mouthW = 0.25 * 2.0 * M_PI * b.r1; d.note += "mouth width 1/4 circumference; "; }
    d.mouthWidth = mouthW;
    d.cutup = 1e-3 * interp(s, findRow(s, "Cut-up"), key);
    {
        const double lt = interp(s, findRow(s, "Languid Thickness"), key);
        if (!std::isnan(lt)) d.languid = 1e-3 * lt;
        else {
            const double wt = interp(s, findRow(s, "Wall Thickness"), key);
            d.languid = std::isnan(wt) ? 0.002 : 1e-3 * wt;
            d.note += "wood: windway length taken as the wall thickness; ";
        }
    }
    d.g.mouthWidth = mouthW; d.g.cutup = d.cutup;
    d.g.wall = d.wood ? 0.003 : 0.0005;   // ASSUMPTION: lip thickness (bevelled wood lip, metal sheet)

    // body length: start from the plain-tube estimate, then solve for the resonance
    const double c = air.c();
    double fRes = d.target / d.speakMode;     // speak on mode speakMode: its first resonance is the octave below
    double L = (b.shape == kStopped || b.shape == kChimney) ? c / (4 * fRes) : c / (2 * fRes);
    if (d.speakMode == 2) fRes = d.target;    // solve the second resonance directly
    auto resonanceAt = [&](double len) {
        buildBody(d.g, b, len);
        if (d.speakMode == 1) return peakNear(d.g, d.target, air);
        auto m = findModes([&](double w) { return flueAdmittance(w, d.g, air); }, d.target * 0.3, d.target * 1.3, 3, 1500);
        return m.size() >= 2 ? m[1].f : 0.0;
    };
    for (int it = 0; it < 8; ++it) {
        const double f = resonanceAt(L);
        if (f <= 0) break;
        const double ratio = f / d.target;
        if (std::fabs(ratio - 1) < 1e-5) break;
        L *= std::pow(ratio, 1.1);
    }
    buildBody(d.g, b, L);

    // toe and flue
    const double rho = air.rho();
    const opus14::Row* toe = findRow(s, "Toe Hole");
    const double toeMm = toe ? interp(s, toe, key) : NAN;
    d.toe = std::isnan(toeMm) ? 0.0 : toeMm * 1e-3;
    if (!applyIsing(d, v.ising, rho)) applyIsing(d, maxIsing(d, rho) * 0.999, rho);
    if (std::isnan(toeMm)) d.note += "open toe (foot at chest pressure); ";

    // modes of the admittance the jet sees
    const double fTop = std::min(14000.0, 0.45 * fs);
    d.modes = findModes([&](double w) { return flueAdmittance(w, d.g, air); }, d.target / d.speakMode * 0.5, fTop, 40, 8000);
    keepResonances(d.modes);
    d.topRatio.clear();
    for (auto& m : d.modes) d.topRatio.push_back(flueTopFlowRatio(2 * M_PI * m.f, d.g, air));
    return d;
}

// Recompute the modes after the body has been recut (length scaled by `ratio`).
inline void recut(Design& d, double ratio, double tempC = 20.0, double fs = 48000.0) {
    Air air; air.tempC = tempC;
    d.g.body[0].L *= ratio;
    const double fTop = std::min(14000.0, 0.45 * fs);
    d.modes = findModes([&](double w) { return flueAdmittance(w, d.g, air); }, d.target / d.speakMode * 0.5, fTop, 40, 8000);
    keepResonances(d.modes);
    d.topRatio.clear();
    for (auto& m : d.modes) d.topRatio.push_back(flueTopFlowRatio(2 * M_PI * m.f, d.g, air));
}

// -- the sounding pipe --------------------------------------------------------
struct FlueSim {
    struct M { double b0, b1, b2, a1, a2, z1, z2, reR, imR, w, qPrev; };
    std::vector<M> modes;
    std::vector<double> xiHist;
    double xi = 0;
    int histPos = 0;
    double fs = 48000, rho = 1.2, h = 0, W = 0, Wm = 0, Sm = 0, dd = 0, b = 0, y0 = 0, omega = 0;
    double qinPrev = 0, vPrev = 0, sPrev = 0;
    double transit = 0, lipSpeed = 1;   // transit = jet-wave travel time x V0 (m); lip speed / V0
    double foot = 0;

    struct LP { double b0, b1, b2, a1, a2, z1, z2; } lp{}, lpj{};
    double jetFlowRadiates = 1.0;
    double growthMax = 50.0;
    double etaLP = 0;
    double windR = 0;   // 12 mu l / h^2
    int os = 1;   // internal oversampling, so that the jet delay and the loop stay well resolved
    void init(const Design& d, const Voicing& v, double sampleRate, double tempC = 20.0) {
        Air air; air.tempC = tempC;
        os = std::max(1, (int)std::ceil(d.target * 24.0 / sampleRate));
        fs = sampleRate * os; rho = air.rho();
        c2rho = rho * air.c() * air.c(); mu = air.eta();
        toeArea = d.toe > 0 ? M_PI * 0.25 * d.toe * d.toe : 0.0;
        windR = 12.0 * mu * d.languid / (d.flue * d.flue);
        {
            const double D = 2.0 * d.radius, Lf = std::max(0.150, 2.5 * D);
            pallet.volume = 0.3e-3 + M_PI / 12.0 * D * D * Lf;
        }
        footP = 0; palletPhase = 0; palletPos = 0; keyDown = false; chest = kChestPa; etaLP = 0;
        h = d.flue; W = d.cutup; Wm = d.mouthWidth; Sm = W * Wm;
        dd = 4.0 / M_PI * std::sqrt(2.0 * h * W);
        // Turbulent plane jet (organ-pipe jets are turbulent: Thwaites & Fletcher 1980). Centre-line
        // speed V(x) = V0 for x < x0 = 6.1 h (potential core), V0 * 2.47 sqrt(h/x) beyond
        // (Rajaratnam 1976: Um/U0 = 3.5 (x/b0)^-1/2, b0 = h/2); self-similar sech^2 profile with
        // half-velocity half-width 0.10 x (Goertler profile, spreading rate S = 0.10, Pope 2000 ch. 5).
        // Jet waves travel at u = V(x)/2 (Thwaites & Fletcher 1980, u/V = 0.5 independent of frequency)
        // and grow at mu = 2 omega / V (Fletcher 1979, short-wave limit), so growth = omega * transit.
        {
            const double x0 = 6.1 * h;
            if (W <= x0) { transit = 2.0 * W; lipSpeed = 1.0; b = 0.4 * h; }
            else {
                transit = 2.0 * (x0 + (2.0 / 3.0) * (std::pow(W, 1.5) - std::pow(x0, 1.5)) / (2.47 * std::sqrt(h)));
                lipSpeed = 2.47 * std::sqrt(h / W);
                b = std::max(0.4 * h, 0.10 * W / 0.8814);          // sech^2 half-value at y = 0.8814 b
            }
        }
        y0 = v.y0OverB * b;
        growthMax = omega * transit / std::max(1e-3, jetSpeed(d.footPa));
        omega = 2 * M_PI * d.target;
        modes.clear();
        const double T = 1.0 / fs;
        for (size_t i = 0; i < d.modes.size(); ++i) {
            const Mode& m = d.modes[i];
            const double wn = 2 * M_PI * m.f, Q = m.Q;
            // Y_n(s) = (A s + B) / (s^2 + s wn/Q + wn^2), matching the complex admittance at the peak
            const double A = m.peak.real() * wn / Q, B = -m.peak.imag() * wn * wn / Q;
            // bilinear transform, prewarped at wn
            const double K = wn / std::tan(wn * T / 2);
            const double a0 = K * K + K * wn / Q + wn * wn;
            M x{};
            x.b0 = (A * K + B) / a0; x.b1 = 2 * B / a0; x.b2 = (-A * K + B) / a0;
            x.a1 = 2 * (wn * wn - K * K) / a0; x.a2 = (K * K - K * wn / Q + wn * wn) / a0;
            x.reR = d.topRatio[i].real(); x.imR = d.topRatio[i].imag(); x.w = wn;
            modes.push_back(x);
        }
        xiHist.assign(8192 * (size_t)os, 0.0); histPos = 0; xi = 0;
        {
            const double fc = std::min(12000.0, 0.4 * fs), K = std::tan(M_PI * fc / fs), q = 1.0 / std::sqrt(2.0);
            const double nrm = 1.0 / (1.0 + K / q + K * K);
            lp = { K * K * nrm, 2 * K * K * nrm, K * K * nrm, 2 * (K * K - 1) * nrm, (1 - K / q + K * K) * nrm, 0, 0 };
            lpj = lp;
        }
        qinPrev = vPrev = sPrev = 0;
    }

    // One sample; footPa = pressure in the foot now. Returns radiated pressure at 1 m (Pa).
    double tick(double footPa) {
        double acc = 0;
        for (int k = 0; k < os; ++k) acc += sub(footPa);
        return acc / os;
    }

    // -- the wind on its way to the flue ----------------------------------------
    // Chest -> pallet -> groove and pipe foot (one volume) -> flue. The pallet opens and
    // closes with a smooth motion (finite acceleration of the action). Flow through the
    // pallet gap and the toe hole: inertial orifices (Cd = 0.61) in series with viscous slot
    // flow through the pallet gap (Poiseuille between parallel plates, 12 mu l Q / (w g^3)),
    // which takes over as the gap closes, so the flow ends smoothly. Out through the flue at
    // the jet speed. The volume stores air by compressibility: dP/dt = rho c^2 / V (Qin - Qout),
    // solved implicitly each step.
    // ASSUMPTIONS (not on the Pasi drawings): pallet 150 mm long, opening 8 mm, seal 10 mm;
    // pallet travel 12 ms each way; volume = a 0.3 litre share of the groove plus the foot, a
    // cone as wide as the pipe and 2.5 diameters long (at least 150 mm). The volume matters:
    // it is what keeps a large pipe's wind from vanishing in a millisecond when the pallet shuts.
    struct Pallet { double w = 0.150, gMax = 0.008, seal = 0.010, tOpen = 0.012, tClose = 0.012, volume = 1.0e-4; } pallet;
    double footP = 0, chest = kChestPa, palletPos = 0, mu = 1.81e-5, c2rho = 1.4e5, toeArea = 0;
    bool keyDown = false;
    void key(bool down, double chestPa) { keyDown = down; chest = chestPa; }
    // Jet speed from the foot pressure: acceleration (Bernoulli) in series with laminar friction
    // in the windway (plane Poiseuille flow over the languid's thickness l):
    // P = rho U^2 / 2 + (12 mu l / h^2) U. At speaking pressure the friction takes some 10 %
    // off U; as the wind dies it takes over, and the jet slows exponentially instead of
    // stopping dead at a finite moment (which would click).
    double jetSpeed(double P) const {
        if (P <= 0) return 0.0;
        return (std::sqrt(windR * windR + 2.0 * rho * P) - windR) / rho;
    }
    double flueFlow(double P) const { return h * Wm * jetSpeed(P); }
    double inflow(double dP) const {
        if (dP <= 0) return 0.0;   // the foot never rises above the chest
        const double g = pallet.gMax * palletPos;
        if (g <= 1e-9) return 0.0;
        const double Ap = kCd * pallet.w * g;
        double a = rho / (2 * Ap * Ap);
        if (toeArea > 0) a += rho / (2 * (kCd * toeArea) * (kCd * toeArea));
        const double b = 12.0 * mu * pallet.seal / (pallet.w * g * g * g);
        return (-b + std::sqrt(b * b + 4 * a * dP)) / (2 * a);
    }
    double tickKey() {
        double acc = 0;
        const double dt = 1.0 / fs;
        for (int k = 0; k < os; ++k) {
            // pallet motion: raised-cosine travel
            const double rate = keyDown ? dt / pallet.tOpen : -dt / pallet.tClose;
            palletPhase = std::max(0.0, std::min(1.0, palletPhase + rate));
            palletPos = 0.5 - 0.5 * std::cos(M_PI * palletPhase);
            // implicit step for the foot pressure: P - P0 = dt c2rho/V (Qin(Pc - P) - Qout(P))
            const double k1 = dt * c2rho / pallet.volume, P0 = footP;
            double lo = 0, hi = std::max(chest, P0);
            for (int it = 0; it < 40; ++it) {
                const double P = 0.5 * (lo + hi);
                const double r = P - P0 - k1 * (inflow(chest - P) - flueFlow(P));
                if (r > 0) hi = P; else lo = P;
            }
            footP = 0.5 * (lo + hi);
            acc += sub(footP);
        }
        return acc / os;
    }
    double palletPhase = 0;
    double sub(double footPa) {
        foot = footPa;
        const double Uj = jetSpeed(footPa);
        double qin = 0, eta = 0, qjet = 0;
        // acoustic displacement of the air in the mouth (leaky integral of v; the leak only
        // removes drift, 0.25 s time constant)
        xi += (vPrev - xi * 4.0) / fs;
        xiHist[(size_t)histPos] = xi;
        if (Uj > 1e-3) {
            // travel time of jet waves to the lip: u = V(x)/2 on the turbulent jet
            const double tauS = transit * (1.0 / Uj);
            // the stored displacement lags one sample (it is built from the previous v): read one less
            const double tau = std::min((double)xiHist.size() - 4.0, std::max(1.0, tauS * fs - 1.0));   // samples
            // growth: integral of mu = 2 omega / V. The law is applied up to its value at the voicing
            // pressure and held there below it: as the wind dies away the 1/V growth would otherwise
            // become unbounded and the jet would cross the lip in microseconds (ASSUMPTION)
            const double C = std::cosh(std::min(omega * tauS, growthMax));
            const double Vl = lipSpeed * Uj;
            const double pos = histPos - tau;
            const int i0 = (int)std::floor(pos);
            const double fr = pos - i0;
            const int n = (int)xiHist.size();
            const double xa = xiHist[(size_t)(((i0 % n) + n) % n)], xb = xiHist[(size_t)((((i0 + 1) % n) + n) % n)];
            const double xd = xa + (xb - xa) * fr;
            // jet position at the lip relative to the lip, deflection towards the inside positive:
            // the disturbance launched at the flue grows to cosh(mu W) on its way and arrives tau later,
            // less the motion of the air at the lip (Fletcher 1976: (v/jw)[cosh(mu W) e^{-jwW/u} - 1])
            eta = C * xd - xi;
            // Short waves do not grow on a jet: for k b > 2 the growth rate is negative (Fletcher
            // 1979, eq. 15, mu b = 0.74 (1 - e^{-3 k b}) - 0.37 k b). With k = omega/u, u = V/2,
            // the jet stops following disturbances above omega ~ 2u/b = V/b at the lip. A one-pole
            // low-pass at that frequency, which falls with the jet speed as the wind dies away.
            {
                const double wc = Vl / b;
                etaLP += (1.0 - std::exp(-wc / fs)) * (eta - etaLP);
                eta = etaLP;
            }
            // the jet cannot be carried further than the mouth is high (the exponential growth law
            // fails long before; this only matters as the wind dies away at the end of a note)
            eta = std::max(-W, std::min(W, eta));
            qin = Wm * b * Vl * (1.0 + std::tanh((eta - y0) / b));
            qjet = 2.0 * Wm * b * Vl;
        }
        // band limit (numerical, not physical): the jet switches across the lip within a sample
        // or two, and the model has no resonances above ~14 kHz; a 2nd-order Butterworth at 12 kHz
        // keeps the switching from aliasing
        {
            const double y = lp.b0 * qin + lp.z1;
            lp.z1 = lp.b1 * qin - lp.a1 * y + lp.z2;
            lp.z2 = lp.b2 * qin - lp.a2 * y;
            qin = y;
            const double yj = lpj.b0 * qjet + lpj.z1;
            lpj.z1 = lpj.b1 * qjet - lpj.a1 * yj + lpj.z2;
            lpj.z2 = lpj.b2 * qjet - lpj.a2 * yj;
            qjet = yj;
        }
        const double dQin = (qin - qinPrev) * fs;
        qinPrev = qin;
        const double v = vPrev;
        const double dp = -(rho * dd / Sm) * dQin - 0.5 * rho * (v / 0.6) * (v / 0.6) * (v >= 0 ? 1 : -1);
        double q = 0, s = 0;
        for (auto& m : modes) {
            const double y = m.b0 * dp + m.z1;
            m.z1 = m.b1 * dp - m.a1 * y + m.z2;
            m.z2 = m.b2 * dp - m.a2 * y;
            q += y;
            const double dq = (y - m.qPrev) * fs;
            m.qPrev = y;
            s += (m.reR - 1.0) * y + m.imR * dq / m.w;
        }
        vPrev = q / Sm;
        histPos = (histPos + 1) % (int)xiHist.size();
        // the part of the jet that does not enter the pipe flows out into the room: the room
        // sees (Qj - Qin) - q leaving the mouth (Qj = whole jet at the lip = 2 b V_lip Wm)
        s += (qjet - qin) * jetFlowRadiates;
        const double out = rho / (4 * M_PI) * (s - sPrev) * fs;
        sPrev = s;
        return out;
    }
};

} // namespace flue
} // namespace pipe

namespace pipe {
namespace flue {

// -- voicing on the sound -------------------------------------------------------
// What a voicer does at the voicing machine: blow the pipe, listen, adjust. Here the
// "listening" is a measurement of the steady sound.

// Hann-windowed DFT magnitude at f.
inline double toneAt(const std::vector<double>& x, size_t from, size_t len, double f, double fs) {
    double re = 0, im = 0;
    const double w = 2 * M_PI * f / fs;
    for (size_t n = 0; n < len; ++n) {
        const double win = 0.5 - 0.5 * std::cos(2 * M_PI * (double)n / (double)(len - 1));
        re += x[from + n] * win * std::cos(w * (double)n);
        im -= x[from + n] * win * std::sin(w * (double)n);
    }
    return std::sqrt(re * re + im * im);
}
inline double peakFreq(const std::vector<double>& x, size_t from, size_t len, double f0, double span, double fs, double* mag = nullptr) {
    // coarse scan, then golden section
    double best = 0, bf = f0;
    for (int i = -20; i <= 20; ++i) { const double f = f0 * (1 + span * i / 20.0); const double m = toneAt(x, from, len, f, fs); if (m > best) { best = m; bf = f; } }
    double a = bf * (1 - span / 20), b = bf * (1 + span / 20);
    for (int it = 0; it < 30; ++it) {
        const double m1 = b - (b - a) * 0.618, m2 = a + (b - a) * 0.618;
        if (toneAt(x, from, len, m1, fs) > toneAt(x, from, len, m2, fs)) b = m2; else a = m1;
    }
    const double f = 0.5 * (a + b);
    if (mag) *mag = toneAt(x, from, len, f, fs);
    return f;
}

// Time for the pipe to settle: a few decay times of its lowest resonance.
inline double settleSeconds(const Design& d) {
    const double f1 = d.modes.empty() ? d.target : d.modes[0].f, Q = d.modes.empty() ? 50 : d.modes[0].Q;
    return std::max(0.35, 4.0 * Q / (M_PI * f1)) + 0.05;
}

struct Steady { std::vector<double> x; size_t from = 0, len = 0; };
inline Steady blow(const Design& d, const Voicing& v, double fs, double pScale = 1.0, double ramp = 0.020, bool resonatorOnly = false) {
    FlueSim sim; sim.init(d, v, fs);
    if (resonatorOnly) sim.jetFlowRadiates = 0.0;
    const double T = settleSeconds(d);
    const double win = std::max(0.12, 40.0 / d.target);
    Steady s; s.x.resize((size_t)((T + win) * fs));
    (void)ramp;
    sim.key(true, kChestPa * pScale);
    for (size_t n = 0; n < s.x.size(); ++n) s.x[n] = sim.tickKey();
    s.from = (size_t)(T * fs); s.len = s.x.size() - s.from;
    return s;
}

// Does the pipe speak on its note? It must hold its intended frequency and not have
// jumped to an upper mode (which leaves no energy at the note itself).
struct Hearing { bool speaks; double f; double levelAtNote, strongest; };
inline Hearing listen(const Design& d, const Steady& s, double fs) {
    Hearing h{};
    double mNote = 0;
    h.f = peakFreq(s.x, s.from, s.len, d.target, 0.04, fs, &mNote);
    double strongest = mNote;
    // partials of the note and the resonances (an overblown pipe sounds on a resonance)
    for (const Mode& m : d.modes) {
        if (m.f > 0.45 * fs || m.f > d.target * 8) break;
        double mm = 0; peakFreq(s.x, s.from, s.len, m.f, 0.03, fs, &mm);
        strongest = std::max(strongest, mm);
    }
    for (int k = 2; k <= 8 && k * h.f < 0.45 * fs; ++k) strongest = std::max(strongest, toneAt(s.x, s.from, s.len, k * h.f, fs));
    h.levelAtNote = 20 * std::log10(mNote + 1e-30);
    h.strongest = 20 * std::log10(strongest + 1e-30);
    double rms = 0; for (size_t i = s.from; i < s.x.size(); ++i) rms += s.x[i] * s.x[i];
    rms = std::sqrt(rms / (double)s.len);
    h.speaks = rms > 1e-4 && h.levelAtNote > h.strongest - 20.0;
    return h;
}

struct VoicingReport { bool ok = false; double ising = 0, isingOpt = 0, hole = 0, centsBeforeCut = 0; int tries = 0; std::string log; };

// Speaking on the note: the note is (within 3 dB) the strongest partial; strings, whose
// fundamental is relatively weak (Fletcher & Douglas 1980), within 10 dB. (Voicing criterion,
// ASSUMPTION.)
inline bool onNote(const Design& d, const Hearing& h) {
    return h.speaks && h.levelAtNote > h.strongest - (d.family == kString ? 10.0 : 3.0);
}
// Efficiency: the resonator's radiated acoustic power (far-field monopole, from the pressure at 1 m) over the
// wind power through the flue (foot pressure x jet flow).
inline double efficiency(const Design& d, const Steady& s) {
    double ms = 0; for (size_t i = s.from; i < s.x.size(); ++i) ms += s.x[i] * s.x[i];
    ms /= (double)s.len;
    const Air air{ 20.0 };
    const double Pac = 4 * M_PI * ms / (air.rho() * air.c());
    const double Uj = std::sqrt(2 * d.footPa / air.rho());
    return Pac / (d.footPa * d.flue * d.mouthWidth * Uj);
}

// Voice and cut one pipe, as at the voicing machine:
// (1) harmonic pipes get a hole at half the speaking length (Flynt: "almost exactly at
//     one-half the speaking length"); its size is not published, so it is opened from 10 %
//     of the diameter until the pipe settles on its second mode;
// (2) Ising: "I = 2 gives maximum efficiency". The flue is set at several widths, the pipe
//     blown at each, and the width of greatest efficiency found (I_opt, in the model). The
//     pipe is then voiced at I_opt x (I_target / 2) - Ising's scale anchored at the model's own
//     efficiency maximum - or at the nearest width at which it still speaks on its note;
// (3) the body is cut until the sounding frequency is within 0.5 cent.
inline VoicingReport voice(Design& d, Voicing& v, double fs = 48000.0) {
    VoicingReport r;
    const double rho = Air{ 20.0 }.rho();
    const double Imax = maxIsing(d, rho);
    std::vector<double> holes = { 0.0 };
    if (d.speakMode == 2) { holes.clear(); for (double k = 0.10; k <= 0.501; k += 0.05) holes.push_back(k); }
    struct Pt { double I, eff; bool on; };
    std::vector<Pt> pts;
    for (double hk : holes) {
        if (d.speakMode == 2) {
            const double L = d.g.body[0].L, dia = 2 * d.radius;
            d.g.holes = { { 0.5 * L, M_PI * 0.25 * (hk * dia) * (hk * dia), d.g.wall } };
            recut(d, 1.0);
        }
        pts.clear();
        for (double I = 0.6; I <= 3.21 && I < Imax; I += 0.1) {
            if (!applyIsing(d, I, rho)) break;
            ++r.tries;
            const Steady s = blow(d, v, fs);
            const Hearing h = listen(d, s, fs);
            pts.push_back({ I, efficiency(d, blow(d, v, fs, 1.0, 0.020, true)), onNote(d, h) });
        }
        bool any = false; for (auto& p : pts) any |= p.on;
        if (any) { r.hole = hk * 2 * d.radius; break; }
    }
    double best = -1, Iopt = 0;
    for (auto& p : pts) if (p.on && p.eff > best) { best = p.eff; Iopt = p.I; }
    if (best < 0) { applyIsing(d, std::min(v.ising, Imax * 0.999), rho); r.log += "does not speak on its note at any flue width; "; return r; }
    r.isingOpt = Iopt;
    const double want = Iopt * v.ising / 2.0;
    // nearest speaking point to the wanted Ising number
    double use = Iopt, dist = 1e9;
    for (auto& p : pts) if (p.on && std::fabs(p.I - want) < dist) { dist = std::fabs(p.I - want); use = p.I; }
    // cut to pitch; cutting moves the resonances, so check again that the pipe still speaks on
    // its note, and if not, narrow the flue a step and cut again
    for (int round = 0; round < 12; ++round) {
        applyIsing(d, use, rho);
        bool on = false;
        for (int it = 0; it < 8; ++it) {
            const Steady s = blow(d, v, fs);
            const Hearing h = listen(d, s, fs);
            on = onNote(d, h);
            if (!on) break;
            const double cents = 1200 * std::log2(h.f / d.target);
            if (round == 0 && it == 0) r.centsBeforeCut = cents;
            if (std::fabs(cents) < 0.5) break;
            if (d.speakMode == 2 && !d.g.holes.empty()) d.g.holes[0].fromBottom *= h.f / d.target;
            recut(d, h.f / d.target);
        }
        if (on) { r.ok = true; r.ising = use; return r; }
        use -= 0.1;
        if (use < 0.55) break;
    }
    r.log += "lost its note after cutting; ";
    return r;
}

} // namespace flue
} // namespace pipe
