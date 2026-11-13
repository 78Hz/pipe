// Pipe - physically modelled pipe organ
// Resonators from geometry: one-dimensional acoustic transmission lines.
//
// A pipe body is a chain of elements (cylinders, cones built from short
// cylinders, side holes) closed by a termination (open end, stopper, chimney).
// Each element is a 2x2 matrix relating acoustic pressure p and volume flow U
// at its two ends, [p1, U1] = T [p2, U2]. Wall losses (viscous and thermal
// boundary layers) enter through the complex propagation constant and
// characteristic impedance (Keefe 1984, large-radius approximation);
// radiation from openings through the unflanged-pipe radiation impedance
// (Levine & Schwinger end correction, low-frequency radiation resistance).
//
// For a flue pipe the jet drives the air in the mouth, which is in series with
// the body: resonances are the peaks of the admittance 1 / (Zmouth + Zbody)
// (Fletcher 1976; Acoustics Today 2017 uses the same input-admittance picture).
// For a reed pipe the tongue sees the resonator's input impedance at the
// shallot: resonances are its peaks.
//
// Everything is in SI units (metres, pascals, m^3/s). No real-time use: this
// runs when a pipe is (re)voiced and produces a list of modes.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace pipe {
namespace resonator {

using cd = std::complex<double>;

struct Air {
    double tempC = 18.0;
    double c() const { return 331.3 * std::sqrt(1.0 + tempC / 273.15); }
    double rho() const { return 1.2929 * 273.15 / (273.15 + tempC); }
    double eta() const { return 1.708e-5 * (1.0 + 0.0029 * tempC); }   // dynamic viscosity, Pa s
};

struct Mat { cd a, b, c, d; };   // [p1;U1] = [[a,b],[c,d]] [p2;U2]
inline Mat operator*(const Mat& x, const Mat& y) {
    return { x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d };
}

// Lossy cylinder of radius r, length L (Keefe 1984, eqs. for large rv).
inline Mat cylinder(double w, double r, double L, const Air& air) {
    const double S = M_PI * r * r, c = air.c(), rho = air.rho();
    const double rv = r * std::sqrt(w * rho / air.eta());
    const double k = w / c;
    // propagation constant and characteristic impedance with boundary-layer losses
    const cd G = cd(k * 1.045 / rv, k * (1.0 + 1.045 / rv));
    const cd Zc = (rho * c / S) * cd(1.0 + 0.369 / rv, -0.369 / rv);
    const cd ch = std::cosh(G * L), sh = std::sinh(G * L);
    return { ch, Zc * sh, sh / Zc, ch };
}

// Cone (frustum) from radius r1 to r2 over length L, as short cylinders.
inline Mat cone(double w, double r1, double r2, double L, const Air& air) {
    const int n = std::max(8, std::min(400, (int)std::ceil(L / 0.004)));
    Mat m{ 1, 0, 0, 1 };
    for (int i = 0; i < n; ++i) {
        const double r = r1 + (r2 - r1) * (i + 0.5) / n;
        m = m * cylinder(w, r, L / n, air);
    }
    return m;
}

// Equivalent radius of an opening of area S.
inline double eqRadius(double S) { return std::sqrt(S / M_PI); }

// Radiation impedance of an unflanged open end of radius a (acoustic, p/U):
// low-frequency resistance plus the Levine-Schwinger end correction, which
// falls with ka (Norris & Sheng fit).
inline cd openEnd(double w, double a, const Air& air) {
    const double c = air.c(), rho = air.rho(), k = w / c, ka = k * a, S = M_PI * a * a;
    const double delta = a * 0.6133 * (1.0 + 0.044 * ka * ka) / (1.0 + 0.19 * ka * ka);
    const double R = rho * c / S * 0.25 * ka * ka / (1.0 + 0.25 * ka * ka);   // saturates at rho c / S
    return cd(R, rho * c / S * k * delta);
}

// A side opening of area S in a wall of thickness t (harmonic-flute hole, mouth):
// air plug of thickness t plus end corrections inside (into the duct) and outside
// (radiating), and the outside radiation resistance.
inline cd orifice(double w, double S, double t, const Air& air, double innerCorr = 0.82, double outerCorr = 0.82) {
    const double rho = air.rho(), c = air.c(), a = eqRadius(S), k = w / c;
    const double len = t + (innerCorr + outerCorr) * a;
    const double R = rho * c / S * 0.25 * (k * a) * (k * a) / (1.0 + 0.25 * (k * a) * (k * a));
    // boundary-layer loss in the plug (as in a short tube)
    const double rv = a * std::sqrt(w * rho / air.eta());
    const double Rv = rho * w * len / S * (1.045 / std::max(1.0, rv));
    return cd(R + Rv, w * rho * len / S);
}

inline cd shuntOf(const Mat& m, cd ZL) {    // input impedance of a chain loaded by ZL
    if (std::abs(ZL) > 1e30) return m.a / m.c;
    return (m.a * ZL + m.b) / (m.c * ZL + m.d);
}

// ---------------------------------------------------------------------------
// Pipe descriptions (metres)

enum class Top { Open, Stopped, Chimney };

struct Segment { double r1, r2, L; };          // r1 at the lower end; r1 == r2 is a cylinder
struct Hole { double fromBottom, area, wall; }; // side hole (harmonic flute)

struct FlueGeometry {
    std::vector<Segment> body;     // from the mouth upwards
    Top top = Top::Open;
    double chimneyR = 0, chimneyL = 0;   // chimney through the stopper (Top::Chimney)
    std::vector<Hole> holes;
    double mouthWidth = 0, cutup = 0, wall = 0.0005;   // mouth opening (width x cut-up), lip thickness
};

struct ReedGeometry {
    std::vector<Segment> resonator;   // from the shallot upwards
    double shallotR = 0, shallotL = 0;   // the shallot's bore below the resonator (0 = none)
    Top top = Top::Open;
};

// Body impedance seen from its lower end.
inline cd bodyImpedance(double w, const std::vector<Segment>& segs, Top top, double chimR, double chimL,
                        const std::vector<Hole>& holes, const Air& air) {
    // terminate at the top
    cd ZL;
    const Segment& last = segs.back();
    if (top == Top::Open) ZL = openEnd(w, last.r2, air);
    else if (top == Top::Stopped) ZL = cd(1e40, 0);
    else ZL = shuntOf(cylinder(w, chimR, chimL, air), openEnd(w, chimR, air));
    // walk down: segments, inserting holes at their positions
    double z = 0; for (const auto& s : segs) z += s.L;
    for (int i = (int)segs.size() - 1; i >= 0; --i) {
        const Segment& s = segs[(size_t)i];
        const double z0 = z - s.L;
        // holes inside this segment, from the top down
        std::vector<Hole> hs;
        for (const auto& h : holes) if (h.fromBottom >= z0 && h.fromBottom < z) hs.push_back(h);
        std::sort(hs.begin(), hs.end(), [](const Hole& a, const Hole& b) { return a.fromBottom > b.fromBottom; });
        double zTop = z;
        auto piece = [&](double from, double to) {   // part of this segment between heights from < to
            if (to - from < 1e-9) return;
            const double f1 = (from - z0) / s.L, f2 = (to - z0) / s.L;
            const double ra = s.r1 + (s.r2 - s.r1) * f1, rb = s.r1 + (s.r2 - s.r1) * f2;
            const Mat m = std::fabs(rb - ra) < 1e-9 ? cylinder(w, ra, to - from, air) : cone(w, ra, rb, to - from, air);
            ZL = shuntOf(m, ZL);
        };
        for (const auto& h : hs) {
            piece(h.fromBottom, zTop);
            const cd Zh = orifice(w, h.area, h.wall, air);
            ZL = 1.0 / (1.0 / ZL + 1.0 / Zh);
            zTop = h.fromBottom;
        }
        piece(z0, zTop);
        z = z0;
    }
    return ZL;
}

inline cd mouthImpedance(double w, const FlueGeometry& g, const Air& air) {
    return orifice(w, g.mouthWidth * g.cutup, g.wall, air);
}

// Admittance the jet sees: 1 / (Zmouth + Zbody).
inline cd flueAdmittance(double w, const FlueGeometry& g, const Air& air) {
    return 1.0 / (mouthImpedance(w, g, air) + bodyImpedance(w, g.body, g.top, g.chimneyR, g.chimneyL, g.holes, air));
}

// Ratio of the flow radiated at the open end (or chimney) to the flow through the
// mouth, at one frequency (for the two radiating sources).
inline cd flueTopFlowRatio(double w, const FlueGeometry& g, const Air& air) {
    if (g.top == Top::Stopped) return 0.0;
    // propagate from the bottom: U_top / U_bottom for the chain loaded by the top termination
    const Segment& last = g.body.back();
    cd ZL = g.top == Top::Open ? openEnd(w, last.r2, air)
                               : shuntOf(cylinder(w, g.chimneyR, g.chimneyL, air), openEnd(w, g.chimneyR, air));
    Mat m{ 1, 0, 0, 1 };
    for (const auto& s : g.body)
        m = m * (std::fabs(s.r2 - s.r1) < 1e-9 ? cylinder(w, s.r1, s.L, air) : cone(w, s.r1, s.r2, s.L, air));
    // U1 = c p2 + d U2 with p2 = ZL U2
    cd ratio = 1.0 / (m.c * ZL + m.d);
    if (g.top == Top::Chimney) {
        const Mat ch = cylinder(w, g.chimneyR, g.chimneyL, air);
        ratio = ratio / (ch.c * openEnd(w, g.chimneyR, air) + ch.d);
    }
    return ratio;   // holes ignored here (none on pipes that radiate from the top through a hole)
}

inline cd reedImpedance(double w, const ReedGeometry& g, const Air& air) {
    cd Z = bodyImpedance(w, g.resonator, g.top, 0, 0, {}, air);
    if (g.shallotL > 0) Z = shuntOf(cylinder(w, g.shallotR, g.shallotL, air), Z);
    return Z;
}

// ---------------------------------------------------------------------------
// Modes: peaks of a response curve, with frequency, quality factor and the
// response's (complex) value at the peak.

struct Mode { double f, Q; cd peak; double magnitude; };

template <class Fn>
std::vector<Mode> findModes(Fn response, double fLo, double fHi, int maxModes) {
    const int N = 24000;
    std::vector<double> fs((size_t)N), mag((size_t)N);
    for (int i = 0; i < N; ++i) {
        fs[(size_t)i] = fLo * std::pow(fHi / fLo, (double)i / (N - 1));
        mag[(size_t)i] = std::abs(response(2.0 * M_PI * fs[(size_t)i]));
    }
    std::vector<Mode> out;
    for (int i = 1; i + 1 < N && (int)out.size() < maxModes; ++i) {
        if (!(mag[(size_t)i] > mag[(size_t)i - 1] && mag[(size_t)i] >= mag[(size_t)i + 1])) continue;
        // refine the peak by golden-section search between neighbours
        double a = fs[(size_t)i - 1], b = fs[(size_t)i + 1];
        for (int it = 0; it < 60; ++it) {
            const double m1 = b - (b - a) * 0.618, m2 = a + (b - a) * 0.618;
            if (std::abs(response(2 * M_PI * m1)) > std::abs(response(2 * M_PI * m2))) b = m2; else a = m1;
        }
        const double f0 = 0.5 * (a + b);
        const cd pk = response(2 * M_PI * f0);
        const double pm = std::abs(pk);
        // half-power points
        auto edge = [&](double dir) {
            double lo = f0, hi = f0;
            double step = f0 * 1e-4;
            while (std::abs(response(2 * M_PI * (f0 + dir * step))) > pm / std::sqrt(2.0) && step < f0) step *= 1.5;
            lo = f0 + dir * step / 1.5; hi = f0 + dir * step;
            for (int it = 0; it < 50; ++it) {
                const double m = 0.5 * (lo + hi);
                if (std::abs(response(2 * M_PI * m)) > pm / std::sqrt(2.0)) lo = m; else hi = m;
            }
            return 0.5 * (lo + hi);
        };
        const double bw = edge(1.0) - edge(-1.0);
        // skip tiny ripples: require the peak to stand above its surroundings
        out.push_back({ f0, f0 / std::max(1e-9, bw), pk, pm });
    }
    return out;
}

} // namespace resonator
} // namespace pipe
