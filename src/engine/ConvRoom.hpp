// Pipe - physically modelled pipe organ
// Measured rooms: picks a measured listening position, decodes its B-format
// response for the chosen microphones (spaced AB, ORTF, or binaural through a
// spherical-head model), resamples it and convolves. All preparation happens
// on a background thread; the audio thread only swaps in the finished
// convolver and crossfades.
#pragma once

#include "Convolver.hpp"
#include "IrData.hpp"
#include "Params.hpp"
#include "Wav.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pipe {

enum RoomStatus { kRoomReady = 0, kRoomBuilding, kRoomNoData, kRoomCustomError, kRoomCustomMissing };

struct RoomRequest {
    int room = kRoomStMargarets;
    int mic = 0;            // 0 AB, 1 ORTF, 2 binaural
    float width = 1.f;
    float distance = 10.f;
    bool trimDirect = true;
    int order = 0;          // custom 4-channel files: 0 FuMa, 1 AmbiX
    double sampleRate = 48000.0;
    int block = 512;
};

// ---- offline helpers (worker thread) -------------------------------------
namespace irtools {

inline std::vector<float> resample(const std::vector<float>& x, double from, double to) {
    if (std::fabs(from - to) < 1e-6 || x.empty()) return x;
    const double ratio = to / from;
    const size_t outLen = (size_t)std::ceil(x.size() * ratio);
    std::vector<float> y(outLen);
    const double fc = std::min(1.0, ratio) * 0.95;
    const int half = 24;
    for (size_t m = 0; m < outLen; ++m) {
        const double t = m / ratio;
        const long c = (long)std::floor(t);
        double acc = 0;
        for (long k = c - half + 1; k <= c + half; ++k) {
            if (k < 0 || k >= (long)x.size()) continue;
            const double d = t - (double)k;
            const double arg = M_PI * fc * d;
            const double s = std::fabs(arg) < 1e-9 ? 1.0 : std::sin(arg) / arg;
            const double w = 0.5 + 0.5 * std::cos(M_PI * d / half);
            acc += x[(size_t)k] * fc * s * w;
        }
        y[m] = (float)acc;
    }
    return y;
}

inline int findOnset(const std::vector<float>& w) {
    float peak = 0.f;
    for (float v : w) peak = std::max(peak, std::fabs(v));
    for (size_t i = 0; i < w.size(); ++i) if (std::fabs(w[i]) > 0.3f * peak) return (int)i;
    return 0;
}

// fractional delay + Brown-Duda head shadow for one ear, accumulated into out
inline void earFilter(const std::vector<float>& s, double fs, double angleToEarDeg, std::vector<float>& out) {
    const double a = 0.0875, c = 343.0;
    const double d = angleToEarDeg * M_PI / 180.0;
    const double itd = d < M_PI / 2 ? -(a / c) * std::cos(d) : (a / c) * (d - M_PI / 2);
    const double delay = (itd + a / c) * fs;                 // >= 0
    const double alpha = 1.05 + 0.95 * std::cos(angleToEarDeg / 150.0 * M_PI);
    const double w0 = c / a;
    // H(s) = (1 + alpha s/(2 w0)) / (1 + s/(2 w0)), bilinear transform
    const double K = 2.0 * fs, tau = 1.0 / (2.0 * w0);
    const double b0 = (1.0 + alpha * tau * K) / (1.0 + tau * K), b1 = (1.0 - alpha * tau * K) / (1.0 + tau * K);
    const double a1 = (1.0 - tau * K) / (1.0 + tau * K);
    double x1 = 0, y1 = 0;
    const int di = (int)delay; const double df = delay - di;
    for (size_t n = 0; n < out.size(); ++n) {
        const long i0 = (long)n - di, i1 = i0 - 1;
        const double v = (i0 >= 0 && i0 < (long)s.size() ? s[(size_t)i0] : 0.0) * (1 - df) + (i1 >= 0 && i1 < (long)s.size() ? s[(size_t)i1] : 0.0) * df;
        const double y = b0 * v + b1 * x1 - a1 * y1;
        x1 = v; y1 = y;
        out[n] += (float)y;
    }
}

// ---- level matching -------------------------------------------------------------------------------
// Every reverb (built-in, custom file, any microphone decoder) is scaled to the same loudness for
// sustained sound: its average power gain over the organ's main range (63 Hz - 4 kHz, in 1/3-octave
// bands) becomes 1, and no 1/3-octave band (a room resonance) may stand more than 6 dB above that.
// Before that, the very bottom of the reverb is tidied: 28 Hz high-pass and a gentle low shelf.

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    void apply(std::vector<float>& x) const {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& v : x) {
            const double in = v, y = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in; y2 = y1; y1 = y; v = (float)y;
        }
    }
    static Biquad highpass(double f, double fs) {
        const double w = 2 * M_PI * f / fs, c = std::cos(w), al = std::sin(w) / (2 * M_SQRT1_2);
        Biquad q; const double a0 = 1 + al;
        q.b0 = (1 + c) / 2 / a0; q.b1 = -(1 + c) / a0; q.b2 = (1 + c) / 2 / a0; q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
        return q;
    }
    static Biquad lowShelf(double f, double fs, double dB) {
        const double A = std::pow(10.0, dB / 40.0), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w);
        const double al = s / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
        Biquad q; const double a0 = (A + 1) + (A - 1) * c + sq;
        q.b0 = A * ((A + 1) - (A - 1) * c + sq) / a0; q.b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
        q.b2 = A * ((A + 1) - (A - 1) * c - sq) / a0; q.a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
        q.a2 = ((A + 1) + (A - 1) * c - sq) / a0;
        return q;
    }
};

