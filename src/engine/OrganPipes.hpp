// Pipe - physically modelled pipe organ
// The organ's flue pipes, precomputed (tools/opus14_organ.cpp -> data/opus14/organ_pipes.bin,
// embedded at build time): one FluePipeSpec per pipe, found by rank and key.
#pragma once

#include "FlueRT.hpp"
#include "ReedModel.hpp"
#include "Opus14Organ.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pipe {

const unsigned char* organBlobData();
size_t organBlobSize();

struct OrganPipeTable {
    const FluePipeSpec* specs = nullptr;
    uint32_t count = 0;
    const ReedPipeSpec* reeds = nullptr;
    uint32_t reedCount = 0;
    OrganPipeTable() {
        const unsigned char* d = organBlobData();
        const size_t n = organBlobSize();
        if (!d || n < 16 || std::memcmp(d, "P14", 4) != 0) return;
        uint32_t ver, cnt, sz;
        std::memcpy(&ver, d + 4, 4); std::memcpy(&cnt, d + 8, 4); std::memcpy(&sz, d + 12, 4);
        if (ver != 2 || sz != sizeof(FluePipeSpec) || 16 + (size_t)cnt * sz > n) return;
        specs = reinterpret_cast<const FluePipeSpec*>(d + 16);   // 16-byte aligned in the blob
        count = cnt;
        size_t pos = 16 + (size_t)cnt * sz;
        while (pos % 16 != 8) ++pos;
        if (pos + 8 > n) return;
        uint32_t nr, szr; std::memcpy(&nr, d + pos, 4); std::memcpy(&szr, d + pos + 4, 4);
        if (szr != sizeof(ReedPipeSpec) || pos + 8 + (size_t)nr * szr > n) return;
        reeds = reinterpret_cast<const ReedPipeSpec*>(d + pos + 8);
        reedCount = nr;
    }
};
inline const OrganPipeTable& organPipes() { static const OrganPipeTable t; return t; }

// The flue pipe at (rank, key), or nullptr (reed rank, or no pipe there).
inline const FluePipeSpec* organFlueSpec(int rank, int key) {
    if (rank < 0 || rank >= organ::kNumOrganRanks || key < 0 || key >= 128) return nullptr;
    const int i = organ::kOrganPipe[rank][key];
    const OrganPipeTable& t = organPipes();
    return (i >= 0 && (uint32_t)i < t.count) ? &t.specs[i] : nullptr;
}

// The reed pipe at (rank, key), or nullptr.
inline const ReedPipeSpec* organReedSpec(int rank, int key) {
    if (rank < 0 || rank >= organ::kNumOrganRanks || key < 0 || key >= 128) return nullptr;
    const int i = organ::kOrganReedPipe[rank][key];
    const OrganPipeTable& t = organPipes();
    return (i >= 0 && (uint32_t)i < t.reedCount) ? &t.reeds[i] : nullptr;
}

} // namespace pipe
