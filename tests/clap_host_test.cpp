// Loads the built Pipe.clap, plays a chord through the real plugin binary and
// checks audio, parameters and state save/restore.
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

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build/bin/Pipe.clap";
    void* lib = dlopen(path, RTLD_NOW);
    if (!lib) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    auto entry = (const clap_plugin_entry_t*)dlsym(lib, "clap_entry");
    entry->init(path);
    auto fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* d = fac->get_plugin_descriptor(fac, 0);
    printf("plugin: %s by %s (%s)\n", d->name, d->vendor, d->id);
    const clap_plugin_t* p = fac->create_plugin(fac, &host, d->id);
    p->init(p);
    auto params = (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
    printf("parameters: %u\n", params->count(p));
    p->activate(p, 48000, 1, 512);
    p->start_processing(p);
    float L[512], R[512]; float* outs[2] = { L, R };
    clap_audio_buffer_t ob{}; ob.data32 = outs; ob.channel_count = 2;
    EvList evs; clap_input_events_t in{ &evs, inSize, inGet }; clap_output_events_t out{ nullptr, outPush };
    double sumSq = 0; bool finite = true; int blocks = 48000 * 3 / 512;
    for (int b = 0; b < blocks; ++b) {
        evs.ev.clear();
        if (b == 0) for (int k : { 48, 60, 64, 67 }) {
            clap_event_midi_t e{}; e.header.size = sizeof(e); e.header.type = CLAP_EVENT_MIDI; e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.time = 0; e.port_index = 0; e.data[0] = 0x90; e.data[1] = (uint8_t)k; e.data[2] = 100; AnyEvent a{}; a.midi = e; evs.ev.push_back(a);
        }
        clap_process_t pr{}; pr.steady_time = -1; pr.frames_count = 512; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
        p->process(p, &pr);
        if (b > blocks / 2) for (int i = 0; i < 512; ++i) { sumSq += L[i] * L[i]; if (!std::isfinite(L[i])) finite = false; }
    }
    const double rms = std::sqrt(sumSq / ((blocks / 2) * 512.0));
    // MPE: the note port must offer the MPE dialect, and Bitwig-style CLAP note expressions must reach the engine
    auto np = (const clap_plugin_note_ports_t*)p->get_extension(p, CLAP_EXT_NOTE_PORTS);
    clap_note_port_info_t npi{};
    const bool mpeDialect = np && np->get(p, 0, true, &npi) && (npi.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE);
    printf("note port dialects 0x%x, MPE %s\n", npi.supported_dialects, mpeDialect ? "yes" : "NO");
    for (int b = 0; b < 48000 * 6 / 512; ++b) {   // silence the chord and let the room tail die away
        evs.ev.clear();
        if (b == 0) for (int k : { 48, 60, 64, 67 }) {
            clap_event_midi_t e{}; e.header.size = sizeof(e); e.header.type = CLAP_EVENT_MIDI; e.data[0] = 0x80; e.data[1] = (uint8_t)k; e.data[2] = 64;
            AnyEvent a{}; a.midi = e; evs.ev.push_back(a);
        }
        clap_process_t pr{}; pr.steady_time = -1; pr.frames_count = 512; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
        p->process(p, &pr);
    }
    auto expressionLevel = [&](double pressure, bool start) {
        double e = 0; const int nb = 48000 / 512;
        for (int b = 0; b < nb; ++b) {
            evs.ev.clear();
            if (b == 0 && start) {
                AnyEvent a{}; a.note.header.size = sizeof(clap_event_note_t); a.note.header.type = CLAP_EVENT_NOTE_ON;
                a.note.note_id = 1; a.note.port_index = 0; a.note.channel = 2; a.note.key = 60; a.note.velocity = 0.7; evs.ev.push_back(a);
            }
            if (b == 0) {
                AnyEvent x{}; x.expr.header.size = sizeof(clap_event_note_expression_t); x.expr.header.type = CLAP_EVENT_NOTE_EXPRESSION;
                x.expr.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE; x.expr.note_id = 1; x.expr.port_index = 0; x.expr.channel = 2; x.expr.key = 60;
                x.expr.value = pressure; evs.ev.push_back(x);
            }
            clap_process_t pr{}; pr.steady_time = -1; pr.frames_count = 512; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
            p->process(p, &pr);
            if (b > nb / 2) for (int i = 0; i < 512; ++i) e += L[i] * L[i];
        }
        return 10 * std::log10(e + 1e-20);
    };
    const double soft = expressionLevel(0.05, true), full = expressionLevel(1.0, false);
    printf("CLAP note expression pressure: %.1f dB at 5%%, %.1f dB at full (%+.1f dB)\n", soft, full, full - soft);
    const bool exprOk = full - soft > 10.0;
    printf("chord rms %.4f (%.1f dBFS), finite %d\n", rms, 20 * std::log10(rms + 1e-12), finite);
    auto state = (const clap_plugin_state_t*)p->get_extension(p, CLAP_EXT_STATE);
    Stream s; clap_ostream_t os{ &s, sWrite };
    bool saved = state->save(p, &os);
    clap_istream_t is{ &s, sRead };
    bool loaded = state->load(p, &is);
    printf("state save %d (%zu bytes), load %d\n", saved, s.data.size(), loaded);
    p->stop_processing(p); p->deactivate(p); p->destroy(p); entry->deinit();
    bool ok = finite && rms > 1e-3 && saved && loaded && params->count != nullptr && mpeDialect && exprOk;
    printf("%s\n", ok ? "CLAP CHECK PASSED" : "CLAP CHECK FAILED");
    return ok ? 0 : 1;
}
