// Pipe - physically modelled pipe organ
// Minimal WAV reader for user impulse responses (PCM 16/24/32, float 32/64).
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace pipe {

struct WavData {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;   // interleaved
    size_t frames() const { return channels ? samples.size() / (size_t)channels : 0; }
};

inline bool readWavFile(const std::string& path, WavData& out, std::string& error, size_t maxFrames = 48000 * 40) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "CAN'T OPEN FILE"; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (d.size() < 44 || std::memcmp(&d[0], "RIFF", 4) != 0 || std::memcmp(&d[8], "WAVE", 4) != 0) { error = "NOT A WAV FILE"; return false; }
    size_t i = 12, fmtOff = 0, dataOff = 0, dataSize = 0;
    while (i + 8 <= d.size()) {
        const uint32_t sz = u32(i + 4);
        if (std::memcmp(&d[i], "fmt ", 4) == 0) fmtOff = i + 8;
        if (std::memcmp(&d[i], "data", 4) == 0) { dataOff = i + 8; dataSize = std::min<size_t>(sz, d.size() - dataOff); }
        i += 8 + sz + (sz & 1);
    }
    if (!fmtOff || !dataOff) { error = "WAV HAS NO AUDIO DATA"; return false; }
    uint32_t tag = u16(fmtOff);
    const int ch = (int)u16(fmtOff + 2);
    const int sr = (int)u32(fmtOff + 4);
    const int bits = (int)u16(fmtOff + 14);
    if (tag == 0xFFFE && fmtOff + 26 <= d.size()) tag = u16(fmtOff + 24);
    if (ch < 1 || ch > 16 || sr < 8000 || sr > 384000) { error = "UNSUPPORTED WAV FORMAT"; return false; }
    const int bytes = bits / 8;
    if (bytes < 2 || bytes > 8) { error = "UNSUPPORTED BIT DEPTH"; return false; }
    size_t frames = dataSize / (size_t)(bytes * ch);
    frames = std::min(frames, maxFrames);
    out.sampleRate = sr; out.channels = ch;
    out.samples.resize(frames * (size_t)ch);
    const uint8_t* p = &d[dataOff];
    for (size_t n = 0; n < frames * (size_t)ch; ++n, p += bytes) {
        float v;
        if (tag == 3 && bytes == 4) { float x; std::memcpy(&x, p, 4); v = x; }
        else if (tag == 3 && bytes == 8) { double x; std::memcpy(&x, p, 8); v = (float)x; }
        else if (bytes == 2) v = (int16_t)(p[0] | (p[1] << 8)) / 32768.f;
        else if (bytes == 3) { int32_t x = p[0] | (p[1] << 8) | (p[2] << 16); if (x & 0x800000) x -= 0x1000000; v = x / 8388608.f; }
        else if (bytes == 4) { int32_t x; std::memcpy(&x, p, 4); v = x / 2147483648.f; }
        else { error = "UNSUPPORTED SAMPLE FORMAT"; return false; }
        out.samples[n] = v;
    }
    return frames > 0;
}

} // namespace pipe
