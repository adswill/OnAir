// "Why isn't it decoding": one verdict for the digital voice modes (DMR, P25, NXDN, D-STAR, TETRA), read from the receivers' own counters.
// Same idea as HfdlTelemetry's why-bad counters: say which stage of the chain stops the signal, so the user knows what to change.
#pragma once
#include <cstdint>

namespace dect2 {

enum class VoiceWhy {
    NoInput,       // no samples / no power at the channel
    NoSync,        // power is there, but no frame sync pattern was found: tuning, polarity, deviation, noise
    IdUnreadable,  // syncs are found, but the frame identity (NID, LICH, slot type) is unreadable: too noisy or distorted
    FecFails,      // identity reads, but the payload fails its FEC / CRC: marginal SNR, fading or a clock error
    Marginal,      // decoding, but over a third of the blocks are lost
    Decoding,      // decoding well
};

struct VoiceWhyIn {
    float levelDb = -200;      // input level, dBFS
    uint64_t syncs = 0;        // frame syncs found
    uint64_t idBad = 0;        // frame identities (NID / LICH / slot type) that failed
    uint64_t fecBad = 0;       // payload blocks lost (FEC or CRC failed)
    uint64_t ok = 0;           // blocks that decoded
};

inline VoiceWhy voiceWhy(const VoiceWhyIn& in) {
    if (in.levelDb < -110.f) return VoiceWhy::NoInput;
    if (in.syncs == 0 && in.ok == 0) return VoiceWhy::NoSync;
    if (in.ok == 0) return in.idBad * 2 > in.syncs ? VoiceWhy::IdUnreadable : VoiceWhy::FecFails;
    if (in.fecBad * 3 > in.ok + in.fecBad) return VoiceWhy::Marginal;
    return VoiceWhy::Decoding;
}

} // namespace dect2
