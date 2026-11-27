// Pipe - physically modelled pipe organ
// Wind system, as Opus 14 is winded: four weighted wedge bellows in a room beside the organ, stacked in a frame
// and emptying into one collecting trunk, and from it a wind line of its own for each manual division (Pos, Sw,
// Gt, their trunks side by side along the floor and up to the pallet boxes) and one for the Pedal; no regulators,
// the bellows' weights hold the wind (Pasi Organ Builders' description of the St Cecilia organ; the construction
// photographs; the case's section and plan drawings). A blower raises the bellows, or they can be raised by
// hand. The wind is flexible by design.
//
// The bellows (WindSystem: "the reservoir"): one lid standing for the four, with its weights' mass, the air
// under it as its spring, the friction of its leather and hinge, and the tilt of a wedge (its pressure rises a
// little as it closes). Speaking pipes and leaks draw air out; the blower puts it back until the lid reaches its
// working height, where the inlet valve closes. When the blower stops it coasts down; the bellows then empty at
// the rate the organ uses wind - slowly with one stop, quickly with full organ. When the lid reaches its stops
// only the air's own springiness is left and the pressure collapses.
// Each division's wind line (DivisionWind) carries the wind to its chest as travelling waves: a long trunk holds
// more air than its chest and a wave takes tens of milliseconds to cross it and back, so how far a chord pulls the
// chest down at its attack is set by the trunk's length and width (Carlsson, "On Dynamic Behaviour of Wind
// Systems for Pipe Organs", Chalmers 2002: "the very strong influence of the wind trunk length on the pallet box
// pressure"). The Wind Flexibility setting below 1 adds what Opus 14 does not have, a modern regulator at the
// chest end of each line, steadier the lower it is set.
//
// Flows are in units of the blower's full capacity (kBlowerCapacity pipe flow units); the bellows' size is the
// number of seconds they can supply that.
#pragma once

#include "Stoplist.hpp"
#include "Tremulant.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace pipe {

constexpr float kChestNominal[kNumDivisions] = { 79.f, 79.f, 79.f, 79.f };   // mm water: Opus 14 is voiced on 79 mm throughout (Vogt ch. 3)
constexpr float kBlowerCapacity = 260.f;   // flow units (Principal 8' C4 = 1)
// The tremulated division's trunk: its flow at the nominal pressure drop, in flow units (ASSUMPTION: about
// what a division's full chorus draws, so full organ on it loads the tremulant about 1.5 x)
constexpr float kTrunkUnitFlow = 80.f;

