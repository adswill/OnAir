// Direct-broadcast and dump downlinks of polar weather and earth-observation satellites (docs/SAT-XBAND-3.8.md): JPSS HRD, EOS DB, MetOp X-band dump, MetOp-SG DDB,
// Landsat, Proba, GCOM, STEREO, Oceansat, Saral, Kanopus-V, DMSP RTD. One receiver class, one profile per mode-list entry, shared CADU handling and instrument decoders.
//
// Most of these need a tracking dish, a down-converter and a very fast radio (tens to hundreds of Msps), so a recording played back through the receiver is the main path.
//
// Link parameters and frame layouts follow SatDump's pipelines and plugins (https://github.com/SatDump/SatDump, GPL-3.0, Copyright Aang23 and contributors); see THIRD_PARTY.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

enum XbProfile {
    kXbNpp = 0,        // Suomi NPP / NOAA-20 HRD
    kXbJpss2,          // NOAA-21 HRD (JPSS-2 onwards)
    kXbAquaAura,       // Aqua and Aura DB
    kXbTerra,          // Terra DB
    kXbMetopDump,      // MetOp X-band dump
    kXbMetopSg,        // MetOp-SG DDB
    kXbLandsatX,       // Landsat 8/9 X-band
    kXbLandsatS,       // Landsat 8/9 S-band
    kXbProbaS,         // Proba-1 / 2 / V S-band dump
    kXbProbaVX,        // Proba-V X-band dump
    kXbGcomW1,         // GCOM-W1
    kXbGcomC1,         // GCOM-C1
    kXbStereo,         // STEREO A/B high rate
    kXbOceansat2,      // Oceansat-2 DB
    kXbOceansat3,      // Oceansat-3 L-band
    kXbSaralL,         // Saral L-band
    kXbOceansat3X,     // Oceansat-3 X-band dump
    kXbSaralX,         // Saral X-band dump
    kXbKanopus,        // Kanopus-V
    kXbDmsp,           // DMSP RTD
    kXbNumProfiles
};

constexpr int kXbFirstFamily = 530;     // numbers: docs/MODE-NUMBERS.md (family, active standard = family + 1, engine standard = family + 2)
constexpr int kXbFirstPollSlot = 530;

enum XbMod { kXbBpsk, kXbQpsk, kXbOqpsk };
enum XbFec { kXbFecNone, kXbFecConv, kXbFecLdpc, kXbFecTurbo };

struct XbLink {
    const char* id;
    const char* name;
    int family;
    double defMhz;          // 0: no published frequency
    double sampleRate;      // what the app asks the radio for
    double minRate;         // below this the receiver says so
    double symbolRate;
    double alpha;           // root raised cosine roll-off
    XbMod mod;
    XbFec fec;
    int caduBytes;          // with the 4-byte sync word; 0: not a CADU link (the profile has its own deframer)
    int rsI;                // interleave; 0: no Reed-Solomon
    int rsE;                // 16: (255,223), 8: (255,239)
    int rsFill;             // shortening (virtual fill bytes)
    bool nrzm;              // bit-level NRZ-M after the convolutional decoder
    bool railDiff;          // NRZ-M per rail on the symbols (Aqua)
    bool derand;
    int derandFrom;         // first randomised byte of the frame
    bool derandAfterRs;
    bool invertBytes;       // the bytes of the frame are inverted (MetOp dump)
    bool swapPairs;         // the two coded bits are swapped in a symbol pair (bpsk_90 of SatDump)
    int mpduData;           // data bytes of an M_PDU zone; 0: no space packets
    int insertZone;         // bytes of the VCDU insert zone
    bool tmFrames;          // CCSDS TM transfer frames, not AOS (Proba)
    const char* instruments;
    const char* needs;      // the gear it needs, for the mode description
};
const XbLink& xbLink(int profile);

// One channel of the current pass, reduced for the screen. Immutable once published. 8 bit, top to bottom.
struct XbPreview {
    uint64_t id = 0;
    std::string name;       // "VIIRS M5", "MODIS 31" ...
    double um = 0;          // centre wavelength in micrometres, 0 unknown
    int width = 0, height = 0, fullWidth = 0, fullHeight = 0;
    std::vector<uint8_t> px;
};

struct XbTelemetry {
    uint64_t seq = 0;
    int profile = 0;
    int state = 0;                      // 0 searching, 1 symbols, 2 frame sync, 3 frames good
    double cfoHz = 0;
    float snrDb = 0, levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    double symbolRate = 0, inputRate = 0, timeSec = 0;
    float carrierLock = 0;
    float acqPeak = 0;                  // the last search block's carrier line (peak to mean of the power-law spectrum; noise stays under 4.5)
    uint64_t idleSkipped = 0;           // input samples the pre-check skipped while there was no line
    bool frameLock = false, rateTooLow = false;
    uint64_t asmHits = 0, rsCorrected = 0, packets = 0, packetsBad = 0, lines = 0;
    int scid = -1, vcid = -1;
    std::string satName, hint, phase;   // phase: which ambiguity / pairing locked
    uint64_t vcidFrames[64] = {};
    std::vector<std::pair<float, float>> constellation;
    std::vector<std::shared_ptr<const XbPreview>> channels;
    uint64_t imageSeq = 0;
    uint64_t caduWritten = 0;
};

inline std::string xbandSummary(const XbTelemetry& t) {
    char b[200];
    const XbLink& p = xbLink(t.profile);
    if (t.state >= 3) snprintf(b, sizeof b, "%s, %s%s%llu frames, %llu lines", p.name, t.satName.c_str(), t.satName.empty() ? "" : ", ", (unsigned long long)t.blocksOk, (unsigned long long)t.lines);
    else if (t.state == 2) snprintf(b, sizeof b, "%s, frame sync, frames failing", p.name);
    else if (t.state == 1) snprintf(b, sizeof b, "%s, symbols, no frame sync", p.name);
    else snprintf(b, sizeof b, "%s, searching", p.name);
    return b;
}

} // namespace dect2
