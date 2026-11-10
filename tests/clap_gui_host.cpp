// Opens Pipe's GUI inside an X11 window and keeps it alive while screenshots are taken.
#include "entry.h"
#include "plugin-factory.h"
#include "plugin.h"
#include "ext/gui.h"
#include "ext/timer-support.h"
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#include <poll.h>
struct Timer { clap_id id; uint32_t ms; std::chrono::steady_clock::time_point next; };
static std::vector<Timer> timers; static clap_id nextId = 1;
static bool regTimer(const clap_host_t*, uint32_t ms, clap_id* id) { *id = nextId++; timers.push_back({*id, ms, std::chrono::steady_clock::now()}); return true; }
static bool unregTimer(const clap_host_t*, clap_id id) { for (size_t i=0;i<timers.size();++i) if (timers[i].id==id) { timers.erase(timers.begin()+i); return true; } return false; }
static clap_host_timer_support_t timerExt = { regTimer, unregTimer };
static void hgNoop(const clap_host_t*) {}
static bool hgResize(const clap_host_t*, uint32_t, uint32_t) { return true; }
static bool hgShow(const clap_host_t*) { return true; }
static void hgClosed(const clap_host_t*, bool) {}
static clap_host_gui_t hostGui = { hgNoop, hgResize, hgShow, hgShow, hgClosed };
static const void* hostExt(const clap_host_t*, const char* id) {
  if (!strcmp(id, CLAP_EXT_GUI)) return &hostGui;
  if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timerExt;
  return nullptr; }
static void noop(const clap_host_t*) {}
static clap_host_t host = { CLAP_VERSION, nullptr, "guitest", "t", "", "1", hostExt, noop, noop, noop };
int main(int argc, char** argv) {
  const char* path = argv[1]; int seconds = argc > 2 ? atoi(argv[2]) : 60;
  void* lib = dlopen(path, RTLD_NOW); if (!lib) { printf("%s\n", dlerror()); return 1; }
  auto entry = (const clap_plugin_entry_t*)dlsym(lib, "clap_entry"); entry->init(path);
  auto fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  auto p = fac->create_plugin(fac, &host, fac->get_plugin_descriptor(fac, 0)->id); p->init(p);
  auto timerP = (const clap_plugin_timer_support_t*)p->get_extension(p, CLAP_EXT_TIMER_SUPPORT);
  auto gui = (const clap_plugin_gui_t*)p->get_extension(p, CLAP_EXT_GUI);
  Display* dpy = XOpenDisplay(nullptr);
  Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1280, 800, 0, 0, 0);
  XStoreName(dpy, w, "pipehost"); XMapWindow(dpy, w); XFlush(dpy);
  if (!gui->create(p, CLAP_WINDOW_API_X11, false)) { printf("gui create failed\n"); return 1; }
  uint32_t gw, gh; gui->get_size(p, &gw, &gh); printf("gui size %ux%u\n", gw, gh); fflush(stdout);
  XResizeWindow(dpy, w, gw, gh);
  clap_window_t win; win.api = CLAP_WINDOW_API_X11; win.x11 = w;
  gui->set_parent(p, &win); gui->show(p);
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end) {
    auto now = std::chrono::steady_clock::now();
    for (auto& t : timers) if (now >= t.next) { t.next = now + std::chrono::milliseconds(t.ms); if (timerP) timerP->on_timer(p, t.id); }
    while (XPending(dpy)) { XEvent e; XNextEvent(dpy, &e); }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  gui->destroy(p); p->destroy(p); entry->deinit();
}