// -> gain applied (1 = unchanged); 0 when the response is silent
inline float levelMatch(std::vector<float>& L, std::vector<float>& R, double fs) {
    Biquad::highpass(28.0, fs).apply(L); Biquad::highpass(28.0, fs).apply(R);
    const size_t len = std::max(L.size(), R.size());
    int N = 1024; while ((size_t)N < len && N < (1 << 21)) N <<= 1;
    PFFFT_Setup* setup = pffft_new_setup(N, PFFFT_REAL);
    if (!setup) return 1.f;
    float* in = (float*)pffft_aligned_malloc(sizeof(float) * N);
    float* out = (float*)pffft_aligned_malloc(sizeof(float) * N);
    float* work = (float*)pffft_aligned_malloc(sizeof(float) * N);
    std::vector<double> pw((size_t)N / 2, 0.0);   // |H|^2, averaged over L and R
    for (int ch = 0; ch < 2; ++ch) {
        const std::vector<float>& h = ch == 0 ? L : R;
        for (int i = 0; i < N; ++i) in[i] = (size_t)i < h.size() ? h[(size_t)i] : 0.f;
        pffft_transform_ordered(setup, in, out, work, PFFFT_FORWARD);
        for (int k = 1; k < N / 2; ++k) pw[(size_t)k] += 0.5 * ((double)out[2 * k] * out[2 * k] + (double)out[2 * k + 1] * out[2 * k + 1]);
    }
    pffft_aligned_free(in); pffft_aligned_free(out); pffft_aligned_free(work);
    pffft_destroy_setup(setup);
    // 1/3-octave bands from 31.5 Hz to 8 kHz
    double sumMain = 0; int nMain = 0; double maxBand = 0;
    for (int b = -15; b <= 9; ++b) {
        const double fc = 1000.0 * std::pow(2.0, b / 3.0);
        const int k0 = std::max(1, (int)(fc / std::pow(2.0, 1.0 / 6.0) / fs * N));
        const int k1 = std::min(N / 2 - 1, (int)(fc * std::pow(2.0, 1.0 / 6.0) / fs * N));
        if (k1 < k0) continue;
        double e = 0; for (int k = k0; k <= k1; ++k) e += pw[(size_t)k];
        e /= (k1 - k0 + 1);
        if (fc >= 60.0 && fc <= 4100.0) { sumMain += e; ++nMain; }
        if (fc >= 40.0) maxBand = std::max(maxBand, e);
    }
    const double mean = nMain ? sumMain / nMain : 0.0;
    if (!(mean > 1e-20)) return 0.f;
    double g = 1.0 / std::sqrt(mean);
    if (maxBand > 4.0 * mean) g *= std::sqrt(4.0 * mean / maxBand);   // resonance cap: +6 dB
    for (auto& v : L) v = (float)(v * g);
    for (auto& v : R) v = (float)(v * g);
    // the low shelf is applied after measuring: it trims the boom at the bottom, not the room's loudness
    Biquad::lowShelf(120.0, fs, -3.0).apply(L); Biquad::lowShelf(120.0, fs, -3.0).apply(R);
    return (float)g;
}

} // namespace irtools

