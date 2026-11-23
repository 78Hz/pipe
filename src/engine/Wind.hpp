// Pipe - physically modelled pipe organ
// Wind system: blower -> main reservoir -> regulator per division -> chest.
//
// The reservoir holds a volume of air under a loaded lid. The load sets the
// pressure: weights press equally hard wherever the lid is, springs press less
// as the lid comes down. Speaking pipes and leaks draw air out; the blower
// puts it back until the lid reaches its working height, where the inlet valve
// closes. When the blower stops it coasts down; the reservoir then empties at
// the rate the organ uses wind - slowly with one stop, quickly with full
// organ - and the pressure follows the lid down. When the lid reaches its
// stops only the air's own springiness is left and the pressure collapses.
// Each division's regulator holds its chest at its set pressure while it can
// and passes the reservoir's pressure through once that falls below it, so
// the division set highest gives way first.
//
// Flows are in units of the blower's full capacity (kBlowerCapacity pipe flow
// units); the reservoir's size is the number of seconds it can supply that.
#pragma once

#include "Stoplist.hpp"
#include "Tremulant.hpp"
#include <cmath>
#include <algorithm>

namespace pipe {

constexpr float kChestNominal[kNumDivisions] = { 79.f, 79.f, 79.f, 79.f };   // mm water: Opus 14 is voiced on 79 mm throughout (Vogt ch. 3)
constexpr float kBlowerCapacity = 260.f;   // flow units (Principal 8' C4 = 1)
// The tremulated division's trunk: its flow at the nominal pressure drop, in flow units (ASSUMPTION: about
// what a division's full chorus draws, so full organ on it loads the tremulant about 1.5 x)
constexpr float kTrunkUnitFlow = 80.f;

// ---------------------------------------------------------------------------------------------------------
// One division's wind at audio rate, in physical units: its regulator (a weighted bellows whose curtain valve
// lets in wind from the main reservoir as its lid falls), the trunk to the chest (the air's inertia and its
// losses), the wind chest (the air's springiness) and, when it is drawn on this division, the tremulant's
// valve in it. The grooves and pipes (Engine::renderDivision) take their wind from the chest sample by
// sample and the chest feels what they take.
// ASSUMPTIONS (no Opus 14 drawings at hand; sizes typical of slider-chest organs - Audsley, Dom Bedos):
// regulator lid 1.0 x 0.6 m working 80 mm open, curtain valve up to 0.03 m^2 closing over 40 mm; trunk 3 m
// of 0.03 m^2 (loss coefficient 1.5); chest 0.21 m^3 (e.g. 2.4 x 0.6 x 0.15 m), which with the trunk sets
// its Helmholtz resonance near 12 Hz, twice the tremulant's beat, as a builder sets a trunk up for it.
struct DivisionWind {
    static constexpr double kAr = 0.6, kXWork = 0.08, kXMax = 0.15, kInletMax = 0.03, kInletSpan = 0.04,
                            kXRelief = 0.12, kReliefMax = 0.01;   // the waste (safety) valve: opens as the lid rises past 120 mm, 0.01 m^2 at the top
    static constexpr double kTrunkLen = 3.0, kTrunkS = 0.03, kTrunkK = 1.5, kChestV = 0.21, kCd = 0.61, kChestQ = 2.0;
    double Pnom = 774.7;          // the division's pressure (Pa)
    double regMass = 20, regZeta = 0.3;
    double concussion = 0.4;      // the chest's concussion bellows, 0 (none or tied down) .. 1 (the largest)
    double Pb = 0;                // the air in the concussion bellows (its spring's pressure), Pa
    double leakM3 = 4.7e-4;       // the chest's leakage at its working pressure, m^3/s (WindSystem::update)
    // state
    double Pr = 0, x = kXWork, v = 0;   // regulator: pressure, lid height, lid speed
    double q = 0, qAvg = 0;             // trunk flow, m^3/s, and its steady part
    double Pc = 0;                      // chest pressure, Pa
    double inSum = 0, outSum = 0, pcSum = 0, pcMean = 0; int nSum = 0;   // what came in from the reservoir / went to the grooves, over the block
    // tremulant valve on this division (Tremulant.hpp sets kind, rate, depth)
    double tx = 0.02, tv = 0, tAmt = 0, tAvg = 0;

