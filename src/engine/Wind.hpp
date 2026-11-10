// Pipe - physically modelled pipe organ
// Wind system: blower -> main reservoir -> regulator per division -> chest.
// Demand from the speaking pipes makes the wind sag and bounce, as with a
// real (flexible) wind supply; the tremulant pulses a chest's pressure.
#pragma once

#include "Stoplist.hpp"
#include <cmath>
#include <algorithm>

namespace pipe {

constexpr float kChestNominal[kNumDivisions] = { 94.f, 78.f, 85.f };   // mm water
constexpr float kBlowerCapacity = 260.f;   // flow units (Principal 8' C4 = 1)

struct WindSystem {
    // settings
    float reservoirTarget = 96.f;
    float flexibility = 0.7f;   // 0 steady .. 1 flexible
    float concussion = 0.4f;
    bool blowerOn = true;
    float tremSpeed = 5.8f, tremDepth = 0.22f;
    int tremTarget = kSwell;
    bool tremOn = false;

    // state
    float reservoir = 0.f;      // mm
    float sag = 0.f, sagVel = 0.f;
    float chest[kNumDivisions] = { 0.f, 0.f, 0.f };
    float tremPhase = 0.f;
    float tremAmt = 0.f;        // smoothed on/off
    float demand = 0.f;         // last demand, relative to blower capacity

    void reset() {
        reservoir = blowerOn ? reservoirTarget : 0.f;
        sag = sagVel = 0.f;
        for (int d = 0; d < kNumDivisions; ++d) chest[d] = blowerOn ? kChestNominal[d] : 0.f;
        tremPhase = tremAmt = 0.f;
    }

    // Advance by dt seconds given total flow demand.
    void update(float dt, float flowDemand) {
        demand = flowDemand / kBlowerCapacity;
        // Blower and reservoir weights bring the reservoir to its target.
        const float target = blowerOn ? reservoirTarget : 0.f;
        const float fill = blowerOn ? 4.0f : 0.6f;             // 1/s
        reservoir += (target - reservoir) * std::min(1.f, fill * dt);
        // Overload: more wind drawn than the blower can give drains the reservoir.
        if (demand > 1.f && blowerOn) reservoir -= (demand - 1.f) * 40.f * dt;
        reservoir = std::max(0.f, reservoir);

        // Lid inertia: the reservoir sags under sudden demand and bounces back.
        const float flex = flexibility;
        const float depth = (0.015f + 0.11f * flex) * (1.f - 0.6f * concussion);
        const float omega = 2.f * (float)M_PI * (1.5f + 2.5f * (1.f - flex));
        const float zeta = 0.25f + 0.9f * (1.f - flex) + 0.6f * concussion;
        const float drive = std::min(demand, 2.f) * depth;
        const float acc = omega * omega * (drive - sag) - 2.f * zeta * omega * sagVel;
        sagVel += acc * dt;
        sag += sagVel * dt;

        const float resEff = reservoir * (1.f - sag);
        // tremulant: a beating valve that pulses the chest pressure
        tremAmt += ((tremOn ? 1.f : 0.f) - tremAmt) * std::min(1.f, 6.f * dt);
        tremPhase += tremSpeed * dt;
        if (tremPhase >= 1.f) tremPhase -= 1.f;
        const float trem = tremAmt * tremDepth * 0.5f * std::sin(2.f * (float)M_PI * tremPhase);

        for (int d = 0; d < kNumDivisions; ++d) {
            // regulator holds nominal pressure when the reservoir is higher,
            // and passes through part of the reservoir's movement
            float p = std::min(kChestNominal[d], resEff * kChestNominal[d] / std::max(1.f, reservoirTarget) * 1.08f);
            p *= 1.f - 0.6f * sag;
            if (d == tremTarget) p *= 1.f + trem;
            chest[d] += (p - chest[d]) * std::min(1.f, 60.f * dt);
        }
    }
};

} // namespace pipe