// ---------------------------------------------------------------------------------------------------------
// One division's wind at audio rate, in physical units: its wind line from the bellows (travelling waves in
// three sections tapering towards the chest), the optional regulator at its chest end, the wind chest (the
// air's springiness) with its concussion bellows, and, when it is drawn on this division, the tremulant's valve.
// The grooves and pipes (Engine::renderDivision) take their wind from the chest sample by sample and the chest
// feels what they take.
// ASSUMPTIONS (the drawings carry no scale; lengths read at an assumed 1:48, widths from the photographs):
// each line runs the collecting trunk (3 m), the floor (6 m) and its rise to the pallet box: Hauptwerk 12 m,
// Unterwerk (Sw) 10 m, Oberwerk (Pos) 15 m; the Pedal's line 12 m. Tapered 0.09 -> 0.07 -> 0.05 m^2 in three equal
// sections (about 30 cm square leaving the bellows). Chest (pallet box) 0.21 m^3. The optional regulator: lid
// 1.0 x 0.6 m working 80 mm open, curtain valve up to 0.03 m^2 closing over 40 mm.
struct DivisionWind {
    static constexpr double kAr = 0.6, kXWork = 0.08, kXMax = 0.15, kInletMax = 0.03, kInletSpan = 0.04,
                            kXRelief = 0.12, kReliefMax = 0.01;   // the regulator's waste (safety) valve: opens as its lid rises past 120 mm, 0.01 m^2 at the top
    static constexpr double kTrunkK = 1.5, kChestV = 0.21, kCd = 0.61;
    static constexpr int kSections = 3;
    static constexpr double kSectionArea[kSections] = { 0.09, 0.07, 0.05 };
    double lineLen = 12.0;        // the wind line's length, m (WindSystem sets it per division)
    double Pnom = 774.7;          // the division's pressure (Pa)
    bool regulated = false;       // a regulator at the line's chest end (Wind Flexibility below 1)
    double regMass = 20, regZeta = 0.3;
    double concussion = 0.4;      // the chest's concussion bellows, 0 (none or tied down) .. 1 (the largest)
    double Pb = 0;                // the air in the concussion bellows (its spring's pressure), Pa
    double leakM3 = 4.7e-4;       // the chest's leakage at its working pressure, m^3/s (WindSystem::update)
    // the line: in each section a wave running to the chest (fwd) and one running back (bwd), as delay lines
    struct Section { std::vector<double> fwd, bwd; int n = 1, pos = 0; double Z = 1, g = 1, dcF = 0, dcB = 0; };
    double dcK = 1e-4;            // the slow part of each wave: a one-pole follower, its coefficient per sample (buildLine)
    // a wave as it arrives: its slow part untouched (the walls' loss is a boundary layer's, nothing at a steady flow:
    // attenuating the whole wave would lose air from the line), its moving part attenuated by the walls
    static double arrive(double raw, double dc, double g) { return dc + g * (raw - dc); }
    Section sec[kSections];
    double lineFs = 0, lineRho = 0, lineC = 0;
    // state
    double Pr = 0, x = kXWork, v = 0;   // regulator: pressure, lid height, lid speed
    double q = 0, qAvg = 0;             // the line's flow into the chest (or the regulator), m^3/s, and its steady part
    double Pc = 0;                      // chest pressure, Pa
    double inSum = 0, outSum = 0, pcSum = 0, pcMean = 0; int nSum = 0;   // what came in from the bellows / went to the grooves, over the block
    double drawSum = 0, drawMean = 0, inMean = 0;   // what left the division for good (pipes, tremulant's vent, leaks), m^3/s
    // tremulant valve on this division (Tremulant.hpp sets kind, rate, depth)
    double tx = 0.02, tv = 0, tAmt = 0, tAvg = 0;

    // the line's sections for this sample rate and air: their delays, impedances rho c / S and wall losses
    void buildLine(double fs, double rho, double c) {
        lineFs = fs; lineRho = rho; lineC = c;
        const double nu = 1.5e-5, f = c / (4.0 * lineLen);   // its lowest resonance, where the loss matters
        for (int k = 0; k < kSections; ++k) {
            Section& s = sec[k];
            const double len = lineLen / kSections, S = kSectionArea[k], side = std::sqrt(S);
            s.n = std::max(1, (int)std::lround(len / c * fs));
            s.fwd.assign((size_t)s.n, 0.0); s.bwd.assign((size_t)s.n, 0.0); s.pos = 0;
            s.Z = rho * c / S;
            // the walls' viscous and thermal boundary layers (Kirchhoff): alpha = (1 / (a c)) sqrt(pi f nu)
            // (1 + (gamma - 1) / sqrt(Pr)), a the hydraulic radius (side / 2)
            const double alpha = std::sqrt(M_PI * f * nu) / (0.5 * side * c) * (1.0 + 0.4 / std::sqrt(0.71));
            s.g = std::exp(-alpha * len);
        }
        dcK = 1.0 - std::exp(-2.0 * M_PI * 0.1 * f / fs);   // a tenth of the lowest resonance
    }
    void fillLine(double P) { for (auto& s : sec) { std::fill(s.fwd.begin(), s.fwd.end(), 0.5 * P); std::fill(s.bwd.begin(), s.bwd.end(), 0.5 * P); s.dcF = s.dcB = 0.5 * P; } }
    // the line's impedance at the chest end, rho c / S
    double lineZ() const { return sec[kSections - 1].Z; }

