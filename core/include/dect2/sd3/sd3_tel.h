// SatDump parity, tier 3 (docs/SATDUMP-TIER3-3.8.md): GOES GRB, GVAR, CDA and MDL, GK-2A UHRIT and CDAS, one generic S-band CCSDS mode with a satellite table, and the UHF cubesats
// Geoscan, Lucky-7, Spino and Erminaz. One receiver class, one profile per mode-list entry (the S-band entry and the links within a profile are a satellite table).
//
// Formats and parameters from SatDump (https://github.com/SatDump/SatDump, GPL-3.0, Copyright Aang23 and contributors); see THIRD_PARTY.md.
#pragma once
#include "dect2/wxsat/wxsat_tel.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

enum Sd3Profile { kSd3Grb = 0, kSd3Gvar, kSd3Cda, kSd3Mdl, kSd3Uhrit, kSd3Cdas, kSd3Sband, kSd3Geoscan, kSd3Lucky7, kSd3Spino, kSd3Erminaz, kSd3NumProfiles };

enum Sd3Mod { kSd3Bpsk = 0, kSd3Qpsk = 1, kSd3Oqpsk = 2, kSd3Pm = 3, kSd3Fsk = 4 };

// Every link one line of a table. A link with `ccsds` false in spirit has its own sync word and no Reed-Solomon: all of it is in the numbers.
struct Sd3Link {
    const char* id;
    const char* name;
    double mhz;               // 0: SatDump lists no frequency
    double sym;
    double rrc;
    int mod;                  // Sd3Mod
    bool conv;                // K = 7 r = 1/2 before the sync word
    int frameBits;            // with the sync word
    bool nrzm, derand, derandAfterRs, dual;
    int rsI, rsType, fill;    // interleave (0 none), 223 / 239, fill bytes
    uint32_t sync;
    bool iqSwap;
};

struct Sd3ProfileInfo {
    const char* id;
    const char* name;
    int family;
    double sampleRate;       // what the app asks the radio for
    double minRate;
};
const Sd3ProfileInfo& sd3Profile(int p);
inline constexpr int kSd3Families[kSd3NumProfiles] = {290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 510};
inline constexpr int kSd3PollSlots[kSd3NumProfiles] = {290, 291, 292, 293, 294, 295, 296, 297, 298, 299, 510};
// the profile of an active standard (family + 1), or -1
inline int sd3ProfileOfActive(int active) { for (int i = 0; i < kSd3NumProfiles; i++) if (kSd3Families[i] + 1 == active) return i; return -1; }

int sd3LinkCount(int profile);
const Sd3Link& sd3LinkAt(int profile, int index);

// One frame of the log the panel shows
struct Sd3Frame {
    double timeSec = 0;
    int scid = -1, vcid = -1;
    int bytes = 0;
    bool rsOk = true;
    std::string hex;         // the first bytes
};

struct Sd3Product { int key = 0; std::string name; uint64_t updated = 0; };

struct Sd3Telemetry {
    uint64_t seq = 0;
    int profile = 0, link = 0;
    int state = 0;                      // 0 searching, 1 carrier and symbols, 2 frame sync, 3 frames good
    double cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double symbolRate = 0, inputRate = 0, timeSec = 0;
    float carrierLock = 0;
    bool frameLock = false;
    uint64_t asmHits = 0, rsCorrected = 0, packets = 0, files = 0, lines = 0, crcBad = 0;
    int scid = -1, vcid = -1;
    std::string satName, hint, lastFile, lastText;
    uint64_t vcidFrames[64] = {};
    std::vector<std::pair<int, uint64_t>> apids;     // (APID, packets), biggest first, at most 32
    std::vector<std::pair<float, float>> constellation;
    std::shared_ptr<const WxImage> image;            // main picture
    std::shared_ptr<const WxImage> image2;           // GVAR: the infrared channels; the visible one is `image`
    std::vector<Sd3Product> products;                // GRB: the pictures being assembled
    int selectedProduct = -1;
    uint64_t imageSeq = 0;
    std::deque<Sd3Frame> frames;                     // newest last, at most 64
};

inline std::string sd3Summary(const Sd3Telemetry& t) {
    char b[200];
    const Sd3ProfileInfo& p = sd3Profile(t.profile);
    const Sd3Link& l = sd3LinkAt(t.profile, t.link);
    const char* name = t.profile == kSd3Sband || sd3LinkCount(t.profile) > 1 ? l.name : p.name;
    if (t.state >= 3) snprintf(b, sizeof b, "%s, %llu frames, %llu packets, %llu files", name, (unsigned long long)t.blocksOk, (unsigned long long)t.packets, (unsigned long long)t.files);
    else if (t.state == 2) snprintf(b, sizeof b, "%s, frame sync, frames failing", name);
    else if (t.state == 1) snprintf(b, sizeof b, "%s, symbols, no frame sync", name);
    else snprintf(b, sizeof b, "%s, searching", name);
    return b;
}

} // namespace dect2
