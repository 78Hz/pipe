// Pipe - the Opus 14 data tables are complete and say what the Pasi sheets say.
// Spot values are typed here a second time from the sheets, independently of data/opus14.
#include "engine/Opus14Data.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
using namespace pipe::opus14;
static int failures = 0;
static void check(bool ok, const char* what) { std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++failures; }
static const Stop* find(Division d, const char* name) {
    for (const Stop& s : kStops) if (s.div == d && !std::strcmp(s.name, name)) return &s;
    return nullptr;
}
static const Row* row(const Stop* s, const char* label) {
    for (int i = 0; s && i < s->nRows; ++i) if (!std::strcmp(s->rows[i].label, label)) return &s->rows[i];
    return nullptr;
}
int main() {
    std::printf("Opus 14 tables\n");
    // Every speaking stop of the stop list with pipes of its own (V1 p.253) has a table. The Pedal
    // octave transmissions (Subbass 16' has its own table; Octave 8' has its own table; Posaune 16'
    // has its own table) and Pedal Gedeckt/Bourdon 8' are included; Cornet V and Sesquialtera II are
    // tabled per rank.
    const char* hw[] = { "Praestant 16'", "Octave 8'", "Rohrflote 8'", "Salicional 8'", "Octave 4'", "Spitzflote 4'",
        "Quinte 2 2/3'", "Superoctave 2'", "Mixture 1 1/3' V", "Mixture 1' IV", "Cornet V 8' rank", "Cornet V 4' rank",
        "Cornet V 2 2/3' rank", "Cornet V 2' rank", "Cornet V 1 3/5' rank", "Trumpet 16'", "Trumpet 8'", "Trompette 8'", "Vox Humana 8'" };
    const char* ow[] = { "Praestant 8'", "Suavial 8'", "Gedeckt 8'", "Octave 4'", "Rohrflote 4'", "Octave 2'", "Waldflote 2'",
        "Sesquialtera II 2 2/3' rank", "Sesquialtera II 1 3/5' rank", "Quinte 1 1/3'", "Mixture 1' IV", "Dulzian 16'", "Trichterregal 8'" };
    const char* uw[] = { "Bourdon 16'", "Principal 8'", "Gamba 8'", "Celeste 8'", "Harmonic Flute 8'", "Principal 4'", "Harmonic Flute 4'",
        "Nazard 2 2/3'", "Octavin 2'", "Tierce 1 3/5'", "Mixture V 2'", "Bassoon 16'", "Trompette 8'", "Oboe 8'", "Clairon 4'" };
    const char* pd[] = { "Subbass 32'", "Praestant 16' (wood Haskell)", "Subbass 16'", "Octave 8'", "Gedeckt 8'", "Octave 4'",
        "Mixture V 2 2/3'", "Trombone 32'", "Posaune 16'", "Trumpet 8'", "Trompette 8'", "Clairon 4'", "Cornet 2'" };
    int found = 0, want = 0;
    for (auto n : hw) { ++want; found += find(kHW, n) != nullptr; }
    for (auto n : ow) { ++want; found += find(kOW, n) != nullptr; }
    for (auto n : uw) { ++want; found += find(kUW, n) != nullptr; }
    for (auto n : pd) { ++want; found += find(kPed, n) != nullptr; }
    char buf[96]; std::snprintf(buf, sizeof buf, "all %d ranks of the stop list are tabled (%d found, %d tables)", want, found, kNumStops);
    check(found == want && kNumStops == want, buf);

    // spot values from the sheets
    const Row* d = row(find(kHW, "Octave 8'"), "Diameter");
    check(d && d->v[0] == 155.f && d->v[2] == 50.f && d->v[5] == 9.f, "HW Octave 8' diameters C 155, c1 50, c4 9");
    const Row* cu = row(find(kUW, "Gamba 8'"), "Toe Hole");
    check(cu && cu->v[1] == 7.5f && std::isnan(cu->v[5]), "UW Gamba toe holes c0 7.5, c4 not given");
    const Row* b = row(find(kUW, "Bourdon 16'"), "Diameter");
    check(b && b->v[0] == 145.f && b->alt[0] == 175.f && b->v[1] == 105.f, "UW Bourdon C rectangular 145 x 175");
    const Row* mw = row(find(kUW, "Bourdon 16'"), "Mouth Width");
    check(mw && mw->v[0] == 145.f && mw->v[1] == 0.25f, "UW Bourdon mouth: 145 mm at C, 1/4 above");
    const Row* l = row(find(kUW, "Bassoon 16'"), "Length");
    check(l && l->v[1] == 1110.f && l->alt[1] == 2250.f, "UW Bassoon c0 length 1110 / 2250");
    const Stop* tr = find(kOW, "Trichterregal 8'");
    check(tr && tr->nCols == 7 && tr->key[3] == 64, "OW Trichterregal has an e1 column");
    const Row* tt = row(find(kHW, "Trumpet 8'"), "Tongue Thickness");
    check(tt && tt->v[2] == 0.25f, "HW Trumpet 8' tongue c1 printed 1/4 -> 0.25");
    const Row* sb = row(find(kPed, "Subbass 32'"), "Diameter");
    const Row* s16 = row(find(kPed, "Subbass 16'"), "Diameter");
    check(sb && s16 && sb->v[1] == s16->v[0] && sb->alt[1] == s16->alt[0], "Subbass 32' c0 = Subbass 16' C (194 x 228)");
    check(std::fabs(find(kHW, "Quinte 2 2/3'")->feet - 8.f / 3.f) < 1e-5f && find(kUW, "Tierce 1 3/5'")->feet == 1.6f, "footages parsed");

    // Diameters shrink up the compass (every metal flue on the sheets; wood given as width x depth).
    int bad = 0;
    for (const Stop& s : kStops) {
        const Row* r = row(&s, s.kind == kFlue ? "Diameter" : "Upper Diameter");
        if (!r) continue;
        for (int i = 1; i < s.nCols; ++i) if (!std::isnan(r->v[i]) && !std::isnan(r->v[i - 1]) && r->v[i] > r->v[i - 1]) { ++bad; std::printf("    rises: %s col %d\n", s.name, i); }
    }
    check(bad == 0, "diameters never rise up the compass");

    // Mixtures: ranks at each break are listed highest pitch first and the breaks only rise.
    int mbad = 0;
    for (const Mixture& m : kMixtures)
        for (int b2 = 0; b2 < m.nBreaks; ++b2) {
            for (int r = 1; r < m.breaks[b2].nRanks; ++r) if (m.breaks[b2].feet[r] >= m.breaks[b2].feet[r - 1]) ++mbad;
            if (b2 && m.breaks[b2].key <= m.breaks[b2 - 1].key) ++mbad;
        }
    check(kNumMixtures == 5 && mbad == 0, "5 mixtures; ranks ordered; breaks rise");

    std::printf(failures ? "%d FAILED\n" : "all Opus 14 table checks passed\n", failures);
    return failures ? 1 : 0;
}