    void reset(double Pres) {
        Pr = Pc = Pb = std::min(Pres, Pnom); x = Pres >= Pnom ? kXWork + 0.5 * kInletSpan : 0.0; v = 0; q = qAvg = 0; tx = 0.02; tv = 0; tAmt = 0; tAvg = Pc;
        inSum = outSum = pcSum = drawSum = drawMean = inMean = 0; pcMean = Pc; nSum = 0;   // all of it: a reset wind is the wind of a new organ
        fillLine(regulated ? Pres : Pc);
    }
    // What the line will draw from the bellows this sample, as a function of their pressure: q = G Pres - J (the
    // wave arriving at the bellows end is known before the sample is stepped). The bellows solve against it.
    void bellowsNorton(double dt, double Pres, double rho, double c, double& G, double& J) {
        if (lineFs != 1.0 / dt || lineRho != rho || lineC != c) { buildLine(1.0 / dt, rho, c); fillLine(regulated ? Pres : Pc); }
        const Section& s = sec[0];
        G = 1.0 / s.Z; J = 2.0 * arrive(s.bwd[(size_t)s.pos], s.dcB, s.g) / s.Z;
    }
    // One sample. Pres: the bellows' pressure (Pa); qOut: what the grooves' pallets and the chest's other pipes
    // take this sample (m^3/s). rho, c: the air. The tremulant: its kind, rate, depth, on.
    // Returns what the line drew from the bellows this sample (m^3/s).
    double step(double dt, double Pres, double qOut, double rho, double c, const Tremulant* trem, bool tremOn) {
        const double rc2 = rho * c * c;
        if (lineFs != 1.0 / dt || lineRho != rho || lineC != c) { buildLine(1.0 / dt, rho, c); fillLine(regulated ? Pres : Pc); }
        // the line: the waves arriving at each end of each section this sample, then the junctions
        double arrF[kSections], arrB[kSections];
        for (int k = 0; k < kSections; ++k) {
            Section& s = sec[k]; const double rf = s.fwd[(size_t)s.pos], rb = s.bwd[(size_t)s.pos];
            arrF[k] = arrive(rf, s.dcF, s.g); arrB[k] = arrive(rb, s.dcB, s.g);
            s.dcF += dcK * (rf - s.dcF); s.dcB += dcK * (rb - s.dcB);
        }
        double sendF[kSections], sendB[kSections];
        // at the bellows: their pressure holds the line's end (their air is far larger than the line's)
        sendF[0] = Pres - arrB[0];
        const double qBellows = (Pres - 2.0 * arrB[0]) / sec[0].Z;
        // between sections: pressure and flow carry on; the change of width reflects part of each wave
        for (int k = 0; k + 1 < kSections; ++k) {
            const double Z1 = sec[k].Z, Z2 = sec[k + 1].Z, P = (2.0 * (arrF[k] / Z1 + arrB[k + 1] / Z2)) / (1.0 / Z1 + 1.0 / Z2);
            sendB[k] = P - arrF[k]; sendF[k + 1] = P - arrB[k + 1];
        }
        // at the chest end the line is a source of 2 p+ behind its impedance Z (and the flow's turbulent loss)
        const double Zl = lineZ(), Sl = kSectionArea[kSections - 1];
        const double src = 2.0 * arrF[kSections - 1];
        const double Rt = kTrunkK * 0.5 * rho / (Sl * Sl) * std::fabs(q);   // the turbulence at its bends and its mouth
        const double Zs = Zl + Rt;

        // the tremulant in the line's chest end
        double qPiston = 0, qVent = 0;
        if (trem) {
            const double tw = 2.0 * M_PI * trem->rate, Q0 = trem->unitFlowM3;
            tAmt += ((tremOn ? 1.0 : 0.0) - tAmt) * std::min(1.0, 8.0 * dt);
            tAvg += (Pc - tAvg) * std::min(1.0, 2.0 * dt);
            if (trem->kind == kTremDoux) {
                // the wind passing through the trunk pushes the flap against its spring (as its dynamic pressure, the
                // flow squared), leaving it less of its travel to swing in; at the trunk's rated flow (a full chorus
                // on the division) it is held on its stop and the beat dies. Pykett: the doux's effect depends 'on
                // the volume and speed of the wind passing through' and 'can sometimes vanish altogether if too many
                // stops are drawn'. ASSUMPTION: it vanishes at kDouxHold x the trunk's unit flow.
                const double qr = std::fabs(qAvg) / (trem->kDouxHold * Q0), room = std::max(0.0, 1.0 - qr * qr);
                const double mu = 0.35 * tAmt, xr = tx / std::max(1e-3, room);
                tv += dt * (-tw * tw * tx + mu * tw * (1.0 - xr * xr) * tv - trem->kDouxBack * tw * tw * (Pc - tAvg) / Pnom / std::max(1e-3, (double)trem->depth));
                tx += dt * tv;
                if (room < 1e-3) { tx *= 0.999; tv *= 0.999; }   // held on its stop
                qPiston = trem->kDouxArea * trem->depth * tAmt * Q0 * tv / tw;
            } else {
                // the vent perdu's pallet is worked by its own motor (a small bellows on the chest's wind that the
                // pallet's opening empties): a self-sustaining beat at the rate its weight and spring set, close to
                // a relaxation (mu 1). Only the opening half of its swing lifts the pallet off its seat, so the
                // spill comes in bursts: a deep, less sinusoidal beat. What it spills scales with the chest's
                // pressure, not with what the pipes draw, so it keeps its depth with full organ (Pykett).
                const double mu = trem->kVentMu * tAmt, wv = trem->kVentRateK * tw;
                tv += dt * (-wv * wv * tx + mu * wv * (1.0 - tx * tx) * tv - (1.0 - tAmt) * 4.0 * wv * tv);
                tx += dt * tv;
                const double lift = std::max(0.0, std::min(1.0, tx));
                qVent = trem->kVentArea * trem->depth * tAmt * lift * Q0 * std::sqrt(std::max(0.0, Pc / Pnom));
                if (tAmt < 1e-3 && std::fabs(tx) < 1e-3) { tx = 0.02; tv = 0; }   // at rest, ready to start
            }
        }

        // the concussion bellows on the chest: a spring-loaded bellows whose leather and hinges give it a
        // resistance; it takes in a surge and gives it back slowly. ASSUMPTION: at full size its springs give it
        // 4 x the chest's own compliance, its resistance the wind line's impedance (so a wave arriving down the line
        // is taken up rather than sent back).
        double qCb = 0;
        if (concussion > 1e-3) {
            const double Cc = concussion * 4.0 * kChestV / rc2;
            qCb = (Pc - Pb) / Zl;
            Pb += dt * qCb / Cc;
        } else Pb = Pc;
        // the chest's leaks: past its sliders and pallets and through its joints, laminar (as the pressure).
        const double qLeak = leakM3 * std::max(0.0, Pc) / Pnom;

        double qLine = 0, Pend = 0, qin = 0;
        if (!regulated) {
            // the line opens into the chest: Pc' = Pc + k (q - out), q = (src - Pc') / Zs (backward Euler)
            const double k = dt * rc2 / kChestV, out = qOut + qVent - qPiston + qCb + qLeak;
            Pc = std::max(0.0, (Pc + k * (src / Zs - out)) / (1.0 + k / Zs));
            qLine = (src - Pc) / Zs;
            Pend = src - Zl * qLine;
            qin = qLine;
        } else {
            // a regulator at the line's end: its curtain valve lets in from the line as its lid falls
            const double open = std::max(0.0, std::min(1.0, (kXWork + 0.5 * kInletSpan - x) / kInletSpan));
            const double G = open > 0 ? kCd * kInletMax * open * std::sqrt(2.0 / rho) : 0.0;
            const double kAir = rc2 * kAr * kAr / (kAr * kXWork);
            const double force = (Pr - Pnom) * kAr - 2.0 * regZeta * std::sqrt(kAir * regMass) * v;
            v += dt * force / regMass;
            x += dt * v;
            if (x < 0.002) { x = 0.002; if (v < 0) v = 0; }
            if (x > kXMax) { x = kXMax; if (v > 0) v = 0; }
            const double Vr = kAr * x;
            // the short connection from regulator to chest: the air's inertia (1 m of 0.03 m^2) and its loss
            const double L = rho * 1.0 / 0.03, loss = kTrunkK * 0.5 * rho / (0.03 * 0.03) * std::fabs(q);
            q += dt * (Pr - Pc - loss * q) / L;
            double qWaste = 0;
            if (x > kXRelief) qWaste = kCd * kReliefMax * std::min(1.0, (x - kXRelief) / (kXMax - kXRelief)) * std::sqrt(2.0 * Pr / rho);
            // the regulator's air: Pr' = Pr + kr (qin - o), the inlet's flow from the line's end: the line is a
            // source src behind Zs, the valve G sgn(u) sqrt|u| with u = Pend - Pr' (solved by Newton)
            const double kr = dt * rc2 / Vr, o = q + qWaste + kAr * v;
            double Pn = Pr;
            for (int it = 0; it < 6; ++it) {
                // the valve flow given the regulator's pressure Pn: qi with src - Zs qi - Pn = u, qi = G sgn(u) sqrt|u|
                const double R = src - Pn;
                double qi = 0, dqi = 0;
                if (G > 0) {
                    const double a = Zs * G;   // u + a sgn(u) sqrt|u| ... with qi = G sgn(u) sqrt|u|: u = R - Zs qi
                    const double r = (-a + std::sqrt(a * a + 4.0 * std::fabs(R))) / 2.0, u = R >= 0 ? r * r : -r * r;
                    qi = (u >= 0 ? 1.0 : -1.0) * G * r;
                    dqi = -G / (a + 2.0 * r + 1e-12);
                }
                const double F = Pn - Pr - kr * (qi - o), dF = 1.0 - kr * dqi;
                Pn = std::max(0.0, Pn - F / dF);
                qin = qi;
            }
            Pr = Pn;
            qLine = qin;
            Pend = src - Zl * qLine;
            const double k = dt * rc2 / kChestV;
            Pc += k * (q - qOut - qVent + qPiston - qCb - qLeak);
            Pc = std::max(0.0, Pc);
        }
        qAvg += (qLine - qAvg) * std::min(1.0, 2.0 * M_PI * 2.0 * dt);   // the line's steady flow (below 2 Hz): what holds the doux's flap
        if (!regulated) q = qLine;
        sendB[kSections - 1] = Pend - arrF[kSections - 1];
        for (int k = 0; k < kSections; ++k) { Section& s = sec[k]; s.fwd[(size_t)s.pos] = sendF[k]; s.bwd[(size_t)s.pos] = sendB[k]; s.pos = (s.pos + 1) % s.n; }
        inSum += qBellows; outSum += qOut; pcSum += Pc; drawSum += qOut + qVent + qLeak; ++nSum;
        return qBellows;
    }
    // what the line drew from the bellows over the block (m^3/s), and reset
    // (and the chest's mean pressure over the block, pcMean)
    double takeInflow() { const double r = nSum ? inSum / nSum : 0.0; pcMean = nSum ? pcSum / nSum : Pc; drawMean = nSum ? drawSum / nSum : 0.0; inMean = r; inSum = outSum = pcSum = drawSum = 0; nSum = 0; return r; }
    // the air the regulator holds above its lid's stops (m^3), and the line's own above its chest's: they give this
    // back to the chest once the bellows fall below its pressure
    double storedM3() const { return regulated ? kAr * std::max(0.0, x - 0.002) : 0.0; }
};

