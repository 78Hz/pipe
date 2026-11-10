// Pipe - physically modelled pipe organ
// The building: listener distance, microphone technique, early reflections and
// an 8-line feedback delay network for the reverberant tail.
#pragma once

#include "Params.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

namespace pipe {

struct RoomShape { float rt60, hfRatio, size, lengthM, widthM; };
constexpr RoomShape kRoomShapes[kNumRooms] = {
    { 1.8f, 0.55f, 0.55f, 22.f, 9.f },    // village church
    { 4.2f, 0.50f, 1.00f, 42.f, 14.f },   // abbey nave
    { 7.5f, 0.45f, 1.60f, 90.f, 24.f },   // cathedral
    { 2.4f, 0.65f, 0.90f, 40.f, 26.f },   // concert hall
    { 0.0f, 1.0f, 0.30f, 6.f, 5.f },      // dry
};

struct DelayLine {
    std::vector<float> buf; int w = 0, mask = 0;
    void init(int maxLen) { int n = 1; while (n < maxLen + 4) n <<= 1; buf.assign(n, 0.f); mask = n - 1; w = 0; }
    inline void write(float x) { w = (w + 1) & mask; buf[w] = x; }
    inline float read(int d) const { return buf[(w - d) & mask]; }
    inline float readFrac(float d) const {
        int i = (int)d; float f = d - i;
        return buf[(w - i) & mask] * (1.f - f) + buf[(w - i - 1) & mask] * f;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
};

class Room {
public:
    void prepare(double sampleRate) {
        fs = (float)sampleRate;
        const int maxFdn = (int)(0.25f * fs * 1.7f) + 8;
        for (auto& d : fdn) d.init(maxFdn);
        pre.init((int)(0.1f * fs) + 8);
        early.init((int)(0.2f * fs) + 8);
        side.init((int)(0.002f * fs) + 8);
        cross.init((int)(0.002f * fs) + 8);
        configure();
    }
    void clear() {
        for (auto& d : fdn) d.clear();
        pre.clear(); early.clear(); side.clear(); cross.clear();
        for (float& s : damp) s = 0.f;
        airL = airR = 0.f; shL = shR = 0.f;
    }

    int preset = kRoomAbbey, mic = 0;
    float distance = 18.f, height = 3.f, width = 1.1f, mix = 0.55f, preDelayMs = 22.f;

    void configure() {
        const RoomShape& r = kRoomShapes[std::max(0, std::min(kNumRooms - 1, preset))];
        static const float base[8] = { 0.0297f, 0.0371f, 0.0411f, 0.0437f, 0.0533f, 0.0617f, 0.0683f, 0.0751f };
        for (int i = 0; i < 8; ++i) {
            len[i] = std::max(8, (int)(base[i] * r.size * fs * 1.6f));
            if (r.rt60 > 0) {
                const float t = len[i] / fs;
                gain[i] = std::pow(10.f, -3.f * t / r.rt60);
                const float gHf = std::pow(10.f, -3.f * t / (r.rt60 * r.hfRatio));
                // one-pole damping so high frequencies decay faster
                dampCoef[i] = std::max(0.f, std::min(0.95f, 1.f - gHf / std::max(1e-4f, gain[i])));
            } else { gain[i] = 0.f; dampCoef[i] = 0.f; }
        }
        wet = r.rt60 > 0 ? mix * 0.28f : 0.f;
        // direct sound falls off with distance; air absorbs highs
        directGain = 1.f / (1.f + distance / 6.f);
        const float airFc = 20000.f * std::exp(-distance / 45.f) * (1.f - 0.03f * height);
        airCoef = 1.f - std::exp(-2.f * (float)M_PI * airFc / fs);
        preDelay = std::max(1, (int)(preDelayMs * 0.001f * fs));
        // early reflections: floor, side walls, vault, west wall
        const float c = 343.f;
        const float d = std::max(1.f, distance);
        const float hw = r.widthM * 0.5f;
        const float paths[6] = {
            std::sqrt(d * d + 4.f * height * height),
            std::sqrt(d * d + 4.f * hw * hw),
            std::sqrt(d * d + 4.f * hw * hw) * 1.07f,
            std::sqrt(d * d + 4.f * (r.widthM * 0.9f) * (r.widthM * 0.9f)),
            d + 2.f * std::min(8.f, r.lengthM * 0.15f),
            std::sqrt(d * d + 4.f * 14.f * 14.f * r.size),
        };
        for (int i = 0; i < 6; ++i) {
            erDelay[i] = std::max(1, std::min((int)(0.19f * fs), (int)((paths[i] - d) / c * fs)));
            erGain[i] = r.rt60 > 0 ? 0.55f * d / paths[i] : 0.f;
        }
        sideDelay = std::max(1, (int)((mic == 0 ? 0.0006f : mic == 1 ? 0.00025f : 0.00065f) * fs));
    }

