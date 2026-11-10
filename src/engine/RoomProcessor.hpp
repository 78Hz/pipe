// Pipe - physically modelled pipe organ
// The building: dry, lite (algorithmic), measured (convolution) or custom IR.
#pragma once

#include "ConvRoom.hpp"
#include "Room.hpp"

namespace pipe {

// Direct sound for convolution rooms: distance, air absorption and the
// microphone image of the pipes' positions in the case.
struct HeadShadow {
    float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    void design(double fs, double angleToEarDeg) {
        const double a = 0.0875, c = 343.0, w0 = c / a;
        const double alpha = 1.05 + 0.95 * std::cos(angleToEarDeg / 150.0 * M_PI);
        const double K = 2.0 * fs, tau = 1.0 / (2.0 * w0);
        b0 = (float)((1.0 + alpha * tau * K) / (1.0 + tau * K));
        b1 = (float)((1.0 - alpha * tau * K) / (1.0 + tau * K));
        a1 = (float)((1.0 - tau * K) / (1.0 + tau * K));
    }
    inline float tick(float x) { const float y = b0 * x + b1 * x1 - a1 * y1; x1 = x; y1 = y; return y; }
};

class DirectPath {
public:
    void prepare(double sampleRate) {
        fs = (float)sampleRate;
        side.init((int)(0.002f * fs) + 8);
        dL.init((int)(0.002f * fs) + 8); dR.init((int)(0.002f * fs) + 8);
        // binaural: the dry stereo pair as sources at +/-30 degrees
        ipsiL.design(sampleRate, 60.0); contraL.design(sampleRate, 120.0);
        ipsiR.design(sampleRate, 60.0); contraR.design(sampleRate, 120.0);
        const double a = 0.0875, c = 343.0;
        itd = (int)std::lround((a / c) * (60.0 * M_PI / 180.0 + std::sin(60.0 * M_PI / 180.0)) * 0.5 * sampleRate);
        configure();
    }
    float distance = 18.f, height = 3.f, width = 1.1f; int mic = 0; bool mute = false;
    void configure() {
        gain = mute ? 0.f : 1.f / (1.f + distance / 6.f);
        const float airFc = 20000.f * std::exp(-distance / 45.f) * (1.f - 0.03f * height);
        airCoef = 1.f - std::exp(-2.f * (float)M_PI * airFc / fs);
        sideDelay = std::max(1, (int)((mic == 0 ? 0.0006f : 0.00025f) * fs));
    }
    float level() const { return gain; }
    void process(float* L, float* R, int n) {
        for (int i = 0; i < n; ++i) {
            airL += airCoef * (L[i] - airL); airR += airCoef * (R[i] - airR);
            float l = airL * gain, r = airR * gain;
            if (mic == 2) {
                dL.write(l); dR.write(r);
                const float outL = ipsiL.tick(l) + contraL.tick(dR.read(itd));
                const float outR = ipsiR.tick(r) + contraR.tick(dL.read(itd));
                L[i] = outL * 0.7f; R[i] = outR * 0.7f;
            } else {
                float m = 0.5f * (l + r), s = 0.5f * (l - r) * width;
                if (mic == 0) { side.write(s); s = 0.5f * (s + side.read(sideDelay)); } else s *= 0.8f;
                L[i] = m + s; R[i] = m - s;
            }
        }
    }
    void clear() { side.clear(); dL.clear(); dR.clear(); airL = airR = 0.f; }
private:
    float fs = 48000.f, gain = 1.f, airCoef = 1.f, airL = 0.f, airR = 0.f;
    int sideDelay = 1, itd = 1;
    DelayLine side, dL, dR;
    HeadShadow ipsiL, contraL, ipsiR, contraR;
};

class RoomProcessor {
public:
    void prepare(double sampleRate) {
        fs = sampleRate;
        lite.prepare(sampleRate);
        direct.prepare(sampleRate);
        mono.assign(4096, 0.f);
        push();
    }
    void clear() { lite.clear(); direct.clear(); conv.clear(); }

    // settings (audio thread)
    int room = kRoomStMargarets, mic = 0, quality = 1, order = 0;
    float distance = 18.f, height = 3.f, width = 1.1f, mix = 0.55f, preDelayMs = 22.f;
    bool trimDirect = true;

    void configure() {
        lite.preset = (roomIsMeasured(room) && !conv.hasMeasuredData()) ? 1 : roomLiteIndex(room);   // no data in this build: abbey
        lite.mic = mic; lite.distance = distance; lite.height = height;
        lite.width = width; lite.mix = mix; lite.preDelayMs = preDelayMs;
        lite.configure();
        direct.distance = distance; direct.height = height; direct.width = width; direct.mic = mic;
        direct.mute = !trimDirect;
        direct.configure();
        push();
    }
    void setCustomPath(const std::string& p) { conv.setCustomPath(p); }

    void process(float* L, float* R, int n) {
        if (!roomIsConvolution(room) || (roomIsMeasured(room) && !conv.hasMeasuredData())) {
            lite.process(L, R, n);
            return;
        }
        int done = 0;
        while (done < n) {
            const int len = std::min(n - done, (int)mono.size());
            for (int i = 0; i < len; ++i) mono[(size_t)i] = (float)M_SQRT1_2 * (L[done + i] + R[done + i]);   // keeps the power of the (decorrelated) pipe pair
            direct.process(L + done, R + done, len);
            // Level-matched rooms: the reverb has unit loudness (see irtools::levelMatch). Distance sets the
            // direct sound, ROOM MIX the reverb, and the sum is kept at a constant loudness, so moving away
            // makes the organ more reverberant rather than quieter, and no room is louder than another.
            const float gd = direct.level();
            const float w = trimDirect ? mix : 1.f;   // a response played as recorded carries its own balance
            const float norm = kRoomLoudness / std::sqrt(gd * gd + w * w + 1e-9f);
            for (int i = 0; i < len; ++i) { L[done + i] *= norm; R[done + i] *= norm; }
            conv.process(mono.data(), L + done, R + done, len, w * norm);
            done += len;
        }
    }

    int status() const {
        if (!roomIsConvolution(room)) return kRoomReady;
        if (roomIsMeasured(room) && !conv.hasMeasuredData()) return kRoomNoData;
        return conv.status.load();
    }
    int usedPosition() const { return conv.usedPosition.load(); }
    float usedDistance() const { return conv.usedDistance.load(); }

private:
    void push() {
        RoomRequest r;
        r.room = room; r.mic = mic; r.width = width; r.distance = distance; r.trimDirect = trimDirect;
        r.order = order; r.sampleRate = fs; r.block = quality == 0 ? 1024 : 512;
        conv.request(r);
    }
    double fs = 48000.0;
    LiteRoom lite;
    DirectPath direct;
    ConvolutionRoom conv;
    std::vector<float> mono;
};

} // namespace pipe
