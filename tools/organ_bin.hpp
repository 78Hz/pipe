// Reading and writing the organ's pipe file (data/opus14/organ_pipes.bin, embedded at build time; the
// layout OrganPipes.hpp reads): "P14\0", u32 version 2, u32 flue count, u32 sizeof(FluePipeSpec), the
// flue records, padding to an offset of 8 mod 16, u32 reed count, u32 sizeof(ReedPipeSpec), the reed
// records. A file whose records are shorter than today's (written before a field was added at the end)
// is read with the new fields at their defaults.
#pragma once

#include "engine/FlueRT.hpp"
#include "engine/ReedModel.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pipe {

struct OrganBin {
    std::vector<FluePipeSpec> flues;
    std::vector<ReedPipeSpec> reeds;

    bool read(const std::string& path, std::string& why) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { why = "cannot open " + path; return false; }
        std::vector<unsigned char> d;
        unsigned char buf[65536]; size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
        std::fclose(f);
        if (d.size() < 16 || std::memcmp(d.data(), "P14", 4) != 0) { why = "not an organ pipe file"; return false; }
        uint32_t ver, cnt, sz; std::memcpy(&ver, &d[4], 4); std::memcpy(&cnt, &d[8], 4); std::memcpy(&sz, &d[12], 4);
        if (ver != 2 || sz > sizeof(FluePipeSpec) || 16 + (size_t)cnt * sz > d.size()) { why = "flue records do not match"; return false; }
        flues.assign(cnt, FluePipeSpec());
        for (uint32_t i = 0; i < cnt; ++i) std::memcpy(&flues[i], &d[16 + (size_t)i * sz], sz);
        size_t pos = 16 + (size_t)cnt * sz;
        while (pos % 16 != 8) ++pos;
        if (pos + 8 > d.size()) { why = "no reed records"; return false; }
        uint32_t nr, szr; std::memcpy(&nr, &d[pos], 4); std::memcpy(&szr, &d[pos + 4], 4);
        if (szr > sizeof(ReedPipeSpec) || pos + 8 + (size_t)nr * szr > d.size()) { why = "reed records do not match"; return false; }
        reeds.assign(nr, ReedPipeSpec());
        for (uint32_t i = 0; i < nr; ++i) std::memcpy(&reeds[i], &d[pos + 8 + (size_t)i * szr], szr);
        return true;
    }
    bool write(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const uint32_t ver = 2, cnt = (uint32_t)flues.size(), sz = sizeof(FluePipeSpec);
        std::fwrite("P14", 1, 4, f); std::fwrite(&ver, 4, 1, f); std::fwrite(&cnt, 4, 1, f); std::fwrite(&sz, 4, 1, f);
        if (cnt) std::fwrite(flues.data(), sizeof(FluePipeSpec), cnt, f);
        size_t pos = 16 + (size_t)cnt * sz;
        while (pos % 16 != 8) { std::fputc(0, f); ++pos; }
        const uint32_t nr = (uint32_t)reeds.size(), szr = sizeof(ReedPipeSpec);
        std::fwrite(&nr, 4, 1, f); std::fwrite(&szr, 4, 1, f);
        if (nr) std::fwrite(reeds.data(), sizeof(ReedPipeSpec), nr, f);
        return std::fclose(f) == 0;
    }
};

} // namespace pipe
