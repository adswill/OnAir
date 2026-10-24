// AMBE+2 (NXDN voice) through the user's own mbelib, loaded at run time: AMBE is patented and OnAir does not ship it (docs/CODECS.md).
// Without the library the NXDN receiver still shows the calls, ids, talkgroups, messages and site information, only without sound.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

bool nxdnAmbeAvailable(std::string* path = nullptr);   // mbelib was found and has the functions needed
bool nxdnAmbeRescan();                                 // look again (the "Look again" button); true when found

// One decoder state: 72 voice bits of a VCH in (AMBE+2 3600x2450), 160 samples of 8 kHz audio out (20 ms)
class NxdnAmbe {
public:
    NxdnAmbe();
    ~NxdnAmbe();
    bool ok() const;
    void decode(const uint8_t bits[72], int16_t pcm[160]);
    void decodeFr(const char fr[4][24], int16_t pcm[160]);   // the four code words already de-interleaved (P25 Phase 2 has its own bit order)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
