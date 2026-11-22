// Pipe - physically modelled pipe organ
// A small pool of worker threads for the audio thread: run(count, fn) calls fn(i) for every i in
// [0, count) across the workers and the calling thread, and returns when all are done. The tasks are
// taken one at a time from a shared counter, so a long one (a groove with a full chorus on it) does
// not hold the others back. Between blocks the workers spin briefly (the next block is usually a
// fraction of a millisecond away) and then sleep, so an idle organ costs nothing.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#define PIPE_POOL_SSE 1
#endif

namespace pipe {

class WorkerPool {
public:
    WorkerPool() = default;
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool() { stop(); }

    // workers: threads besides the caller (0 runs everything on the caller)
    void start(int workers) {
        stop();
        quit.store(false);
        for (int i = 0; i < workers; ++i) threads.emplace_back([this] { loop(); });
    }
    void stop() {
        if (threads.empty()) return;
        { std::lock_guard<std::mutex> lk(m); quit.store(true); }
        cv.notify_all();
        for (auto& t : threads) t.join();
        threads.clear();
    }
    int workers() const { return (int)threads.size(); }

    template <class F> void run(int count, F& fn) {
        if (count <= 0) return;
        if (threads.empty() || count == 1) { for (int i = 0; i < count; ++i) fn(i); return; }
        const uint64_t job = (uint64_t)++jobs;
        ctx = &fn; call = [](void* c, int i) { (*static_cast<F*>(c))(i); };
        total.store(count, std::memory_order_relaxed);
        done.store(0, std::memory_order_relaxed);
        next.store(job << 32, std::memory_order_release);   // the job's tickets: its number, then the task index
        { std::lock_guard<std::mutex> lk(m); gen.store(job, std::memory_order_release); }
        if (sleepers.load(std::memory_order_acquire) > 0) cv.notify_all();
        work(job);
        while (done.load(std::memory_order_acquire) < count) pause();
    }

private:
    static void pause() {
#ifdef PIPE_POOL_SSE
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }
    // take tickets of this job until none are left; a worker late for a job finds its number gone
    // from the counter and takes nothing from the next one
    void work(uint64_t job) {
        for (;;) {
            uint64_t v = next.load(std::memory_order_acquire);
            if ((v >> 32) != job) return;
            const int i = (int)(v & 0xffffffffu);
            if (i >= total.load(std::memory_order_relaxed)) return;
            if (!next.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            call(ctx, i);
            done.fetch_add(1, std::memory_order_release);
        }
    }
    void loop() {
#ifdef PIPE_POOL_SSE
        _mm_setcsr(_mm_getcsr() | 0x8040);   // flush denormals, as the audio thread does
#endif
        uint64_t seen = gen.load(std::memory_order_acquire);
        for (;;) {
            // spin a little (about 0.2 ms), then sleep until the next block
            int spins = 0;
            while (gen.load(std::memory_order_acquire) == seen && !quit.load(std::memory_order_relaxed) && spins < 4000) { pause(); ++spins; }
            if (gen.load(std::memory_order_acquire) == seen && !quit.load()) {
                std::unique_lock<std::mutex> lk(m);
                sleepers.fetch_add(1, std::memory_order_acq_rel);
                cv.wait(lk, [&] { return gen.load(std::memory_order_acquire) != seen || quit.load(); });
                sleepers.fetch_sub(1, std::memory_order_acq_rel);
            }
            if (quit.load()) return;
            seen = gen.load(std::memory_order_acquire);
            work(seen);
        }
    }

    std::vector<std::thread> threads;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<uint64_t> gen{0};
    std::atomic<uint64_t> next{0};
    std::atomic<int> total{0}, done{0}, sleepers{0};
    std::atomic<bool> quit{false};
    uint32_t jobs = 0;
    void* ctx = nullptr;
    void (*call)(void*, int) = nullptr;
};

} // namespace pipe
