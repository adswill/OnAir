#include "dect2/dmr/dmr_ambe.h"
#include <cstring>

namespace dect2 {

bool dmrAmbeAvailable(std::string* path) { return nxdnAmbeAvailable(path); }

void dmrVoiceFrames(const dmr::Bits& vs, uint8_t f[3][72]) {
    memset(f, 0, 3 * 72);
    for (size_t i = 0; i < 216 && i < vs.size(); i++) f[i / 72][i % 72] = vs[i] & 1;
}

void DmrAmbe::decodeBurst(const dmr::Bits& vs, int16_t pcm[480]) {
    uint8_t f[3][72];
    dmrVoiceFrames(vs, f);
    for (int k = 0; k < 3; k++) a_.decode(f[k], pcm + 160 * k);
}

void DmrAmbe::decodeBurstSoft(const dmr::Bits& vs, const uint8_t rel[216], int16_t pcm[480]) {
    uint8_t f[3][72];
    dmrVoiceFrames(vs, f);
    for (int k = 0; k < 3; k++) a_.decodeSoft(f[k], rel + 72 * k, pcm + 160 * k);
}

} // namespace dect2
