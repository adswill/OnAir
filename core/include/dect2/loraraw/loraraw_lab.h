// LoRa demodulator for offline use (the Signal Lab): one recording or selection in, frames out, with every parameter adjustable, and an
// automatic search for the parameters. Built on the LoRa physical layer of the mesh receiver (mesh_lora.h), the same Channelizer / Demod
// pair the LoRa (raw) receiver uses. Receive only; nothing is decrypted or interpreted.
#pragma once
#include "dect2/mesh/mesh_lora.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace lora {

struct LabSettings {
    int sf = 7;                   // 5..12
    double bwHz = 125000;         // 7.8 kHz .. 500 kHz (any value works)
    int cr = 5;                   // 5..8 for 4/5..4/8: used with the implicit header only (an explicit header carries it)
    int syncWord = 0x12;          // 0x12 private networks, 0x34 LoRaWAN, 0x2B Meshtastic ...
    bool implicitHdr = false;     // no header: implicitLen, cr and crc must be given
    int implicitLen = 0;
    bool crc = true;              // implicit header only
    int ldro = -1;                // low data rate optimisation: -1 automatic (symbol >= 16 ms), 0 off, 1 on
    bool invertIq = false;        // the signal was sent with inverted IQ (LoRaWAN downlinks): the samples are conjugated
    double offsetHz = 0;          // where the signal sits relative to the centre of the samples
    int preamble = 8;             // up-chirps in the preamble
};

struct LabFrame {
    std::vector<uint8_t> payload;
    bool crcPresent = false, crcOk = false;
    int cr = 0;                   // from the header (explicit) or the setting
    int corrected = 0;
    double startSec = 0;          // frame start in the input
    double snrDb = 0, cfoHz = 0, levelDb = 0;
    int syncWord = 0;
};

struct LabResult {
    std::vector<LabFrame> frames;
    DemodStats stats;             // preambles found, bad sync words, bad headers, frames, CRC failures
};

LabResult labDemodulate(const cf32* x, size_t n, double fs, const LabSettings& s);

struct LabDetectOptions {
    double maxSeconds = 12;       // how much of the input is searched
    bool autoOffset = true;       // measure the signal's offset (and use its width to skip bandwidths that cannot fit)
    double offsetHz = 0;          // used when autoOffset is off
    bool tryInverted = true;      // search again with inverted IQ when nothing was found
    bool lowSf = false;           // also SF5 and SF6
};

struct LabCandidate {
    int sf = 0;
    double bwHz = 0;
    int frames = 0, crcOk = 0, preambles = 0;
};

struct LabDetect {
    bool found = false;
    LabSettings settings;         // sf, bwHz, cr, syncWord, crc, ldro, invertIq, offsetHz of the best candidate
    int frames = 0, crcOk = 0;
    std::vector<LabCandidate> candidates;   // every (SF, bandwidth) that heard a preamble, best first
    std::string note;             // what was found or why not
};

LabDetect labAutoDetect(const cf32* x, size_t n, double fs, const LabDetectOptions& o = {});

} // namespace lora
} // namespace dect2
