// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 3 bit layer: the burst formats of ICAO Annex 10 Vol III, Part I, 6.4.4 (2nd edition, amendment 81 and later), and the
// shared D8PSK modulation of 6.4.2 (10 500 symbols/s, Gray-coded phase steps, raised cosine alpha 0.6). Read from the Annex text:
//   Training: transmitter ramp-up, then 16 synchronisation symbols (3 bits each, D8PSK phase steps like the Mode 2 unique word):
//     S1  M burst downlink (standard),  S1* net entry request,  S2 voice / data (V/D) burst,  S2* M uplink, H uplink and poll responses.
//     S1* and S2* have every phase change 180 degrees from S1 and S2 (a correlator tells them apart by the sign).
//   M uplink (S2*): 32 symbols = 4 Golay (24,12) words (non-3T), or 128 symbols = 16 words (3T configuration).
//   H uplink, handoff check (S2*, 3T): 40 symbols = 5 Golay words.
//   M downlink (S1, S1*, poll response S2*): 16 symbols = 2 Golay words.
//   V/D (S2): header 8 symbols = 1 Golay word, then 192 symbols of user information: data as one Reed-Solomon (72,62) code word over
//     GF(2^8) (field x^8 + x^7 + x^2 + x + 1, generator roots alpha^120 .. alpha^129, input padded with zeros to 62 octets); voice is
//     the output of the Mode 3 vocoder (6.8) with its own FEC.
//   Golay (24,12): y = x G with the G of 6.4.4.1.2 (12 parity columns, then the identity), sent from left to right.
//   Scrambling: the Mode 2 scrambler (x^15 + x + 1, the 6.4.3.1.4 start value), restarted after the training of every burst. No interleaving.
// Not in the Annex (they are in ICAO Doc 9805, the VDL Mode 3 technical manual, which is not public): what the system data, header and
// handoff bits mean, the slot timing, and the bit order of the Reed-Solomon octets (this code sends each octet least significant bit
// first, as Mode 2 does, and the 62 data octets before the 10 check octets). So the decoder shows those fields as raw values.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

constexpr double kVdl3SymbolRate = 10500.0;
constexpr int kVdl3SyncSyms = 16;
constexpr int kVdl3RsN = 72, kVdl3RsK = 62;

enum class Vdl3Sync : int { S1 = 0, S1Star = 1, S2 = 2, S2Star = 3 };
const char* vdl3SyncName(Vdl3Sync s);                 // "S1", "S1*", "S2", "S2*"
// The 48 bits (16 symbols x 3, first sent first) of a synchronisation sequence
const std::vector<uint8_t>& vdl3SyncBits(Vdl3Sync s);
// The absolute phases of the 16 training symbols in units of pi/4, from a ramp at phase 0 (the first symbol is always 0)
const std::vector<int>& vdl3SyncPhases(Vdl3Sync s);

// D8PSK Gray code (Annex 10 Table 6-1, as Mode 2): phase step in units of pi/4 -> its three bits (first sent = most significant)
extern const uint8_t kVdl3GrayBits[8];
int vdl3StepForBits(int bits3);

enum class Vdl3Kind : int {
    Unknown = 0,
    MUplink = 1,      // S2*, 32 symbols
    MUplink3T = 2,    // S2*, 128 symbols
    Handoff = 3,      // S2*, 40 symbols (3T handoff check message)
    PollResponse = 4, // S2*, 16 symbols (downlink)
    MDownlink = 5,    // S1, 16 symbols
    NetEntry = 6,     // S1*, 16 symbols
    VdData = 7,       // S2, the user information is a good Reed-Solomon word
    VdVoice = 8,      // S2, the user information is not a Reed-Solomon word: vocoder frames (or data too damaged to correct)
};
const char* vdl3KindName(Vdl3Kind k);
int vdl3PayloadSymbols(Vdl3Kind k);                   // symbols after the training sequence
int vdl3GolayWords(Vdl3Kind k);                       // Golay words after the training (V/D: the header only)

// ---- codes ----
uint32_t vdl3GolayEncode(uint32_t data12);            // 24-bit word, bit 23 = first sent: 12 parity bits, then the data (bit 11 first)
int vdl3GolayDecode(uint32_t word24, uint32_t& data12);   // bits corrected (0 .. 3), -1 when more than three are wrong
void vdl3Scramble(std::vector<uint8_t>& bits, size_t from = 0);   // XOR with the Mode 2 scrambler sequence, restarted at `from`
// Reed-Solomon (72,62): data up to 62 octets (zero padded), returns the 72-octet code word
std::vector<uint8_t> vdl3RsEncode(const std::vector<uint8_t>& data);
int vdl3RsDecode(std::vector<uint8_t>& cw72);         // in place; octets corrected (0 .. 5), -1 when it cannot be corrected

// ---- bursts ----
// The bits after the training sequence (not yet scrambled): Golay words of `info12` (as many as the kind has), or for V/D the header word
// then the 576 user information bits (data: the RS code word of `data`; voice: `voiceBits`, 576 of them)
std::vector<uint8_t> vdl3PayloadBits(Vdl3Kind k, const std::vector<uint32_t>& info12, const std::vector<uint8_t>& data, const std::vector<uint8_t>& voiceBits);
// A whole burst as bits: training sequence, then the scrambled payload
std::vector<uint8_t> vdl3BurstBits(Vdl3Kind k, const std::vector<uint32_t>& info12, const std::vector<uint8_t>& data = {}, const std::vector<uint8_t>& voiceBits = {});
Vdl3Sync vdl3SyncFor(Vdl3Kind k);

struct Vdl3Decoded {
    Vdl3Kind kind = Vdl3Kind::Unknown;
    std::vector<uint32_t> info12;   // the Golay words' 12 information bits each (V/D: the header)
    int golayFixed = 0;             // bits corrected over all Golay words
    bool golayOk = false;           // every Golay word decoded
    bool rsOk = false;              // V/D: the user information is a good RS (72,62) word
    int rsFixed = 0;
    std::vector<uint8_t> data;      // V/D data: the 62 information octets
    std::vector<uint8_t> raw;       // V/D voice: the 72 user information octets as received (descrambled)
};
// Payload bits (descrambled, after the training) of a burst whose sync is `sync` and which has `symbols` symbols after the training
bool vdl3DecodePayload(Vdl3Sync sync, int symbols, const std::vector<uint8_t>& bits, Vdl3Decoded& out);
Vdl3Kind vdl3KindFromSync(Vdl3Sync s, int symbols);   // Unknown when that sync never has that length

std::string vdl3Hex(const std::vector<uint8_t>& b, size_t max = 80);
std::string vdl3InfoHex(const std::vector<uint32_t>& info12);   // "A5C 012 ..." (3 hex digits per word)
std::string vdl3PrintableText(const std::vector<uint8_t>& b);   // the printable ASCII run, "" when the octets do not look like text

} // namespace dect2
