// Every pipe of the organ rebuilt unedited for the Workshop (PipeEdit.hpp): it must reproduce the pipe as
// voiced. Prints the pipes that do not, and the worst differences.
#include "engine/PipeEdit.hpp"
#include <cstdio>
using namespace pipe;
int main() {
    int nf = 0, nr = 0, badF = 0, badR = 0; double worstF = 0, worstT = 0, worstR = 0;
    for (int r = 0; r < organ::kNumOrganRanks; ++r)
        for (int k = 0; k < 128; ++k) {
            std::string why;
            if (const FluePipeSpec* b = organFlueSpec(r, k)) {
                ++nf; FluePipeSpec s; PipeEdit e;
                if (!edit::editFlue(r, k, *b, e, s, why)) { ++badF; std::printf("FLUE %-22s key %3d: %s\n", organ::kOrganRanks[r].name, k, why.c_str()); continue; }
                double w = 0; const int n = std::min(s.nModes, b->nModes);
                for (int i = 0; i < std::min(n, 4); ++i) w = std::max(w, std::fabs(1200.0 * std::log2((double)s.f[i] / (double)b->f[i])));
                worstF = std::max(worstF, w); worstT = std::max(worstT, std::fabs((double)s.transit / b->transit - 1.0));
                if (s.nModes != b->nModes) std::printf("FLUE %-22s key %3d: %d modes, voiced %d\n", organ::kOrganRanks[r].name, k, s.nModes, b->nModes);
            } else if (const ReedPipeSpec* b = organReedSpec(r, k)) {
                ++nr; ReedPipeSpec s; PipeEdit e;
                if (!edit::editReed(r, k, *b, e, s, why)) { ++badR; std::printf("REED %-22s key %3d: %s\n", organ::kOrganRanks[r].name, k, why.c_str()); continue; }
                double w = 0; const int n = std::min(s.nModes, b->nModes);
                for (int i = 0; i < std::min(n, 4); ++i) w = std::max(w, std::fabs(1200.0 * std::log2((double)s.f[i] / (double)b->f[i])));
                worstR = std::max(worstR, w);
            }
        }
    std::printf("flue: %d pipes, %d not rebuilt; worst first-4-mode difference %.3f c, jet transit %.3f %%\n", nf, badF, worstF, 100 * worstT);
    std::printf("reed: %d pipes, %d not rebuilt; worst first-4-mode difference %.3f c\n", nr, badR, worstR);
    return (badF || badR) ? 1 : 0;
}
