// LR-FHSS (Long Range - Frequency Hopping Spread Spectrum, the LoRaWAN data rates EU868 DR8 - DR11, US915 DR5 - DR6): the coding chain
// (whitening, CRC-16, convolutional code of rate 1/3 punctured to 1/2, 2/3, 5/6, interleaver, header with CRC-8, tail-biting rate 1/2 code and
// header interleaver, sync word), the hopping sequence and grid, the time on air, and the soft decoders the receiver uses.
// Ported from Semtech's sx126x_driver v2.5.0 (src/lr_fhss_mac.c, src/sx126x_lr_fhss.c; The Clear BSD License, Copyright Semtech Corporation
// 2021; the same code is in RadioLib 7.8.1 src/modules/SX126x/SX126x_LR_FHSS.cpp, MIT for RadioLib, Clear BSD for this part): the header
// interleaver, the hop-sequence polynomials and the channel counts are copied, the algorithms re-written; the convolutional codes are written
// from their generators (derived from Semtech's tables: rate 1/3 K = 7 155, 117, 127 octal; rate 1/2 K = 5 35, 23 octal). The decoders are new.
// What the transmitter does with the bits (GMSK at 488.28125 bit/s, BT 1, which bit is the upper tone) is not in the drivers: see lrfhss_gen.h.
// Synthetic tests only; not checked against a real transmitter.
//
// Frame: header_count copies of a 114-bit header block (2 guard bits, 40 coded header bits, the 32-bit sync word, the other 40), each on a hop of
// its own, then the payload in blocks of 2 guard bits and 48 coded bits, one hop each. Copy i of H carries the sync index H - 1 - i (0 = the last
// header): the payload starts that many header blocks after the end of the copy a receiver found.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace lrfhss {

constexpr double kBitRate = 488.28125;      // bit/s (32 MHz / 2^16)
constexpr double kStepHz = 488.28125;       // the physical channel spacing (one grid step of 512 PLL steps of 0.95367431640625 Hz)
constexpr int kHeaderBits = 114;            // one header block on the air
constexpr int kHalfHeaderBits = 40;
constexpr int kSyncBits = 32;
constexpr int kFragBits = 48;               // coded payload bits per hop
constexpr int kBlockBits = 50;              // with the 2 guard bits
constexpr uint32_t kLorawanSync = 0x2C0F7995;   // LoRaWAN's sync word (RadioLib RADIOLIB_LORAWAN_LR_FHSS_SYNC_WORD)

enum Cr { Cr5_6 = 0, Cr2_3 = 1, Cr1_2 = 2, Cr1_3 = 3 };
enum Grid { Grid25391 = 0, Grid3906 = 1 };     // 52 or 8 physical channels per grid cell

struct Params {
    int cr = Cr1_3;
    int grid = Grid3906;
    int bw = 2;                  // 0..9: 39.063, 85.938, 136.719, 183.594, 335.938, 386.719, 722.656, 773.438, 1523.438, 1574.219 kHz
    bool hopping = true;
    int headers = 3;             // header copies (LoRaWAN: 3 at CR 1/3, 2 at CR 2/3)
    uint32_t syncWord = kLorawanSync;
};

const char* crName(int cr);                    // "1/3"
double occupiedHz(int bw);                     // the occupied channel width (OCW) of a bandwidth index
double gridHz(int grid);                       // 25390.625 or 3906.25
int channelCount(int bw);                      // physical channels in the OCW (80 .. 3224)
int hopSequenceCount(const Params& p);         // 384 or 512 hop sequences