enum ReservoirLoad { kLoadWeights = 0, kLoadSprings, kNumReservoirLoads };
static const char* const kReservoirLoadLabels[kNumReservoirLoads] = { "WEIGHTS", "SPRINGS" };

struct WindSystem {
    // settings
    float reservoirTarget = 79.f;   // the bellows' pressure at their working height, mm (Opus 14 has no regulators: the weights are set so the chests stand at the 79 mm the pipes were voiced on; the lines lose a fraction of a mm at a few pipes' flow)
    float flexibility = 1.f;        // 1: Opus 14's wind, the bellows alone; below 1 a regulator at each chest, steadier towards 0
    float concussion = 0.4f;
    bool blowerOn = true;
    int load = kLoadWeights;        // Opus 14's wedge bellows carry weights
    float sizeSeconds = 18.f;       // the bellows' air at their working height, in seconds of the blower's full output
    float spinDown = 8.f;           // blower coast-down time constant, s
    float spinUp = 2.5f;            // blower run-up time constant, s
    float leak = 0.004f;            // leakage at working pressure, fraction of blower output
    float tremSpeed = 5.8f, tremDepth = 0.22f;
    Tremulant trem;
    float tremLoad = 0.f;
    DivisionWind div[kNumDivisions];
    int tremTarget = kSwell;
    bool tremOn = false;

