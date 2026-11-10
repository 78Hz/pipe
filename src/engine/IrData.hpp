// Pipe - physically modelled pipe organ
// Access to the measured rooms embedded in the DSP binary (tools/prepare_irs.py).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace pipe {

// Defined in IrBlob.cpp (empty when the build has no measured rooms).
const unsigned char* irBlobData();
size_t irBlobSize();

struct IrEntry {
    int room = -1, pos = -1;
    float distance = 0.f, scale = 1.f;
    int frames = 0, channels = 0, onset = 0;
    const int16_t* data = nullptr;   // interleaved W, X, Y
};

inline std::vector<IrEntry> irEntries() {
    std::vector<IrEntry> out;
    const unsigned char* b = irBlobData();
    const size_t size = irBlobSize();
    if (!b || size < 12 || std::memcmp(b, "PIPEIR01", 8) != 0) return out;
    uint32_t count; std::memcpy(&count, b + 8, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* h = b + 12 + 32 * i;
        if ((size_t)(h + 32 - b) > size) break;
        uint32_t room, pos, frames, ch, onset, offset; float dist, scale;
        std::memcpy(&room, h, 4); std::memcpy(&pos, h + 4, 4); std::memcpy(&dist, h + 8, 4);
        std::memcpy(&frames, h + 12, 4); std::memcpy(&ch, h + 16, 4); std::memcpy(&onset, h + 20, 4);
        std::memcpy(&scale, h + 24, 4); std::memcpy(&offset, h + 28, 4);
        if ((size_t)offset + (size_t)frames * ch * 2 > size) break;
        IrEntry e;
        e.room = (int)room; e.pos = (int)pos; e.distance = dist; e.frames = (int)frames;
        e.channels = (int)ch; e.onset = (int)onset; e.scale = scale;
        e.data = reinterpret_cast<const int16_t*>(b + offset);
        out.push_back(e);
    }
    return out;
}

} // namespace pipe
