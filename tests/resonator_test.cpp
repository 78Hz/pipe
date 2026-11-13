// Pipe - resonators computed from geometry, checked against published measurements.
#include "engine/Resonator.hpp"
#include <cstdio>
using namespace pipe::resonator;
static int failures = 0;
static void check(bool ok, const char* what, double got, double want) {
    std::printf("  %s  %-58s %9.3f (measured %.3f)\n", ok ? "ok  " : "FAIL", what, got, want);
    if (!ok) ++failures;
}
int main() {
    Air air; air.tempC = 20;
    // Szoliva, Wik, Angster, Miklos & Augusztinovicz, Forum Acusticum 2005, pipe No. 1:
    // open metal flue pipe, body 1190 mm, diameter 82 mm, cut-up 17 mm, mouth width 64 mm.
    FlueGeometry g; g.body = { { 0.041, 0.041, 1.190 } }; g.mouthWidth = 0.064; g.cutup = 0.017; g.wall = 0.0008;
    auto m = findModes([&](double w) { return flueAdmittance(w, g, air); }, 40, 1500, 6);
    std::printf("Szoliva et al. 2005, pipe No. 1 (air at 20 C; the paper gives no temperature)\n");
    check(m.size() >= 5 && std::fabs(m[0].f / 126.88 - 1) < 0.02, "fundamental (Hz), within 2 %", m[0].f, 126.88);
    const double meas[4] = { 2.029, 3.099, 4.150, 5.241 };
    for (int i = 0; i < 4 && i + 1 < (int)m.size(); ++i) {
        char w[64]; std::snprintf(w, sizeof w, "mode %d / mode 1, within 1 %%", i + 2);
        check(std::fabs(m[(size_t)i + 1].f / m[0].f / meas[i] - 1) < 0.01, w, m[(size_t)i + 1].f / m[0].f, meas[i]);
    }
    // A conical reed resonator closed at its narrow end resonates like an open cylinder of its
    // length: Pasi Opus 14 Trumpet 8' c1 (11 -> 70 mm over 540 mm) should resonate near c1.
    air.tempC = 18;
    ReedGeometry r; r.resonator = { { 0.0055, 0.035, 0.540 } };
    auto rm = findModes([&](double w) { return reedImpedance(w, r, air); }, 60, 1000, 2);
    std::printf("Pasi Opus 14 Trumpet 8' c1 resonator\n");
    check(!rm.empty() && std::fabs(rm[0].f / 261.63 - 1) < 0.02, "first resonance (Hz) near the note, within 2 %", rm.empty() ? 0 : rm[0].f, 261.63);
    std::printf(failures ? "%d FAILED\n" : "all resonator checks passed\n", failures);
    return failures ? 1 : 0;
}
