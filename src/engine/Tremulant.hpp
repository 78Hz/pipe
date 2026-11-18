// Pipe - physically modelled pipe organ
// Tremulant: a valve in the wind of one division, working against the trunk and the chest.
//
// Two kinds (Dom Bedos; Shannon, "Understanding the Pipe Organ"; Pykett, "Tremulant simulation in
// digital organs"):
//  - tremblant doux (retained wind): a hinged flap across the trunk, held open by a spring and a
//    lead weight. The wind blows it towards closing; as it closes the chest pressure falls, the flow
//    behind it builds up in the trunk and pushes it open again. Nothing is let out of the organ. It
//    gives a gentle, nearly sinusoidal beat, and its effect weakens as more stops are drawn (the
//    pipes drawing wind damp the trunk-chest resonance it works through) and can vanish at full organ.
//  - tremblant a vent perdu (spilled wind, "tremblant fort"): a weighted pallet on the chest's wind
//    that opens outward when the pressure lifts it and lets wind escape; the pressure falls, the
//    pallet drops back, the pressure rebuilds. Deeper and less sinusoidal; up to about 75 % peak to
//    peak at its strongest (Pykett: +-30 mm about 80 mm in a church organ).
// The rate is the valve's own (its weight and spring: Pykett); the trunk and chest are a Helmholtz
// pair tuned near it, as a builder sets a tremulant up (Pykett: the rate has to agree with a
// resonance of the wind system).
//
// Units: pressure relative to the division's nominal pressure, flow relative to the trunk's flow at
// the nominal pressure drop (q = 1), time in seconds.
#pragma once

#include <cmath>
#include <algorithm>

namespace pipe {

enum TremKind { kTremDoux = 0, kTremVentPerdu, kNumTremKinds };
static const char* const kTremKindLabels[kNumTremKinds] = { "DOUX", "VENT PERDU" };

struct Tremulant {
    // settings
    int kind = kTremDoux;
    float rate = 5.8f;     // the valve's natural frequency, Hz (its weight and spring)
    float depth = 0.22f;   // the valve's size (doux: how far the flap can close; vent perdu: the pallet's area), 0..0.5
    // wind system (seconds): chest compliance and trunk inertance, as time constants of the unit flow
    float tauL = 0.0f;   // set from the rate
    float rTrunk = 0.05f;               // trunk's pressure drop at unit flow with the flap open

    // state
    float p = 1.f, q = 0.f, x = 0.f, v = 0.f;
    float amt = 0.f;   // smoothed on/off: the stop's action engaging the valve

    void reset(float p0 = 1.f) { p = pAvg = p0; q = 0.f; x = 0.02f; v = 0.f; amt = 0.f; }

    // One step. pr: supply pressure from the regulator (relative); d: the division's pipes'
    // conductance (flow at p = 1, relative to the trunk's unit flow); on: tremulant drawn.
    // Returns the chest pressure (relative).
    float step(float dt, float pr, float d, bool on) {
        amt += ((on ? 1.f : 0.f) - amt) * std::min(1.f, 8.f * dt);
        const float w = 2.f * (float)M_PI * rate;
        // the trunk: its inertance makes the chest's Helmholtz frequency (with the chest's compliance)
        // 1.6 x the tremulant's, so it is set up to work through it without ringing at the beat itself
        tauL = kTrunkReact / w;
        const float wh = (kind == kTremDoux ? kHelmDoux : kHelmVent) * w; const float tc = 1.f / (wh * wh * tauL);
        const int sub = std::max(1, (int)std::ceil(dt * 6000.f));
        const float h = dt / (float)sub;
        for (int i = 0; i < sub; ++i) {
            const float qp = d * std::sqrt(std::max(0.f, p));
            float qv = 0.f, qpist = 0.f;
            pAvg += (p - pAvg) * std::min(1.f, h * 2.f);   // the chest's mean pressure (over half a second)
            if (kind == kTremDoux) {
                // the flap swinging in the trunk sweeps air into and out of the chest (a piston of its area)
                qpist = kDouxArea * depth * amt * v / w;
                // a self-sustaining swing (the wind's push on the flap, the spring and weight bring it back),
                // loaded by the chest's pressure on its face
                const float mu = 0.35f * amt;
                v += h * (-w * w * x + mu * w * (1.f - x * x) * v - kDouxBack * w * w * (p - pAvg) / std::max(1e-3f, depth));
                x += h * v;
            } else {
                // the pallet lifted by the chest's pressure against its weight lets wind out
                const float lift = std::max(0.f, x);
                qv = kVentArea * depth * amt * lift * std::sqrt(std::max(0.f, p));
                const float force = kVentSeat + kVentGain * (p - pAvg);   // the pallet rides open by kVentSeat, lifted further as the pressure rises
                const float wv = kVentRate * w;
                v += h * (wv * wv * (force - x) - 2.f * kVentZeta * wv * v);
                x += h * v;
                if (x < 0.f) { x = 0.f; if (v < 0.f) v = 0.f; }   // the pallet on its seat
            }
            q += h * (pr - p - rTrunk * q * std::fabs(q) - kTrunkLin * (q - qAvg)) / tauL;
            qAvg += (q - qAvg) * std::min(1.f, h * 1.5f);
            p += h * (q - qp - qv + qpist) / tc;
            p = std::max(0.f, p);
        }
        return p;
    }
    float pAvg = 1.f, qAvg = 0.f;
    static constexpr float kTrunkReact = 2.0f;     // the trunk's reactance at the beat, relative (pressure / unit flow)
    float kDouxArea = 0.074f;      // swept flow at full swing, per unit of depth
    float kDouxBack = 0.02f;
    float kHelmDoux = 1.6f, kHelmVent = 0.5f;   // doux: works below the trunk-chest resonance; vent perdu: above it (the chest's compliance gives the lag)
    float kVentRate = 0.79f;   // the pallet's own frequency / the beat it gives (the loop pulls it up)
    float kTrunkLin = 0.6f;   // the trunk's viscous loss (pressure / unit flow): damps its Helmholtz ring
    float kVentArea = 1.1f, kVentGain = 4.f, kVentSeat = 0.5f, kVentZeta = 0.1f;
};

} // namespace pipe
