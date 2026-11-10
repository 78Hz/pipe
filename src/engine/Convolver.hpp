// Pipe - physically modelled pipe organ
// Zero-latency convolution: mono in, stereo out. The first block of the
// impulse response runs as a direct FIR; the rest is uniformly partitioned
// FFT convolution (overlap-save) whose one-block delay lines up exactly with
// where the FIR leaves off, so nothing is added to the plugin's latency.
#pragma once

#include "pffft.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace pipe {

template <typename T>
struct AlignedBuf {
    T* p = nullptr; size_t n = 0;
    AlignedBuf() = default;
    explicit AlignedBuf(size_t count) { alloc(count); }
    ~AlignedBuf() { if (p) pffft_aligned_free(p); }
    AlignedBuf(const AlignedBuf&) = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
    void alloc(size_t count) {
        if (p) pffft_aligned_free(p);
        n = count; p = (T*)pffft_aligned_malloc(sizeof(T) * std::max<size_t>(1, count));
        std::memset(p, 0, sizeof(T) * std::max<size_t>(1, count));
    }
};

class StereoConvolver {
public:
    // irL/irR: impulse responses at the processing rate. block: partition size (power of two >= 64).
    StereoConvolver(const std::vector<float>& irL, const std::vector<float>& irR, int block)
        : B(block), N(2 * block)
    {
        const size_t len = std::max(irL.size(), irR.size());
        setup = pffft_new_setup(N, PFFFT_REAL);
        // head: taps [0, B)
        headL.assign(B, 0.f); headR.assign(B, 0.f);
        for (int i = 0; i < B; ++i) { headL[i] = i < (int)irL.size() ? irL[i] : 0.f; headR[i] = i < (int)irR.size() ? irR[i] : 0.f; }
        // tail partitions: taps [B + pB, B + (p+1)B)
        P = len > (size_t)B ? (int)((len - (size_t)B + (size_t)B - 1) / (size_t)B) : 0;
        if (P > 0) {
            kernL.alloc((size_t)P * N); kernR.alloc((size_t)P * N);
            fdl.alloc((size_t)P * N);
            AlignedBuf<float> tmp(N), work(N);
            for (int p = 0; p < P; ++p) {
                for (int ch = 0; ch < 2; ++ch) {
                    const std::vector<float>& h = ch == 0 ? irL : irR;
                    std::memset(tmp.p, 0, sizeof(float) * N);
                    for (int i = 0; i < B; ++i) {
                        const size_t idx = (size_t)B + (size_t)p * B + (size_t)i;
                        tmp.p[i] = idx < h.size() ? h[idx] : 0.f;
                    }
                    pffft_transform(setup, tmp.p, (ch == 0 ? kernL.p : kernR.p) + (size_t)p * N, work.p, PFFFT_FORWARD);
                }
            }
        }
        inBlock.alloc(N); spec.alloc(N); accL.alloc(N); accR.alloc(N); work.alloc(N); timeL.alloc(N); timeR.alloc(N);
        tailL.assign(B, 0.f); tailR.assign(B, 0.f);
        hist.assign(2 * B, 0.f);
    }
    ~StereoConvolver() { if (setup) pffft_destroy_setup(setup); }
    StereoConvolver(const StereoConvolver&) = delete;
    StereoConvolver& operator=(const StereoConvolver&) = delete;

    int length() const { return B + P * B; }

    // in: mono input, adds the wet signal into outL/outR scaled by gain
    void process(const float* in, float* outL, float* outR, int n, float gain) {
        for (int i = 0; i < n; ++i) {
            const float x = in[i];
            // input history, written twice so the FIR reads one contiguous window
            hpos = (hpos + 1) & (B - 1);
            hist[(size_t)hpos] = x; hist[(size_t)hpos + B] = x;
            const float* hwin = &hist[(size_t)hpos + 1];   // oldest .. newest (B samples)
            float yl = 0.f, yr = 0.f;
            for (int k = 0; k < B; ++k) { const float v = hwin[k]; yl += headL[(size_t)(B - 1 - k)] * v; yr += headR[(size_t)(B - 1 - k)] * v; }
            yl += tailL[(size_t)pos]; yr += tailR[(size_t)pos];
            outL[i] += yl * gain; outR[i] += yr * gain;
            inBlock.p[B + pos] = x;
            if (++pos == B) { pos = 0; blockDone(); }
        }
    }

    void reset() {
        std::fill(hist.begin(), hist.end(), 0.f);
        std::fill(tailL.begin(), tailL.end(), 0.f); std::fill(tailR.begin(), tailR.end(), 0.f);
        if (P > 0) std::memset(fdl.p, 0, sizeof(float) * (size_t)P * N);
        std::memset(inBlock.p, 0, sizeof(float) * N);
        pos = 0; hpos = 0; fdlPos = 0;
    }

private:
    void blockDone() {
        if (P == 0) { std::memmove(inBlock.p, inBlock.p + B, sizeof(float) * B); return; }
        // spectrum of [previous block | this block] into the frequency-domain delay line
        float* slot = fdl.p + (size_t)fdlPos * N;
        pffft_transform(setup, inBlock.p, slot, work.p, PFFFT_FORWARD);
        std::memmove(inBlock.p, inBlock.p + B, sizeof(float) * B);
        std::memset(accL.p, 0, sizeof(float) * N);
        std::memset(accR.p, 0, sizeof(float) * N);
        const float scale = 1.f / (float)N;
        for (int p = 0; p < P; ++p) {
            const int idx = (fdlPos - p + P) % P;
            const float* X = fdl.p + (size_t)idx * N;
            pffft_zconvolve_accumulate(setup, X, kernL.p + (size_t)p * N, accL.p, scale);
            pffft_zconvolve_accumulate(setup, X, kernR.p + (size_t)p * N, accR.p, scale);
        }
        fdlPos = (fdlPos + 1) % P;
        pffft_transform(setup, accL.p, timeL.p, work.p, PFFFT_BACKWARD);
        pffft_transform(setup, accR.p, timeR.p, work.p, PFFFT_BACKWARD);
        // overlap-save: the second half is valid; it belongs to the next block
        std::memcpy(tailL.data(), timeL.p + B, sizeof(float) * B);
        std::memcpy(tailR.data(), timeR.p + B, sizeof(float) * B);
    }

    const int B, N;
    int P = 0;
    PFFFT_Setup* setup = nullptr;
    std::vector<float> headL, headR, tailL, tailR, hist;
    AlignedBuf<float> kernL, kernR, fdl, inBlock, spec, accL, accR, work, timeL, timeR;
    int pos = 0, hpos = 0, fdlPos = 0;
};

} // namespace pipe