    // geometry of the bellows
    static constexpr float kWorkHeight = 0.8f;   // lid height (fraction of full travel) where the inlet valve shuts
    static constexpr float kStops = 0.025f;      // last part of the travel: lid on its stops, folds closing
    static constexpr float kSpringAtBottom = 0.12f;   // spring load at the bottom, relative to working height (springs nearly slack)
    static constexpr float kFanHead = 1.35f;     // blower's shut-off pressure, relative to the bellows' working pressure
    // Opus 14's four wedge bellows, each 4 x 8 ft (Pasi): 11.9 m^2 of lid between them, at 18 s of the blower's output.
    // ASSUMPTIONS: a wedge opens to 12 degrees at the top of its travel (its pressure, from weights, goes with the
    // cosine of its tilt); the friction of its leather folds and hinge damps its lid at 0.1 of critical.
    static constexpr double kBellowsArea = 4 * 1.219 * 2.438, kBellowsSize = 18.0, kWedgeMaxDeg = 12.0, kLidZeta = 0.1, kLidSlowS = 1.0;
    static constexpr double kCapM3 = 4.5e-4 * kBlowerCapacity;   // the blower's full output, m^3/s (Engine kRefFlow per flow unit)
    // the wind lines' lengths: Pedal, Hauptwerk, Unterwerk (Sw), Oberwerk (Pos) (DivisionWind)
    static constexpr double kLineLen[kNumDivisions] = { 12.0, 12.0, 10.0, 15.0 };

