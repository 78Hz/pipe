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

struct EvList { std::vector<clap_event_midi_t> ev; };
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
            e.header.time = 0; e.port_index = 0; e.data[0] = 0x90; e.data[1] = (uint8_t)k; e.data[2] = 100; evs.ev.push_back(e);
        }
        clap_process_t pr{}; pr.steady_time = -1; pr.frames_count = 512; pr.audio_outputs = &ob; pr.audio_outputs_count = 1; pr.in_events = &in; pr.out_events = &out;
        p->process(p, &pr);
        if (b > blocks / 2) for (int i = 0; i < 512; ++i) { sumSq += L[i] * L[i]; if (!std::isfinite(L[i])) finite = false; }
    }
    const double rms = std::sqrt(sumSq / ((blocks / 2) * 512.0));
    printf("chord rms %.4f (%.1f dBFS), finite %d\n", rms, 20 * std::log10(rms + 1e-12), finite);
    auto state = (const clap_plugin_state_t*)p->get_extension(p, CLAP_EXT_STATE);
    Stream s; clap_ostream_t os{ &s, sWrite };
    bool saved = state->save(p, &os);
    clap_istream_t is{ &s, sRead };
    bool loaded = state->load(p, &is);
    printf("state save %d (%zu bytes), load %d\n", saved, s.data.size(), loaded);
    p->stop_processing(p); p->deactivate(p); p->destroy(p); entry->deinit();
    bool ok = finite && rms > 1e-3 && saved && loaded && params->count != nullptr;
    printf("%s\n", ok ? "CLAP CHECK PASSED" : "CLAP CHECK FAILED");
    return ok ? 0 : 1;
}