class ConvolutionRoom {
public:
    ConvolutionRoom() : entries(irEntries()) {
        worker = std::thread([this] { run(); });
    }
    ~ConvolutionRoom() {
        quit = true;
        if (worker.joinable()) worker.join();
        delete incoming.exchange(nullptr);
        delete retired.exchange(nullptr);
        delete current; delete fading; delete waitingToRetire;
    }

    bool hasMeasuredData() const { return !entries.empty(); }

    // audio thread: publish the wanted configuration (no locks, no allocation)
    void request(const RoomRequest& r) {
        if (roomIsConvolution(r.room)) status.store(kRoomBuilding);   // until the worker has looked at it
        reqRoom.store(r.room);
        reqVersion.fetch_add(1, std::memory_order_acq_rel);
        req = r;
        reqVersion.fetch_add(1, std::memory_order_release);
    }
    // host/UI thread
    void setCustomPath(const std::string& path) {
        std::lock_guard<std::mutex> lk(pathMutex);
        customPath = path;
        if (reqRoom.load() == kRoomCustom) status.store(kRoomBuilding);
        pathVersion.fetch_add(1);
    }

    std::atomic<int> status{kRoomBuilding};
    std::atomic<int> reqRoom{-1};
    std::atomic<int> usedPosition{-1};
    std::atomic<float> usedDistance{0.f};

    // audio thread: mono in, wet added to L/R
    void process(const float* in, float* L, float* R, int n, float gain) {
        if (StereoConvolver* c = incoming.exchange(nullptr, std::memory_order_acq_rel)) {
            if (fading) retire(fading);
            fading = current;
            current = c;
            fadePos = 0;
        }
        if (!current && !fading) return;
        if (!fading) { current->process(in, L, R, n, gain); return; }
        // crossfade old -> new
        int done = 0;
        while (done < n) {
            const int len = std::min(n - done, 64);
            const float g0 = (float)fadePos / kFade, g1 = (float)std::min(kFade, fadePos + len) / kFade;
            const float g = 0.5f * (g0 + g1);
            if (current) current->process(in + done, L + done, R + done, len, gain * g);
            fading->process(in + done, L + done, R + done, len, gain * (1.f - g));
            fadePos += len; done += len;
            if (fadePos >= kFade) { retire(fading); fading = nullptr; if (done < n && current) current->process(in + done, L + done, R + done, n - done, gain); break; }
        }
        if (waitingToRetire && !retired.load()) { retired.store(waitingToRetire); waitingToRetire = nullptr; }
    }

    void clear() { if (current) current->reset(); if (fading) fading->reset(); }

private:
    static constexpr int kFade = 4096;
    void retire(StereoConvolver* c) {
        if (!c) return;
        StereoConvolver* expected = nullptr;
        if (!retired.compare_exchange_strong(expected, c)) { if (!waitingToRetire) waitingToRetire = c; }
    }