    // state
    float lid = 0.f;            // lid height, 0 (empty) .. 1 (full travel)
    double lidVel = 0;          // its speed, travel / s
    double lidVelSlow = 0;      // its slow travel, m/s (stepBellows)
    double airPa = 0;           // the air under the lid, Pa
    double qExtern = 0;         // what reaches the bellows other than through the wind lines, m^3/s (update)
    double directM3 = 0;        // the pipes on chests of their own: what they drew (the engine sets it), m^3/s
    float fan = 0.f;            // blower speed, 0 .. 1
    float reservoir = 0.f;      // the bellows' pressure, mm
    float sag = 0.f;            // (kept for the engine's outputs: the lid's own motion is in the pressure now)
    float chest[kNumDivisions] = {};
    float demand = 0.f;         // last demand, relative to blower capacity
    float secondsLeft = -1.f;   // how long the stored wind lasts at today's use (-1: blower keeping up)

    // The load's pressure at lid height h (mm): weights on a wedge go with the cosine of its tilt; springs press
    // less as the lid comes down.
    float lidPressure(float h) const {
        float loadAt;
        if (load == kLoadWeights) {
            const double deg = kWedgeMaxDeg * M_PI / 180.0;
            loadAt = (float)(std::cos(deg * h) / std::cos(deg * kWorkHeight));
        } else
            loadAt = kSpringAtBottom + (1.f - kSpringAtBottom) * std::min(1.f, h / kWorkHeight) + (h > kWorkHeight ? 0.3f * (h - kWorkHeight) : 0.f);
        return reservoirTarget * loadAt;   // the weights press all the way down: the lid on its stops is the air running out
    }
    // the lid's area and the mass its load stands for: weights weigh what they press; springs, ASSUMPTION, a
    // quarter of that (the lid and its folds)
    double bellowsArea() const { return kBellowsArea * std::max(0.05, (double)sizeSeconds / kBellowsSize); }
    double lidMass() const { const double m = reservoirTarget * 9.80665 * bellowsArea() / 9.80665; return load == kLoadWeights ? m : 0.25 * m; }
    double maxVolume() const { return (double)std::max(0.5f, sizeSeconds) * kCapM3 / kWorkHeight; }

