// Loads the built OpusXIV.clap and checks, through the real binary, how CLAP note events and note
// expressions reach the notes (CLAP events.h: voices by note_id or port/channel/key, -1 = wildcard).
#include "entry.h"
#include "plugin-factory.h"
#include "plugin.h"
#include "ext/params.h"
#include "ext/state.h"
#include "ext/note-ports.h"
#include "events.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

static const void* hostExt(const clap_host_t*, const char*) { return nullptr; }
static void hostNoop(const clap_host_t*) {}
static clap_host_t host = { CLAP_VERSION, nullptr, "test", "test", "", "1", hostExt, hostNoop, hostNoop, hostNoop };

union AnyEvent { clap_event_header_t header; clap_event_midi_t midi; clap_event_note_t note; clap_event_note_expression_t expr; clap_event_param_value_t param; };
struct EvList { std::vector<AnyEvent> ev; };
static uint32_t inSize(const clap_input_events_t* l) { return (uint32_t)((EvList*)l->ctx)->ev.size(); }
static const clap_event_header_t* inGet(const clap_input_events_t* l, uint32_t i) { return &((EvList*)l->ctx)->ev[i].header; }
static bool outPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }

struct Stream { std::string data; size_t pos = 0; };
static int64_t sWrite(const clap_ostream_t* s, const void* b, uint64_t n) { ((Stream*)s->ctx)->data.append((const char*)b, n); return (int64_t)n; }
static int64_t sRead(const clap_istream_t* s, void* b, uint64_t n) { Stream* st = (Stream*)s->ctx; uint64_t k = std::min<uint64_t>(n, st->data.size() - st->pos); memcpy(b, st->data.data() + st->pos, k); st->pos += k; return (int64_t)k; }

