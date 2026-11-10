// Loads the built Pipe.clap and checks, through the real binary, how CLAP note events and note
// expressions reach the notes (CLAP events.h: voices by note_id or port/channel/key, -1 = wildcard).
#include "entry.h"
#include "plugin-factory.h"
#include "plugin.h"
#include "ext/params.h"
#include "ext/state.h"
#include "ext/note-ports.h"
#include "events.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

static const void* hostExt(const clap_host_t*, const char*) { return nullptr; }
static void hostNoop(const clap_host_t*) {}
static clap_host_t host = { CLAP_VERSION, nullptr, "test", "test", "", "1", hostExt, hostNoop, hostNoop, hostNoop };

union AnyEvent { clap_event_header_t header; clap_event_midi_t midi; clap_event_note_t note; clap_event_note_expression_t expr; };
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
    void midi(uint8_t a, uint8_t b, uint8_t c, uint32_t t = 0) {
        AnyEvent m{}; m.midi.header.size = sizeof(clap_event_midi_t); m.midi.header.type = CLAP_EVENT_MIDI; m.midi.header.time = t;
        m.midi.port_index = 0; m.midi.data[0] = a; m.midi.data[1] = b; m.midi.data[2] = c; evs.ev.push_back(m);
    }
    // process for `seconds`, events (if any) at the start; returns level in dB over the second half
    double play(double seconds) {
        const int nb = (int)(seconds * 48000 / 512); double e = 0; int n = 0;
        for (int b = 0; b < nb; ++b) {
            clap_process_t pr{}; pr.steady_time = -1; pr.frames_count = 512; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
            p->process(p, &pr); evs.ev.clear();
            if (b >= nb / 2) { for (int i = 0; i < 512; ++i) e += L[i] * L[i]; n += 512; }
        }
        return 10 * std::log10(e / std::max(1, n) + 1e-20);
    }
};

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build/bin/Pipe.clap";
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto entry = (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
    entry->init(path);
    auto fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const char* id = fac->get_plugin_descriptor(fac, 0)->id;
    int failures = 0;
    auto check = [&](bool ok, const char* what, double a, double b = 0) { printf("  %-4s %s (%.1f, %.1f)\n", ok ? "ok" : "FAIL", what, a, b); if (!ok) ++failures; };
    printf("CLAP note events and expressions:\n");

    {   // the first note after loading sounds straight away
        Instance s(fac, id); s.note(CLAP_EVENT_NOTE_ON, 1, 0, 60); s.expr(1, 0, 60, 1.0);
        const double l = s.play(0.4);
        check(l > -60, "first note right after activation sounds (dB)", l);
    }
    {   // notes sharing a channel must behave exactly like notes on their own channels
        double after[2], held[2];
        for (int layout = 0; layout < 2; ++layout) {
            Instance s(fac, id);
            const int chA = layout == 0 ? 0 : 1, chB = layout == 0 ? 0 : 2;
            s.play(0.2);
            s.note(CLAP_EVENT_NOTE_ON, 1, chA, 60); s.expr(1, chA, 60, 1.0); s.play(0.5);
            s.note(CLAP_EVENT_NOTE_ON, 2, chB, 64); s.expr(2, chB, 64, 0.6);
            held[layout] = s.play(1.0);
            s.expr(1, chA, 60, 0.0); s.note(CLAP_EVENT_NOTE_OFF, 1, chA, 60, 0.5);
            after[layout] = s.play(1.5);
        }
        check(std::fabs(held[0] - held[1]) < 0.5 && std::fabs(after[0] - after[1]) < 0.5,
              "shared channel = own channels, while both held and after releasing one (dB difference, dB difference)", held[0] - held[1], after[0] - after[1]);
    }
    {   // note_id targets one voice; -1 wildcards target all
        Instance s(fac, id); s.play(0.2);
        s.note(CLAP_EVENT_NOTE_ON, 7, 0, 60); s.note(CLAP_EVENT_NOTE_ON, 8, 0, 67);
        s.expr(7, -1, -1, 1.0); s.expr(8, -1, -1, 1.0); s.play(0.5);
        s.expr(8, -1, -1, 0.0);
        const double one = s.play(2.0);
        Instance r(fac, id); r.play(0.2);
        r.note(CLAP_EVENT_NOTE_ON, 7, 0, 60); r.note(CLAP_EVENT_NOTE_ON, 8, 0, 67);
        r.expr(7, -1, -1, 1.0); r.expr(8, -1, -1, 1.0); r.play(0.5);
        r.note(CLAP_EVENT_NOTE_OFF, 8, 0, 67);
        const double ref = r.play(2.0);
        s.expr(-1, -1, -1, 0.0);
        s.play(0.5);
        const double none = s.play(1.0);
        check(std::fabs(one - ref) < 1.0 && none < one - 25, "pressure 0 by note_id closes only that voice (= releasing it); wildcard closes all (dB diff, dB drop)", one - ref, one - none);
    }
    {   // choke with wildcards releases every voice
        Instance s(fac, id); s.play(0.2);
        s.note(CLAP_EVENT_NOTE_ON, 1, 0, 60); s.note(CLAP_EVENT_NOTE_ON, 2, 3, 64);
        const double on = s.play(0.6);
        s.note(CLAP_EVENT_NOTE_CHOKE, -1, -1, -1);
        s.play(0.5);
        const double off = s.play(0.6);
        check(off < on - 30, "NOTE_CHOKE with key/channel -1 stops all voices (dB, dB)", on, off);
    }
    {   // velocity 0 is a valid NOTE_ON in CLAP
        Instance s(fac, id); s.play(0.2);
        s.note(CLAP_EVENT_NOTE_ON, 1, 0, 60, 0.0);
        const double l = s.play(0.6);
        check(l > -60, "NOTE_ON with velocity 0 sounds (dB)", l);
    }
    {   // a dense block: thousands of controller messages before a note-on in the same block
        Instance s(fac, id); s.play(0.2);
        for (int i = 0; i < 3000; ++i) s.midi((uint8_t)(0xD1 + i % 15), (uint8_t)(i % 128), 0, (uint32_t)(i * 500 / 3000));
        s.note(CLAP_EVENT_NOTE_ON, 1, 0, 60, 0.6, 505);
        const double l = s.play(0.6);
        check(l > -60, "note-on after 3000 events in one block still sounds (dB)", l);
    }
    printf("%s\n", failures ? "CLAP MPE FAILURES" : "CLAP MPE CHECKS PASSED");
    return failures ? 1 : 0;
}
