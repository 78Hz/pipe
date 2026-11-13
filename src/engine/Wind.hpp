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
#include <cmath>
#include <algorithm>

namespace pipe {

constexpr float kChestNominal[kNumDivisions] = { 94.f, 78.f, 85.f };   // mm water
constexpr float kBlowerCapacity = 260.f;   // flow units (Principal 8' C4 = 1)

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
    float chest[kNumDivisions] = { 0.f, 0.f, 0.f };
    float tremPhase = 0.f;
    float tremAmt = 0.f;        // smoothed on/off
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
        sag = sagVel = 0.f;
        for (int d = 0; d < kNumDivisions; ++d) chest[d] = blowerOn ? std::min(kChestNominal[d], reservoir) : 0.f;
        tremPhase = tremAmt = 0.f;
        secondsLeft = -1.f;
    }

    // Advance by dt seconds given total flow demand (pipe flow units).
    void update(float dt, float flowDemand) {
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
        // tremulant: a beating valve that pulses the chest pressure
        tremAmt += ((tremOn ? 1.f : 0.f) - tremAmt) * std::min(1.f, 6.f * dt);
        tremPhase += tremSpeed * dt;
        if (tremPhase >= 1.f) tremPhase -= 1.f;
        const float trem = tremAmt * tremDepth * 0.5f * std::sin(2.f * (float)M_PI * tremPhase);

        for (int d = 0; d < kNumDivisions; ++d) {
            // the regulator holds its set pressure while the reservoir is above it (less a
            // small drop through the trunk), and passes the reservoir through below that
            float p = std::min(kChestNominal[d], reservoir * 0.985f);
            p *= 1.f - 0.6f * sag;      // flexible wind: part of the lid's bounce comes through
            if (d == tremTarget) p *= 1.f + trem;
            chest[d] += (std::max(0.f, p) - chest[d]) * std::min(1.f, 60.f * dt);
        }
    }
};

} // namespace pipe
