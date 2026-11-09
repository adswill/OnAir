// DMR voice: a voice burst carries 3 AMBE+2 (3600x2450) vocoder frames of 72 bits = 60 ms of speech (ETSI TS 102 361-1 clause 9.1 and annex B; the
// vocoder is TS 102 361-1 / -2 AMBE+2 half rate). The 216 voice bits of a burst (the two 108 bit halves either side of the sync or embedded
// signalling, joined by dmr::burstVoicePayload) are three consecutive 72 bit frames; each frame is the same 36 dibit AMBE+2 frame as in NXDN and is
// de-interleaved into the four code words C0..C3 of mbelib by the same table (nxdn::ambeFrame). The decoder is a user-installed mbelib
// loaded at run time (docs/CODECS.md): NxdnAmbe is the AMBE+2 3600x2450 decoder.
#pragma once
#include "dmr_fec.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include <cstdint>
#include <string>

namespace dect2 {

bool dmrAmbeAvailable(std::string* path = nullptr);

// The 216 voice bits of a burst (one bit per element) -> 3 frames of 72 bits, in the order they were spoken
void dmrVoiceFrames(const dmr::Bits& vs216, uint8_t frames[3][72]);

// One decoder state per speaker (per time slot)
class DmrAmbe {
public:
    bool ok() const { return a_.ok(); }
    // 480 samples of 8 kHz audio (60 ms) from one voice burst
    void decodeBurst(const dmr::Bits& vs216, int16_t pcm[480]);
    // the same with the reliability (0..255) of each of the 216 bits: soft-decision frame correction and concealment (voice/vocoder_fec.h)
    void decodeBurstSoft(const dmr::Bits& vs216, const uint8_t rel216[216], int16_t pcm[480]);
    void fecCounts(int* frames, int* repaired, int* concealed) const { a_.fecCounts(frames, repaired, concealed); }
private:
    NxdnAmbe a_;
};

} // namespace dect2