    // no usable response: fade the previous room out rather than keep playing it
    void publishSilence(const RoomRequest& r) {
        const std::vector<float> z(1, 0.f);
        delete incoming.exchange(new StereoConvolver(z, z, r.block));
        usedPosition.store(-1);
        usedDistance.store(0.f);
    }

    void run() {
        uint32_t seenReq = 0, seenPath = 0;
        while (!quit) {
            delete retired.exchange(nullptr);
            uint32_t v1 = reqVersion.load(std::memory_order_acquire);
            const uint32_t pv = pathVersion.load();
            if ((v1 != seenReq || pv != seenPath) && !(v1 & 1)) {
                RoomRequest r = req;
                if (reqVersion.load(std::memory_order_acquire) == v1) {
                    const bool rebuild = (v1 != seenReq && needsRebuild(r)) || (pv != seenPath && r.room == kRoomCustom);
                    seenReq = v1; seenPath = pv;
                    if (rebuild && roomIsConvolution(r.room)) build(r, pv);
                    else if (roomIsConvolution(r.room)) status.store(lastStatus);
                    last = r; haveLast = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    bool needsRebuild(const RoomRequest& r) const {
        if (!haveLast) return true;
        if (r.room != last.room || r.mic != last.mic || std::fabs(r.width - last.width) > 1e-3f || r.trimDirect != last.trimDirect
            || r.order != last.order || r.sampleRate != last.sampleRate || r.block != last.block) return true;
        // distance only matters when it selects another measured position
        return roomIsMeasured(r.room) && pickPositions(r).first != pickPositions(last).first;
    }

    // -> (primary position index into entries, AB partner or -1)
    std::pair<int, int> pickPositions(const RoomRequest& r) const {
        int best = -1; float bestD = 1e9f;
        for (int i = 0; i < (int)entries.size(); ++i) {
            if (entries[(size_t)i].room != r.room) continue;
            const float d = std::fabs(entries[(size_t)i].distance - r.distance);
            if (d < bestD - 1e-3f) { bestD = d; best = i; }
        }
        int partner = -1;
        if (best >= 0 && r.mic == 0) {
            for (int i = 0; i < (int)entries.size(); ++i)
                if (i != best && entries[(size_t)i].room == r.room && std::fabs(entries[(size_t)i].distance - entries[(size_t)best].distance) < 0.6f) { partner = i; break; }
        }
        return { best, partner };
    }

    void loadEntry(const IrEntry& e, std::vector<float>& w, std::vector<float>& x, std::vector<float>& y, int& onset) const {
        w.resize((size_t)e.frames); x.resize((size_t)e.frames); y.resize((size_t)e.frames);
        for (int i = 0; i < e.frames; ++i) {
            w[(size_t)i] = e.data[i * e.channels] * e.scale;
            x[(size_t)i] = e.data[i * e.channels + 1] * e.scale;
            y[(size_t)i] = e.data[i * e.channels + 2] * e.scale;
        }
        onset = e.onset;
    }

    void build(const RoomRequest& r, uint32_t pathVer) {
        status.store(kRoomBuilding);
        std::vector<float> L, R;
        double srcRate = 48000.0;
        bool ok = false;
        if (roomIsMeasured(r.room)) {
            const auto pp = pickPositions(r);
            if (pp.first < 0) { lastStatus = kRoomNoData; status.store(kRoomNoData); return; }
            std::vector<float> w, x, y; int onset = 0;
            loadEntry(entries[(size_t)pp.first], w, x, y, onset);
            if (r.mic == 0 && pp.second >= 0) {
                std::vector<float> w2, x2, y2; int on2 = 0;
                loadEntry(entries[(size_t)pp.second], w2, x2, y2, on2);
                trimAndAlign(w, onset, r.trimDirect); trimAndAlign(w2, on2, r.trimDirect);
                const size_t n = std::max(w.size(), w2.size());
                L.assign(n, 0.f); R.assign(n, 0.f);
                for (size_t i = 0; i < w.size(); ++i) L[i] = (float)M_SQRT2 * w[i];
                for (size_t i = 0; i < w2.size(); ++i) R[i] = (float)M_SQRT2 * w2[i];
            } else {
                trimAndAlign(w, onset, r.trimDirect, &x, &y);
                decode(w, x, y, r.mic, srcRate, L, R);
            }
            usedPosition.store(entries[(size_t)pp.first].pos);
            usedDistance.store(entries[(size_t)pp.first].distance);
            ok = true;
        } else if (r.room == kRoomCustom) {
            std::string path;
            { std::lock_guard<std::mutex> lk(pathMutex); path = customPath; }
            (void)pathVer;
            if (path.empty()) { publishSilence(r); lastStatus = kRoomCustomMissing; status.store(kRoomCustomMissing); return; }
            WavData wav; std::string err;
            if (!readWavFile(path, wav, err)) { publishSilence(r); lastStatus = kRoomCustomError; status.store(kRoomCustomError); return; }
            srcRate = wav.sampleRate;
            const size_t n = wav.frames();
            auto chan = [&](int c) { std::vector<float> v(n); for (size_t i = 0; i < n; ++i) v[i] = wav.samples[i * (size_t)wav.channels + (size_t)c]; return v; };
            if (wav.channels >= 3) {
                std::vector<float> w, x, y;
                if (r.order == 1 && wav.channels >= 4) {           // AmbiX: ACN W Y Z X, SN3D
                    w = chan(0); y = chan(1); x = chan(3);
                    for (auto& v : w) v *= (float)M_SQRT1_2;
                } else { w = chan(0); x = chan(1); y = chan(2); }   // FuMa: W X Y Z
                int onset = irtools::findOnset(w);
                normaliseDirect(w, onset, &x, &y);
                trimAndAlign(w, onset, r.trimDirect, &x, &y);
                decode(w, x, y, r.mic, srcRate, L, R);
            } else {
                L = chan(0); R = wav.channels > 1 ? chan(1) : L;
                std::vector<float> mono(n); for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (L[i] + R[i]);
                int onset = irtools::findOnset(mono);
                normaliseDirect(mono, onset, &L, &R);
                trimAndAlign(L, onset, r.trimDirect, &R);
            }
            usedPosition.store(-1);
            usedDistance.store(0.f);
            ok = true;
        }
        if (!ok) return;
        // stereo width (M/S) for the microphone pairs
        if (r.mic != 2) {
            const size_t n = std::min(L.size(), R.size());
            for (size_t i = 0; i < n; ++i) {
                const float m = 0.5f * (L[i] + R[i]), s = 0.5f * (L[i] - R[i]) * r.width;
                L[i] = m + s; R[i] = m - s;
            }
        }
        L = irtools::resample(L, srcRate, r.sampleRate);
        R = irtools::resample(R, srcRate, r.sampleRate);
        if (irtools::levelMatch(L, R, r.sampleRate) <= 0.f) {
            publishSilence(r);
            lastStatus = r.room == kRoomCustom ? kRoomCustomError : kRoomNoData;
            status.store(lastStatus);
            return;
        }
        auto* conv = new StereoConvolver(L, R, r.block);
        delete incoming.exchange(conv);
        lastStatus = kRoomReady;
        status.store(kRoomReady);
    }

    static void normaliseDirect(std::vector<float>& w, int onset, std::vector<float>* a = nullptr, std::vector<float>* b = nullptr) {
        const int sr = 48000;   // window length in samples is not critical here
        double e = 0;
        for (int i = std::max(0, onset - sr / 2000); i < std::min((int)w.size(), onset + sr / 400); ++i) e += (double)w[(size_t)i] * w[(size_t)i];
        const float g = e > 0 ? (float)(1.0 / std::sqrt(e)) : 1.f;
        for (auto& v : w) v *= g;
        if (a) for (auto& v : *a) v *= g;
        if (b) for (auto& v : *b) v *= g;
    }

    // Index 0 becomes the direct sound's arrival. With trimDirect the direct
    // sound itself is removed (the engine models it, with each pipe's position).
    static void trimAndAlign(std::vector<float>& w, int onset, bool trimDirect, std::vector<float>* a = nullptr, std::vector<float>* b = nullptr) {
        const int start = trimDirect ? onset : std::max(0, onset - 48);
        auto cut = [&](std::vector<float>& v) {
            if (start > 0 && start < (int)v.size()) v.erase(v.begin(), v.begin() + start);
            if (trimDirect) {
                const int gap = 120, ramp = 48;   // 2.5 ms of direct sound removed, 1 ms fade-in
                for (int i = 0; i < (int)v.size() && i < gap + ramp; ++i) v[(size_t)i] *= i < gap ? 0.f : (float)(i - gap) / ramp;
            }
        };
        cut(w); if (a) cut(*a); if (b) cut(*b);
    }

    static void decode(const std::vector<float>& w, const std::vector<float>& x, const std::vector<float>& y, int mic, double fs,
                       std::vector<float>& L, std::vector<float>& R) {
        const size_t n = w.size();
        const float s2 = (float)M_SQRT2;
        L.assign(n, 0.f); R.assign(n, 0.f);
        if (mic == 1) {                       // ORTF: cardioids at +/-55 degrees
            const float c = std::cos(55.f * (float)M_PI / 180.f), s = std::sin(55.f * (float)M_PI / 180.f);
            for (size_t i = 0; i < n; ++i) {
                L[i] = 0.5f * (s2 * w[i] + c * x[i] + s * y[i]);
                R[i] = 0.5f * (s2 * w[i] + c * x[i] - s * y[i]);
            }
        } else if (mic == 2) {                // binaural: 8 virtual sources through a spherical head
            const int nv = 8;
            std::vector<float> sig(n);
            L.assign(n + 64, 0.f); R.assign(n + 64, 0.f);
            for (int k = 0; k < nv; ++k) {
                const double th = 2.0 * M_PI * k / nv;
                const float c = (float)std::cos(th), s = (float)std::sin(th);
                for (size_t i = 0; i < n; ++i) sig[i] = (s2 * w[i] + 2.f * (c * x[i] + s * y[i])) / (float)nv;
                double deg = th * 180.0 / M_PI;
                auto wrap = [](double a) { a = std::fmod(a + 540.0, 360.0) - 180.0; return std::fabs(a); };
                irtools::earFilter(sig, fs, wrap(deg - 90.0), L);
                irtools::earFilter(sig, fs, wrap(deg + 90.0), R);
            }
        } else {                              // AB from a single point: wide sub-cardioids, spaced by delay
            const float c = std::cos(70.f * (float)M_PI / 180.f), s = std::sin(70.f * (float)M_PI / 180.f);
            const int lag = (int)(0.0006 * fs);
            for (size_t i = 0; i < n; ++i) {
                L[i] = 0.7f * s2 * w[i] + 0.3f * (c * x[i] + s * y[i]);
                const size_t j = i >= (size_t)lag ? i - (size_t)lag : 0;
                R[i] = i >= (size_t)lag ? 0.7f * s2 * w[j] + 0.3f * (c * x[j] - s * y[j]) : 0.f;
            }
        }
    }

    std::vector<IrEntry> entries;
    std::thread worker;
    std::atomic<bool> quit{false};
    std::atomic<uint32_t> reqVersion{0}, pathVersion{0};
    RoomRequest req, last;
    bool haveLast = false;
    int lastStatus = kRoomBuilding;
    std::mutex pathMutex;
    std::string customPath;
    std::atomic<StereoConvolver*> incoming{nullptr}, retired{nullptr};
    StereoConvolver* current = nullptr;
    StereoConvolver* fading = nullptr;
    StereoConvolver* waitingToRetire = nullptr;
    int fadePos = 0;
};

} // namespace pipe