    void reset(double Pres) {
        Pr = Pc = Pb = std::min(Pres, Pnom); x = Pres >= Pnom ? kXWork + 0.5 * kInletSpan : 0.0; v = 0; q = qAvg = 0; tx = 0.02; tv = 0; tAmt = 0; tAvg = Pc;
        inSum = outSum = pcSum = 0; pcMean = Pc; nSum = 0;   // all of it: a reset wind is the wind of a new organ
    }
    // One sample. Pres: the main reservoir's pressure (Pa); qOut: what the grooves' pallets and the chest's
    // other pipes take this sample (m^3/s). rho, c: the air. The tremulant: its kind, rate, depth, on.
    void step(double dt, double Pres, double qOut, double rho, double c, const Tremulant* trem, bool tremOn) {
        const double rc2 = rho * c * c;
        // regulator: the lid carries its weights; the curtain valve opens as it falls below its working height
        const double open = std::max(0.0, std::min(1.0, (kXWork + 0.5 * kInletSpan - x) / kInletSpan));
        // the inlet: an opening, not a check valve (wind goes back through it when the regulator stands above the
        // reservoir). Its flow is solved with the regulator's pressure below, implicitly: sqrt(dP) is infinitely
        // stiff at dP = 0, and an empty regulator (its lid on its stops, a litre of air) stepped explicitly rang
        // and pumped itself to twice its supply as the wind failed (the pipes jumping register).
        const double G = open > 0 ? kCd * kInletMax * open * std::sqrt(2.0 / rho) : 0.0;
        double qin = 0;
        // the lid rides on the air under it (a spring of rho c^2 Ar^2 / V): its damping is regZeta of critical at
        // its working height, where it rides. (Taken at the lid's own height it grew eightfold near the bottom, and an
        // empty regulator took 8 s to rise when the blower started, its chest at the reservoir's 93 mm meanwhile.)
        const double kAir = rc2 * kAr * kAr / (kAr * kXWork);
        const double force = (Pr - Pnom) * kAr - 2.0 * regZeta * std::sqrt(kAir * regMass) * v;
        v += dt * force / regMass;
        x += dt * v;
        if (x < 0.002) { x = 0.002; if (v < 0) v = 0; }
        if (x > kXMax) { x = kXMax; if (v > 0) v = 0; }
        const double Vr = kAr * x;
        // trunk: the air's inertia and its losses; the doux flap sits in it
        const double L = rho * kTrunkLen / kTrunkS;
        double qPiston = 0, qVent = 0;
        if (trem) {
            const double tw = 2.0 * M_PI * trem->rate, Q0 = trem->unitFlowM3;
            tAmt += ((tremOn ? 1.0 : 0.0) - tAmt) * std::min(1.0, 8.0 * dt);
            tAvg += (Pc - tAvg) * std::min(1.0, 2.0 * dt);
            if (trem->kind == kTremDoux) {
                const double mu = 0.35 * tAmt;
                tv += dt * (-tw * tw * tx + mu * tw * (1.0 - tx * tx) * tv - trem->kDouxBack * tw * tw * (Pc - tAvg) / Pnom / std::max(1e-3, (double)trem->depth));
                tx += dt * tv;
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
        // losses: the trunk's turbulence, and the damping of the trunk and chest's resonance by the chest's
        // wooden walls, its slides' and pallets' leakage and the wind conductor's linings, taken together as a
        // resistance that leaves that resonance a Q of kChestQ. ASSUMPTION: kChestQ = 2 (no measurement of a
        // chest's wind resonance to hand; with the turbulent loss alone it rang for seconds and the reeds,
        // which draw less as their pressure rises, kept it going).
        const double Z0 = std::sqrt(L * rc2 / kChestV);
        const double loss = kTrunkK * 0.5 * rho / (kTrunkS * kTrunkS) * std::fabs(q);
        qAvg += (q - qAvg) * std::min(1.0, 2.0 * M_PI * 2.0 * dt);   // the steady flow (below 2 Hz): the damping acts on what moves
        q += dt * (Pr - Pc - loss * q - Z0 / kChestQ * (q - qAvg)) / L;
        // the waste valve: a lid that overshoots its working height (a sudden release of a full chord) lifts
        // it and spills the excess rather than holding the regulator's air above its weights
        double qWaste = 0;
        if (x > kXRelief) qWaste = kCd * kReliefMax * std::min(1.0, (x - kXRelief) / (kXMax - kXRelief)) * std::sqrt(2.0 * Pr / rho);
        // the regulator's air and the chest's air (their springiness)
        {
            // Pr' = Pr + k (G sgn(u) sqrt|u| - o), u = Pres - Pr' (backward Euler in the inlet): u + k G sgn(u) sqrt|u| = R
            const double k = dt * rc2 / Vr, o = q + qWaste + kAr * v, R = Pres - Pr + k * o, a = k * G;
            const double r = (-a + std::sqrt(a * a + 4.0 * std::fabs(R))) / 2.0;   // sqrt|u|
            const double u = R >= 0 ? r * r : -r * r;
            qin = (u >= 0 ? 1.0 : -1.0) * G * r;
            Pr = std::max(0.0, Pres - u);
        }
        // the concussion bellows on the chest: a spring-loaded bellows whose leather and hinges give it a
        // resistance; it takes in a surge (a big chord let go, the trunk's wind still coming) and gives it back
        // slowly. ASSUMPTION: at full size its springs give it 4 x the chest's own compliance, its resistance
        // the trunk-chest resonance's own impedance (so it takes up a shock without ringing).
        double qCb = 0;
        if (concussion > 1e-3) {
            const double Cc = concussion * 4.0 * kChestV / rc2;
            qCb = (Pc - Pb) / Z0;
            Pb += dt * qCb / Cc;
        } else Pb = Pc;
        // the chest's leaks: past its sliders and pallets and through its joints, laminar (as the pressure).
        // As much as the Wind Leakage setting lets out of the reservoir (a share of the blower's output at the
        // working pressure) leaks from each chest too. With the blower off this is what lets the chest's wind
        // run out once the pipes have taken all they can.
        const double qLeak = leakM3 * std::max(0.0, Pc) / Pnom;
        Pc += dt * rc2 / kChestV * (q - qOut - qVent + qPiston - qCb - qLeak);
        Pc = std::max(0.0, Pc);
        inSum += qin; outSum += qOut; pcSum += Pc; ++nSum;
    }
    // what the regulator drew from the reservoir over the block (m^3/s), and reset
    // (and the chest's mean pressure over the block, pcMean)
    double takeInflow() { const double r = nSum ? inSum / nSum : 0.0; pcMean = nSum ? pcSum / nSum : Pc; inSum = outSum = pcSum = 0; nSum = 0; return r; }
};

enum ReservoirLoad { kLoadWeights = 0, kLoadSprings, kNumReservoirLoads };
static const char* const kReservoirLoadLabels[kNumReservoirLoads] = { "WEIGHTS", "SPRINGS" };

struct WindSystem {
    // settings
    float reservoirTarget = 96.f;   // pressure under the lid at its working height, mm
    float flexibility = 0.7f;       // 0 steady .. 1 flexible
    float concussion = 0.4f;
    bool blowerOn = true;
    int load = kLoadSprings;
    float sizeSeconds = 12.f;       // reservoir volume, in seconds of the blower's full output
    float spinDown = 8.f;           // blower coast-down time constant, s
    float spinUp = 2.5f;            // blower run-up time constant, s
    float leak = 0.004f;            // leakage at working pressure, fraction of blower output
    float tremSpeed = 5.8f, tremDepth = 0.22f;
    Tremulant trem;
    float tremLoad = 0.f;
    DivisionWind div[kNumDivisions];
    int tremTarget = kSwell;
    bool tremOn = false;

    // geometry of the reservoir
    static constexpr float kWorkHeight = 0.8f;   // lid height (fraction of full travel) where the inlet valve shuts
    static constexpr float kStops = 0.025f;      // last part of the travel: lid on its stops, folds closing
    static constexpr float kSpringAtBottom = 0.12f;   // spring load at the bottom, relative to working height (springs nearly slack)
    static constexpr float kFanHead = 1.35f;     // blower's shut-off pressure, relative to the reservoir's working pressure

    // state
    float lid = 0.f;            // lid height, 0 (empty) .. 1 (full travel)
    float fan = 0.f;            // blower speed, 0 .. 1
    float reservoir = 0.f;      // pressure under the lid (with sag), mm
    float sag = 0.f, sagVel = 0.f;
    float chest[kNumDivisions] = {};
    float demand = 0.f;         // last demand, relative to blower capacity
    float secondsLeft = -1.f;   // how long the stored wind lasts at today's use (-1: blower keeping up)

    // Pressure under the lid at height h.
    float lidPressure(float h) const {
        const float loadAt = load == kLoadWeights ? 1.f
            : kSpringAtBottom + (1.f - kSpringAtBottom) * std::min(1.f, h / kWorkHeight) + (h > kWorkHeight ? 0.3f * (h - kWorkHeight) : 0.f);
        return reservoirTarget * loadAt * std::min(1.f, h / kStops);
    }

    void reset() {
        fan = blowerOn ? 1.f : 0.f;
        lid = blowerOn ? kWorkHeight : 0.f;
        reservoir = lidPressure(lid);
        sag = sagVel = 0.f; demand = 0.f; tremLoad = 0.f;
        for (int d = 0; d < kNumDivisions; ++d) { chest[d] = blowerOn ? std::min(kChestNominal[d], reservoir) : 0.f; div[d].Pnom = kChestNominal[d] * 9.80665; div[d].reset(reservoir * 9.80665); }
        trem.reset();
        secondsLeft = -1.f;
    }

    // Advance by dt seconds given total flow demand (pipe flow units).
    // divFlow: each division's share of it (for the tremulant's load), or null.
    void update(float dt, float flowDemand, const float* divFlow = nullptr) {
        demand = flowDemand / kBlowerCapacity;
        // blower runs up or coasts down
        fan += ((blowerOn ? 1.f : 0.f) - fan) * std::min(1.f, dt / (blowerOn ? spinUp : spinDown));
        if (!blowerOn && fan < 1e-3f) fan = 0.f;

        const float P = lidPressure(lid);
        const float Pw = std::max(1.f, reservoirTarget);
        // Blower: a fan's pressure goes with its speed squared; flow through it with the
        // square root of the pressure it has to spare. The inlet valve closes as the lid
        // reaches its working height. A check valve keeps the wind from running back out.
        const float head = kFanHead * Pw * fan * fan;
        const float valve = std::max(0.f, std::min(1.f, (kWorkHeight + 0.03f - lid) / 0.06f));
        const float spare = std::max(0.f, head - P) / ((kFanHead - 1.f) * Pw);
        const float inflow = 1.15f * fan * std::sqrt(spare) * valve;
        // Outflow: the pipes (their demand already follows their foot pressure) and leaks.
        const float leakFlow = leak * std::sqrt(std::max(0.f, P) / Pw);
        const float net = inflow - demand - leakFlow;
        // In the last part of the travel the lid sits on its stops: the space left is
        // small and the pressure follows the air within it.
        lid = std::max(0.f, std::min(1.f, lid + net * dt / std::max(0.5f, sizeSeconds)));
        const float outflow = demand + leakFlow;
        secondsLeft = (net < -1e-4f && outflow > 1e-4f) ? lid * sizeSeconds / std::max(1e-4f, outflow - inflow) : -1.f;

        // Lid inertia: the reservoir sags under sudden demand and bounces back.
        const float flex = flexibility;
        const float depth = (0.015f + 0.11f * flex) * (1.f - 0.6f * concussion);
        const float omega = 2.f * (float)M_PI * (1.5f + 2.5f * (1.f - flex));
        const float zeta = 0.25f + 0.9f * (1.f - flex) + 0.6f * concussion;
        const float drive = std::min(demand, 2.f) * depth * std::min(1.f, lid / kStops);
        const float acc = omega * omega * (drive - sag) - 2.f * zeta * omega * sagVel;
        sagVel += acc * dt;
        sag += sagVel * dt;

        reservoir = lidPressure(lid) * (1.f - sag);

        // each division's regulator, trunk, chest and tremulant: DivisionWind, stepped at audio rate by the
        // engine with the grooves on the chest (Engine::renderGrooves); the regulators' flexibility:
        for (int d = 0; d < kNumDivisions; ++d) {
            div[d].Pnom = kChestNominal[d] * 9.80665;
            div[d].regMass = 8.0 + 40.0 * flexibility;                       // a heavier lid answers slower
            div[d].regZeta = 0.08 + 0.7 * (1.0 - flexibility);
            div[d].concussion = concussion;
            div[d].leakM3 = (double)leak * kBlowerCapacity * 4.5e-4;   // the blower's output in m^3/s (kRefFlow 4.5e-4 per flow unit)
        }
    }
};

} // namespace pipe