struct Instance {
    const clap_plugin_t* p = nullptr;
    float L[512], R[512]; float* outs[2] = { L, R };
    clap_audio_buffer_t ob{};
    EvList evs; clap_input_events_t in{ &evs, inSize, inGet }; clap_output_events_t out{ nullptr, outPush };
    Instance(const clap_plugin_factory_t* fac, const char* id) {
        p = fac->create_plugin(fac, &host, id); p->init(p); p->activate(p, 48000, 1, 512); p->start_processing(p);
        ob.data32 = outs; ob.channel_count = 2;
        // The measured room is prepared on a worker thread; levels are only comparable once it is in.
        usleep(700000);
    }
    ~Instance() { p->stop_processing(p); p->deactivate(p); p->destroy(p); }
    void note(int type, int id, int ch, int key, double vel = 0.6, uint32_t t = 0) {
        AnyEvent a{}; a.note.header.size = sizeof(clap_event_note_t); a.note.header.type = (uint16_t)type; a.note.header.time = t;
        a.note.note_id = id; a.note.port_index = 0; a.note.channel = (int16_t)ch; a.note.key = (int16_t)key; a.note.velocity = vel; evs.ev.push_back(a);
    }
    void expr(int id, int ch, int key, double v, int which = CLAP_NOTE_EXPRESSION_PRESSURE, uint32_t t = 0) {
        AnyEvent x{}; x.expr.header.size = sizeof(clap_event_note_expression_t); x.expr.header.type = CLAP_EVENT_NOTE_EXPRESSION; x.expr.header.time = t;
        x.expr.expression_id = which; x.expr.note_id = id; x.expr.port_index = 0; x.expr.channel = (int16_t)ch; x.expr.key = (int16_t)key; x.expr.value = v; evs.ev.push_back(x);
    }
    // parameter by its display name, set through an event like a host automation lane would
    double get(const char* name) const {
        auto* pp = (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
        for (uint32_t i = 0; pp && i < pp->count(p); ++i) {
            clap_param_info_t info{};
            if (!pp->get_info(p, i, &info) || strcmp(info.name, name) != 0) continue;
            double v = 0; pp->get_value(p, info.id, &v); return v;
        }
        return -1e9;
    }
    void param(const char* name, double value) {
        auto* pp = (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
        for (uint32_t i = 0; pp && i < pp->count(p); ++i) {
            clap_param_info_t info{};
            if (!pp->get_info(p, i, &info) || strcmp(info.name, name) != 0) continue;
            AnyEvent e{}; e.param.header.size = sizeof(clap_event_param_value_t); e.param.header.type = CLAP_EVENT_PARAM_VALUE;
            e.param.param_id = info.id; e.param.cookie = info.cookie; e.param.note_id = -1; e.param.port_index = -1; e.param.channel = -1; e.param.key = -1; e.param.value = value;
            evs.ev.push_back(e); return;
        }
        printf("  FAIL parameter '%s' not found\n", name); exit(1);
    }
    double value(const char* name) {
        auto* pp = (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
        for (uint32_t i = 0; pp && i < pp->count(p); ++i) {
            clap_param_info_t info{}; double v = 0;
            if (pp->get_info(p, i, &info) && strcmp(info.name, name) == 0 && pp->get_value(p, info.id, &v)) return v;
        }
        printf("  FAIL parameter '%s' not found\n", name); exit(1);
    }
    std::vector<float> cap;     // everything played so far (left channel)
    // strongest partial between lo and hi Hz in the last `seconds` of audio
    double peakHz(double seconds, double lo, double hi) const {
        const size_t n = (size_t)(seconds * 48000), o = cap.size() - n; double best = -1, bf = 0;
        for (double f = lo; f <= hi; f += 0.05) {
            double re = 0, im = 0;
            for (size_t i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / n), a = 2 * M_PI * f * i / 48000; re += w * cap[o + i] * std::cos(a); im += w * cap[o + i] * std::sin(a); }
            if (re * re + im * im > best) { best = re * re + im * im; bf = f; }
        }
        return bf;
    }
    void midi(uint8_t a, uint8_t b, uint8_t c, uint32_t t = 0) {
        AnyEvent m{}; m.midi.header.size = sizeof(clap_event_midi_t); m.midi.header.type = CLAP_EVENT_MIDI; m.midi.header.time = t;
        m.midi.port_index = 0; m.midi.data[0] = a; m.midi.data[1] = b; m.midi.data[2] = c; evs.ev.push_back(m);
    }
    // process for `seconds`, events (if any) at the start; returns level in dB over the second half
    double play(double seconds) {
        const int nb = (int)(seconds * 48000 / 512); double e = 0; int n = 0;
        for (int b = 0; b < nb; ++b) {
            clap_process_t pr{}; pr.steady_time = -1; pr.frames_count = 512; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
            p->process(p, &pr); evs.ev.clear(); cap.insert(cap.end(), L, L + 512);
            if (b >= nb / 2) { for (int i = 0; i < 512; ++i) e += L[i] * L[i]; n += 512; }
        }
        return 10 * std::log10(e / std::max(1, n) + 1e-20);
    }
};


// ---------------------------------------------------------------------------------------------------------
// The organ as one system, tested through the plugin as a host loads and plays it. (This file shares the
// host above with clap_mpe_test.cpp.) Each part of the wind and air added to the model gets its check here.
static void onlyStop(Instance& s, const char* div, const char* stop) {
    auto* pp = (const clap_plugin_params_t*)s.p->get_extension(s.p, CLAP_EXT_PARAMS);
    for (uint32_t i = 0; i < pp->count(s.p); ++i) {
        clap_param_info_t info{}; if (!pp->get_info(s.p, i, &info)) continue;
        const std::string n = info.name;
        const bool isStop = n.rfind("PEDAL ", 0) == 0 || n.rfind("I ", 0) == 0 || n.rfind("II ", 0) == 0 || n.rfind("III ", 0) == 0;
        if (!isStop) continue;
        const bool want = n.rfind(std::string(div) + " ", 0) == 0 && n.size() >= strlen(stop) && n.compare(n.size() - strlen(stop), strlen(stop), stop) == 0;
        s.param(info.name, want ? 1.0 : 0.0);
    }
}
// the speed of sound and density of the air, as the model's Air.hpp computes them (moist ideal gas)
static void airOf(double tC, double rh, double kPa, double& c, double& rho) {
    const double R = 8.314462618, Md = 0.0289647, Mw = 0.01801528, T = 273.15 + tC, p = kPa * 1000;
    const double ps = 611.21 * std::exp((18.678 - tC / 234.5) * (tC / (257.14 + tC))), xw = rh / 100 * ps / p;
    const double M = (1 - xw) * Md + xw * Mw, cv = (1 - xw) * 2.5 + xw * 3.04, g = (cv + 1) / cv;
    c = std::sqrt(g * R * T / M); rho = p * M / (R * T);
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build/bin/OpusXIV.clap";
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto entry = (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
    entry->init(path);
    auto fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const char* id = fac->get_plugin_descriptor(fac, 0)->id;
    int failures = 0;
    auto check = [&](bool ok, const char* what, double a, double b = 0) { printf("  %-4s %s (%.2f, %.2f)\n", ok ? "ok" : "FAIL", what, a, b); if (!ok) ++failures; };
    printf("The organ as one system (through the plugin):\n");

    // ---- the air: temperature, humidity, pressure act on the pipes themselves
    {
        // c1 held in the tuning air (the tuner learns it), then the room warms by 10 C with it held
        auto pitchIn = [&](const char* stop, double tC, double rh, double kPa, double& lvl) {
            Instance s(fac, id); s.param("Room Mix", 0.0); s.param("Age Drift", 0); s.param("Tuning System", 0);
            onlyStop(s, "I", stop); s.play(0.1);
            s.note(CLAP_EVENT_NOTE_ON, 1, 0, 60, 0.8); s.play(3.0);
            s.param("Air Temperature", tC); s.param("Air Humidity", rh); s.param("Air Pressure", kPa);
            lvl = s.play(3.0);
            return s.peakHz(1.5, 240, 285);
        };
        double l0, l1, l2, l3;
        const double fF0 = pitchIn("OCTAVE 8'", 18, 50, 101.325, l0), fF1 = pitchIn("OCTAVE 8'", 28, 50, 101.325, l1);
        double c0, r0, c1, r1; airOf(18, 50, 101.325, c0, r0); airOf(28, 50, 101.325, c1, r1);
        const double flueC = 1200 * std::log2(fF1 / fF0), expectC = 1200 * std::log2(c1 / c0);
        check(std::fabs(flueC - expectC) < 2.0, "flue (I Octave 8' c1) 10 C warmer: sharpens with the speed of sound (cents, expected)", flueC, expectC);
        const double fR0 = pitchIn("TRUMPET 8'", 18, 50, 101.325, l2), fR1 = pitchIn("TRUMPET 8'", 28, 50, 101.325, l3);
        const double reedC = 1200 * std::log2(fR1 / fR0);
        // The tongue hardly follows the air (warm brass is a little softer); the resonator does and pulls the
        // tongue with it: typically about 70 % of the flues' rise, anywhere from 0 to 100 % with the design
        // (Mechanical Music Digest, "Sound speed and pipe tuning"). So it rises, and less than the flues.
        check(reedC > -3.0 && reedC < flueC - 1.0, "reed (I Trumpet 8' c1) 10 C warmer: its resonator pulls it up, less than the flues (cents, flue cents)", reedC, flueC);
        // thinner air (a town at ~1800 m): the speed of sound is the same, the jet faster
        double lA, lB;
        const double fP0 = pitchIn("OCTAVE 8'", 18, 50, 101.325, lA), fP1 = pitchIn("OCTAVE 8'", 18, 50, 81.5, lB);
        check(std::fabs(1200 * std::log2(fP1 / fP0)) < 4.0 && std::isfinite(lB) && lB > lA - 6, "flue in thin air (81.5 kPa): still speaks, near pitch (cents, dB change)", 1200 * std::log2(fP1 / fP0), lB - lA);
    }

    // ---- the slider chest: the drawn stops on a key share its groove's wind
    {
        auto grooveMm = [&](bool full, double& demand) {
            Instance s(fac, id); s.param("Room Mix", 0.0); s.param("Age Drift", 0);
            onlyStop(s, "I", "OCTAVE 8'");
            if (full) {   // every stop of the Hauptwerk
                auto* pp = (const clap_plugin_params_t*)s.p->get_extension(s.p, CLAP_EXT_PARAMS);
                for (uint32_t i = 0; i < pp->count(s.p); ++i) { clap_param_info_t info{}; if (pp->get_info(s.p, i, &info)) { const std::string nm = info.name; if (nm.rfind("I ", 0) == 0 && nm.find('/') == std::string::npos && nm.find("TREMULANT") == std::string::npos) s.param(info.name, 1.0); } }
            }
            s.play(0.1);
            s.note(CLAP_EVENT_NOTE_ON, 1, 0, 48, 0.8); s.play(1.5);
            demand = s.get("Wind Demand");
            return s.get("Note 1 mm");
        };
        double d1, dF;
        const double g1 = grooveMm(false, d1), gF = grooveMm(true, dF);
        check(g1 > 70 && g1 < 90, "c (key 48), one stop: its groove holds the chest's wind (mm)", g1);
        check(gF < g1 - 0.5 && gF > 0.5 * g1, "c, the whole Hauptwerk on it: the stops share the groove, its wind sinks (mm, one stop)", gF, g1);
        check(dF > 3 * d1, "the blower feels what the pallets let in (demand, one stop / all)", dF, d1);
    }

    // ---- the division's wind at audio rate: regulator, trunk and chest, and the tremulant's valve in them
    {
        auto swell = [&](int trem, bool full, double& mean, double& pp, double& hz, double& afterMax, double& afterMean) {
            Instance s(fac, id); s.param("Room Mix", 0.0); s.param("Age Drift", 0);
            auto* pp_ = (const clap_plugin_params_t*)s.p->get_extension(s.p, CLAP_EXT_PARAMS);
            for (uint32_t i = 0; i < pp_->count(s.p); ++i) { clap_param_info_t info{}; if (!pp_->get_info(s.p, i, &info)) continue;
                const std::string nm = info.name; const bool stop = nm.rfind("PEDAL ", 0) == 0 || nm.rfind("I ", 0) == 0 || nm.rfind("II ", 0) == 0 || nm.rfind("III ", 0) == 0;
                if (nm.find("TREMULANT") != std::string::npos) s.param(info.name, trem >= 0 ? 1.0 : 0.0);
                else if (stop) s.param(info.name, nm.rfind("III ", 0) == 0 && nm.find('/') == std::string::npos && (full || nm.find("GAMBA") != std::string::npos) ? 1.0 : 0.0); }
            s.param("Lower Zone Plays", 2); s.param("Upper Zone Plays", 2);
            if (trem >= 0) { s.param("Tremulant Division", 2); s.param("Tremulant Kind", trem); }
            s.play(0.1);
            for (int k : { 60, 64, 67, 72 }) s.note(CLAP_EVENT_NOTE_ON, k, 0, k, 0.8);
            s.play(1.5);
            std::vector<double> pc; for (int b = 0; b < 190; ++b) { s.play(512.0 / 48000); pc.push_back(s.get("III Unterwerk Chest")); }
            double mn = 1e9, mx = -1e9; mean = 0; for (double v : pc) { mn = std::min(mn, v); mx = std::max(mx, v); mean += v; } mean /= pc.size();
            int cr = 0; for (size_t i = 1; i < pc.size(); ++i) if (pc[i - 1] < mean && pc[i] >= mean) ++cr;
            pp = mx - mn; hz = cr / (pc.size() * 512.0 / 48000);
            for (int k : { 60, 64, 67, 72 }) s.note(CLAP_EVENT_NOTE_OFF, k, 0, k, 0.0);
            afterMax = -1e9; afterMean = 0; int na = 0;
            for (int b = 0; b < 94; ++b) { s.play(512.0 / 48000); const double v = s.get("III Unterwerk Chest"); afterMax = std::max(afterMax, v); if (b >= 47) { afterMean += v; ++na; } }
            afterMean /= na;
        };
        const double s_reservoirMm = 96.0;   // the main reservoir (Reservoir Pressure default)
        double m0, p0, h0, am0, aM0, m1, p1, h1, am1, aM1, m2, p2, h2, am2, aM2, x, g1, g2, hg;
        swell(-1, true, m0, p0, h0, am0, aM0); swell(0, true, m1, p1, h1, am1, aM1); swell(1, true, m2, p2, h2, am2, aM2);
        swell(0, false, x, g1, hg, x, x); swell(1, false, x, g2, hg, x, x);
        check(p1 < 0.6 * g1, "doux: it weakens as the stops are drawn (mm p-p full Swell, Gamba alone)", p1, g1);
        check(p2 / g2 > p1 / g1, "vent perdu: it loses less of its depth than the doux (mm p-p full Swell, Gamba alone)", p2, g2);
        check(std::fabs(m0 - 79) < 2 && p0 < 2, "full Swell, 4-note chord: its regulator holds the chest steady (mm mean, mm p-p)", m0, p0);
        check(am0 < s_reservoirMm && std::fabs(aM0 - 79) < 1.5, "the chord released: the trunk's wind surges into the chest (below the reservoir's pressure) and settles back within 0.5 s (mm peak, mm after)", am0, aM0);
        check(p1 > 0.03 * m1 && p1 < 0.4 * m1 && std::fabs(h1 - 5.8) < 1.0, "tremulant (doux) on the full Swell: a gentle beat at its rate (mm p-p, Hz)", p1, h1);
        check(p2 > 0.12 * m2 && p2 < 0.75 * m2 && p2 > 1.5 * p1 && std::fabs(h2 - 5.8) < 1.0, "tremulant (vent perdu) on the full Swell: deeper, it keeps working (mm p-p, Hz)", p2, h2);
    }

    printf("%s\n", failures ? "SYSTEM FAILURES" : "SYSTEM CHECKS PASSED");
    return failures ? 1 : 0;
}