    void setLines() {
        for (int d = 0; d < kNumDivisions; ++d) {
            div[d].lineLen = kLineLen[d];
            div[d].regulated = flexibility < 0.999f;
        }
    }
    void reset() {
        fan = blowerOn ? 1.f : 0.f;
        lid = blowerOn ? kWorkHeight : 0.f;
        lidVel = 0; lidVelSlow = 0;
        reservoir = blowerOn ? lidPressure(lid) : 0.f;
        airPa = reservoir * 9.80665;
        sag = 0.f; demand = 0.f; tremLoad = 0.f;
        setLines();
        for (int d = 0; d < kNumDivisions; ++d) { chest[d] = blowerOn ? std::min(kChestNominal[d], reservoir) : 0.f; div[d].Pnom = kChestNominal[d] * 9.80665; div[d].lineFs = 0; div[d].reset(reservoir * 9.80665); }
        trem.reset();
        secondsLeft = -1.f;
    }

    // One sample of the bellows: their air and their lid, against what the wind lines draw (q = G P - J, each
    // line's bellowsNorton summed) and what update() found reaches them otherwise. Stepped with the lines, sample
    // by sample, and solved implicitly (backward Euler) for the air and the lid together: a small bellows, or one
    // whose lid has come down on its stops, is a stiff spring of air against the lines, and stepped explicitly it
    // rang and ran away.
    void stepBellows(double dt, double G, double J) {
        const double rc2 = 1.2041 * 343.21 * 343.21, A = bellowsArea(), m = lidMass(), Vmax = maxVolume();
        // ASSUMPTION: what stays below the lid on its stops (the bottom board's depth, the feeders' trunk), 5 % of the travel
        const double V = std::max(0.05 * Vmax, Vmax * (double)lid);
        const double Pload = lidPressure(lid) * 9.80665, kAir = rc2 * A * A / V, wn = std::sqrt(kAir / m);
        const double K = rc2 / V;                        // dP = K (q - dV/dt)
        double vy = lidVel * Vmax / A;                   // the lid's speed, m/s (its swept volume A vy)
        // ASSUMPTION: the lid's losses (its leather folds and hinges) damp its swing about its slow travel, not the
        // travel itself: a dashpot on its whole speed held a falling lid's pressure 1-10 % below its weights' (the
        // faster it fell, the more), which a weighted bellows does not do. The slow travel: its speed over 1 s.
        const double cz = 2.0 * kLidZeta * wn * dt, damp = 1.0 + cz, a = dt * A / (m * damp);
        const double vFree = (vy + cz * lidVelSlow) / damp;
        // the lid: vy' = vFree + a (P' - Pload); the air: P' = P + dt K (qExtern - (G P' - J) - A vy')
        double P = (airPa + dt * K * (qExtern + J - A * (vFree - a * Pload))) / (1.0 + dt * K * (G + A * a));
        vy = vFree + a * (P - Pload);
        // on its stops the lid stands: the air alone answers
        if ((lid <= 0.f && vy < 0) || (lid >= 1.f && vy > 0)) { vy = 0; P = (airPa + dt * K * (qExtern + J)) / (1.0 + dt * K * G); }
        airPa = std::max(0.0, P);
        lidVelSlow += std::min(1.0, dt / kLidSlowS) * (vy - lidVelSlow);
        lidVel = vy * A / Vmax;
        lid += (float)(dt * lidVel);
        if (lid <= 0.f) { lid = 0.f; if (lidVel < 0) lidVel = 0; }
        if (lid >= 1.f) { lid = 1.f; if (lidVel > 0) lidVel = 0; }
        reservoir = (float)(airPa / 9.80665);
    }