    // In-place: stereo dry from the pipes -> listener's stereo
    void process(float* L, float* R, int n) {
        for (int i = 0; i < n; ++i) {
            float l = L[i], r = R[i];
            // air absorption on the direct path
            airL += airCoef * (l - airL);
            airR += airCoef * (r - airR);
            float dl = airL * directGain, dr = airR * directGain;
            // microphone technique (stereo image of the direct sound)
            float m = 0.5f * (dl + dr), s = 0.5f * (dl - dr) * width;
            if (mic == 0) { side.write(s); s = 0.5f * (s + side.read(sideDelay)); }       // AB: spaced, wide, diffuse
            else if (mic == 1) { s *= 0.8f; }                                            // ORTF
            dl = m + s; dr = m - s;
            if (mic == 2) {                                                              // binaural: head shadow crossfeed
                cross.write(0.5f * (dl + dr));
                const float c = cross.read(sideDelay);
                shL += 0.2f * (c - shL);
                dl = dl * 0.85f + 0.15f * shL; dr = dr * 0.85f + 0.15f * shL;
            }

            // reverberant field (mono-ish input from both channels)
            const float in = 0.5f * (l + r);
            pre.write(in);
            const float x = pre.read(preDelay);
            early.write(x);
            float erL = 0.f, erR = 0.f;
            for (int k = 0; k < 6; ++k) {
                const float e = early.read(erDelay[k]) * erGain[k];
                if (k & 1) erR += e; else erL += e;
            }
            float o[8];
            for (int k = 0; k < 8; ++k) {
                float y = fdn[k].read(len[k]);
                damp[k] += (1.f - dampCoef[k]) * (y - damp[k]);
                o[k] = damp[k] * gain[k];
            }
            // Householder feedback matrix
            float sum = 0.f; for (int k = 0; k < 8; ++k) sum += o[k];
            sum *= 0.25f;
            for (int k = 0; k < 8; ++k) fdn[k].write(o[k] - sum + x * 0.35f + 1e-20f);
            const float tl = o[0] + o[2] + o[4] + o[6];
            const float tr = o[1] + o[3] + o[5] + o[7];
            const float wl = (erL * 0.6f + tl) * wet, wr = (erR * 0.6f + tr) * wet;
            const float ws = 0.5f * (wl - wr) * std::min(1.5f, 0.5f + width);
            const float wm = 0.5f * (wl + wr);
            L[i] = dl + wm + ws;
            R[i] = dr + wm - ws;
        }
    }

private:
    float fs = 48000.f;
    DelayLine fdn[8], pre, early, side, cross;
    int len[8] = {};
    float gain[8] = {}, dampCoef[8] = {}, damp[8] = {};
    int erDelay[6] = {}; float erGain[6] = {};
    float wet = 0.f, directGain = 1.f, airCoef = 1.f, airL = 0.f, airR = 0.f;
    int preDelay = 1, sideDelay = 1;
    float shL = 0.f, shR = 0.f;
};

} // namespace pipe