// ---- hopping ----
struct HopParams {
    bool ok = false;
    int nGrid = 0;               // grid cells in the OCW
    uint16_t polynomial = 0, xoringSeed = 0, id = 0, initState = 0;
};
HopParams hopParams(const Params& p, int hopSeqId);
// Hop k (headers first) of a frame with hopping: the grid position in -nGrid/2 .. nGrid/2 - 1. The LFSR is advanced 4 - headers times before
// hop 0, so a header copy with sync index s always sits on advance 4 - s and payload hop j on advance 5 + j, whatever the header count.
std::vector<int> gridSequence(const Params& p, int hopSeqId, int hops);
int gridPositionHeader(const Params& p, int hopSeqId, int syncIndex);
int gridPositionPayload(const Params& p, int hopSeqId, int payloadHop);
// Carrier of a hop relative to the OCW centre as the SX126x sets it: -position * grid - (deviceOffset + gridOffset) * 488.28125 Hz, and
// + 244.140625 Hz on the header copies with an odd sync index. deviceOffset: -4..3 (narrow grid) or -26..25 (wide grid).
double hopOffsetHz(const Params& p, int nGrid, int position, int deviceOffset, int syncIndexOrMinus1);

// ---- sizes and time on air ----
struct Digest {
    int bits = 0;                // physical bits (all header copies and the payload blocks with their guard bits)
    int bytes = 0;
    int hops = 0;
    int codedBits = 0;           // coded payload bits (after puncturing)
};
Digest digest(const Params& p, size_t payloadLen);
// Time on air: bits / 488.28125 (the SX126x adds one symbol on the first hop and the hop switching; not counted, as in Semtech's driver)
double airSeconds(const Params& p, size_t payloadLen);
uint32_t airMs(const Params& p, size_t payloadLen);      // lr_fhss_get_time_on_air_in_ms: rounded up

// ---- coding ----
uint8_t headerCrc8(const uint8_t* p, size_t n);          // polynomial 0x2F, start 0xFF
uint16_t payloadCrc16(const uint8_t* p, size_t n);       // polynomial 0x755B, start 0xFFFF, over the whitened bytes
void whiten(const uint8_t* in, size_t n, uint8_t* out);  // the LFSR (start 0xFF) and a nibble swap
void dewhiten(const uint8_t* in, size_t n, uint8_t* out);
// The 5 header bytes before coding: length, modulation / CR / grid / hopping / BW, hop sequence, sync index, CRC-8
std::array<uint8_t, 5> rawHeader(const Params& p, int hopSeqId, size_t payloadLen, int syncIndex);
// The whole physical frame as Semtech's lr_fhss_build_frame writes it (bits first in the most significant bit of each byte)
std::vector<uint8_t> buildFrame(const Params& p, int hopSeqId, const uint8_t* payload, size_t n);
// The same as bits (0 / 1), digest(p, n).bits of them
std::vector<uint8_t> frameBits(const Params& p, int hopSeqId, const uint8_t* payload, size_t n);
// Order of the payload interleaver: coded bit order[j] is sent j-th (guard bits not counted)
std::vector<uint16_t> interleaverOrder(int codedBits);
extern const uint8_t kHeaderInterleaver[80];

// ---- decoding (soft values: > 0 means bit 1, the magnitude its reliability) ----
struct Header {
    bool ok = false;             // CRC-8 good and the fields valid
    int len = 0, modulation = 0, cr = 0, grid = 0, bw = 0, hopSeqId = 0, syncIndex = 0;
    bool hopping = true;
    std::array<uint8_t, 5> raw{};
};
Header parseHeader(const std::array<uint8_t, 5>& raw);
// The 80 coded header bits in their order on the air (the two halves around the sync word, guard bits left out): tail-biting Viterbi (every
// start state tried), CRC-8 and the field checks
Header decodeHeader(const float* soft80);
// The coded header bits on the air for a header (80 bits, the order decodeHeader takes)
std::vector<uint8_t> headerAirBits(const std::array<uint8_t, 5>& raw);
// The coded payload bits as received (digest(p, len).codedBits of them, guard bits removed, interleaved order): de-interleave, de-puncture,
// Viterbi (rate 1/3, K = 7, terminated), CRC-16, de-whiten. False when the CRC fails (payload holds the bytes anyway).
bool decodePayload(int cr, size_t len, const float* soft, std::vector<uint8_t>& payload, int* bitErrors = nullptr);

} // namespace lrfhss
} // namespace dect2