    // Advance by dt seconds given total flow demand (pipe flow units: what the wind lines and the pipes standing
    // on the bellows directly drew). divFlow: each division's share of it (for the tremulant's load), or null.
    void update(float dt, float flowDemand, const float* divFlow = nullptr) {
        (void)divFlow;
        demand = flowDemand / kBlowerCapacity;
        // blower runs up or coasts down
        fan += ((blowerOn ? 1.f : 0.f) - fan) * std::min(1.f, dt / (blowerOn ? spinUp : spinDown));
        if (!blowerOn && fan < 1e-3f) fan = 0.f;

        const float Pw = std::max(1.f, reservoirTarget);
        const float Pmm = (float)(airPa / 9.80665);
        // Blower: a fan's pressure goes with its speed squared; flow through it with the square root of the
        // pressure it has to spare. The inlet valve closes as the lid reaches its working height. A check valve
        // keeps the wind from running back out.
        const float head = kFanHead * Pw * fan * fan;
        const float valve = std::max(0.f, std::min(1.f, (kWorkHeight + 0.03f - lid) / 0.06f));
        const float spare = std::max(0.f, head - Pmm) / ((kFanHead - 1.f) * Pw);
        const float inflow = 1.15f * fan * std::sqrt(spare) * valve;
        const float leakFlow = leak * std::sqrt(std::max(0.f, Pmm) / Pw);
        // what reaches the bellows other than the wind lines (stepBellows takes those sample by sample): the
        // blower, the leak, and the pipes standing on chests of their own
        qExtern = ((double)inflow - leakFlow) * kCapM3 - directM3;
        // how long the wind lasts: all the air stored (the bellows' and the regulators', which feed the chests)
        // over what leaves the system for good (the chests' pipes and leaks, the bellows' leak) less what the blower brings.
        {
            double stored = (double)lid * std::max(0.5f, sizeSeconds) / kWorkHeight, used = leakFlow + demand;
            for (int d = 0; d < kNumDivisions; ++d) { stored += div[d].storedM3() / kCapM3; used += (div[d].drawMean - div[d].inMean) / kCapM3; }
            const double drain = used - inflow;
            secondsLeft = drain > 1e-4 ? (float)(stored / drain) : -1.f;
        }

        // each division's wind line, regulator, chest and tremulant: DivisionWind, stepped at audio rate by the
        // engine with the grooves on the chest (Engine::renderGrooves); the regulators' flexibility:
        for (int d = 0; d < kNumDivisions; ++d) {
            div[d].Pnom = kChestNominal[d] * 9.80665;
            div[d].lineLen = kLineLen[d];
            const bool reg = flexibility < 0.999f;
            if (reg != div[d].regulated) { div[d].regulated = reg; div[d].x = DivisionWind::kXWork + 0.5 * DivisionWind::kInletSpan; div[d].v = 0; div[d].Pr = div[d].Pc; }
            const double f = std::min(1.0, (double)flexibility / 0.999);
            div[d].regMass = 8.0 + 40.0 * f;                       // a heavier lid answers slower
            div[d].regZeta = 0.08 + 0.7 * (1.0 - f);
            div[d].concussion = concussion;
            div[d].leakM3 = (double)leak * kCapM3;
        }
    }
};

} // namespace pipe
