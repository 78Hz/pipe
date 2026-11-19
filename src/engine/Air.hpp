// Pipe - physically modelled pipe organ
// The air the organ breathes: one state for the whole instrument (the blower draws it from the room, the
// bellows hold it, every pipe speaks with it). Temperature, humidity and barometric pressure give the air's
// density, speed of sound and viscosity, and from those every pipe's resonances (speed of sound), its jet
// and reed flow (density), and its windway friction (viscosity).
//
//  - water vapour: saturation pressure over water, Buck (1981, the 1996 coefficients); mole fraction
//    x_w = RH p_sat / p (the enhancement factor, ~1.004, is left out)
//  - density: ideal gas of the moist mixture, rho = p M / (R T), M = (1 - x_w) M_d + x_w M_w
//    (CIPM-2007 gives the same to ~0.05 % at room conditions: its compressibility Z ~ 0.9996)
//  - speed of sound: c = sqrt(gamma R T / M) with gamma of the mixture from its molar heat capacities
//    (dry air cv = 2.5 R, water vapour cv = 3.04 R at room temperature); at 20 C, 50 % RH, 101.325 kPa this
//    gives 343.9 m/s (Cramer 1993: 344.0); dry air at 20 C 343.2, at 0 C 331.3
//  - viscosity: Sutherland's law for air (mu0 1.716e-5 Pa s at 273.15 K, S = 110.4 K); humidity changes it
//    by < 1 % at room conditions and is left out
// The pipes were voiced (the voicer's resonator and jet models) in dry air at 20 C and 101.325 kPa: the
// reference the air's effect is measured from.
#pragma once

#include <cmath>
#include <algorithm>

namespace pipe {

struct AirState {
    double tempC = 20.0, humidity = 0.0, pressureKPa = 101.325;
    double rho = 1.2041, c = 343.21, mu = 1.8134e-5;

    static double saturationPa(double tC) {   // Buck (1996), over water
        return 611.21 * std::exp((18.678 - tC / 234.5) * (tC / (257.14 + tC)));
    }
    void update() {
        constexpr double R = 8.314462618, Md = 0.0289647, Mw = 0.01801528;
        const double T = 273.15 + tempC, p = pressureKPa * 1000.0;
        const double xw = std::max(0.0, std::min(0.2, humidity / 100.0 * saturationPa(tempC) / p));
        const double M = (1.0 - xw) * Md + xw * Mw;
        const double cv = (1.0 - xw) * 2.5 + xw * 3.04;    // in units of R
        const double gamma = (cv + 1.0) / cv;
        rho = p * M / (R * T);
        c = std::sqrt(gamma * R * T / M);
        mu = 1.716e-5 * std::pow(T / 273.15, 1.5) * (273.15 + 110.4) / (T + 110.4);
    }
    static AirState make(double tC, double rh, double kPa) { AirState a; a.tempC = tC; a.humidity = rh; a.pressureKPa = kPa; a.update(); return a; }
    // the air the pipes were voiced in
    static const AirState& voiced() { static const AirState a = make(20.0, 0.0, 101.325); return a; }
};

// What a pipe needs from the air, relative to the air it was voiced in.
struct PipeAir {
    double rhoRatio = 1.0;   // density: jet and reed flow, the windway's inertia
    double cRatio = 1.0;     // speed of sound: the resonances scale with it
    double muRatio = 1.0;    // viscosity: windway friction
    double zRatio() const { return rhoRatio * cRatio; }   // rho c: the resonator's impedance
    static PipeAir of(const AirState& a) {
        const AirState& v = AirState::voiced();
        PipeAir p; p.rhoRatio = a.rho / v.rho; p.cRatio = a.c / v.c; p.muRatio = a.mu / v.mu;
        return p;
    }
    bool operator==(const PipeAir& o) const { return rhoRatio == o.rhoRatio && cRatio == o.cRatio && muRatio == o.muRatio; }
    bool operator!=(const PipeAir& o) const { return !(*this == o); }
};

} // namespace pipe
