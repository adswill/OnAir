// More CubeSat framings (3.9), without any radio, both directions (the test signals are built with the same code they are checked by):
//  - GOMspace U482C: sync word 0xC3AA6655, a Golay (24,12) length field (low 8 bits the length, 0x100 convolutional code, 0x200 CCSDS scrambler,
//    0x400 Reed-Solomon (255,223)), then the packet; the convolutional code is K = 7, r = 1/2 with the polynomials 0x6D, 0x4F (no inversion),
//    terminated with six zero bits. As gr-satellites' u482c_decode (lib/u482c_decode_impl.cc).
//  - CCSDS concatenated: the K = 7, r = 1/2 convolutional code (CCSDS 79 / -109; NASA-DSN -109 / 79; or uninverted) run continuously over the
//    sync word 0x1ACFFC1D and the CCSDS randomised Reed-Solomon code block (frame size + 32 x interleave bytes, dual or conventional basis),
//    optionally NRZ-M precoded. As gr-satellites' ccsds_concatenated_deframer.
//  - USP (Unified SPUTNIX Protocol): a 64 bit sync word, a 64 symbol PLS code (two codes: 48 or 223 data bytes), the CCSDS convolutional code
//    (79 / -109, from state 0, not terminated) over the CCSDS scrambled, dual basis Reed-Solomon block; the block holds two bytes, a little
//    endian length and the AX.25 frame. As gr-satellites' usp_deframer, usp_pls_crop, usp_ax25_crop.
//  - NGHam (Jon Petter Skagmo LA3JPA): sync 0x5DE62A7E, a 24 bit size tag (seven sizes), then a CCSDS scrambled Reed-Solomon code word
//    (GF 0x187, first root 112, step 11, 16 or 32 parity bytes, conventional basis, shortened); the data: a header byte (3 flag bits, 5 bits of
//    padding), the payload, CRC-16 CCITT (X.25) big endian, zero padding. As ngham.c of the NGHam reference code (LGPL-3) and gr-satellites'
//    ngham_deframer.
//  - Mobitex and Mobitex-NX (BEESAT, TECHNOSAT, TUBIN, TUBiX20): sync 0x5765 (NX: 0x0EF0), two control bytes with a (12,8) Hamming FEC byte,
//    a six character callsign with CRC-16 (0x1021, start 0), then 1 .. 32 blocks of 30 bytes: 18 data bytes + CRC-16 X.25 (big endian) in the
//    (12,8) code, a 12 x 20 bit interleaver and the additive scrambler (mask 0x22, seed 0x1FF, 9 bits) running over the blocks of a frame.
//    BEESAT-1 has no callsign; BEESAT-9 always sends 32 blocks. As gr-satellites' mobitex_deframer, mobitex_to_datablocks, mobitex_fec and
//    tubix20_reframer (the output frame layout is that of tubix20_reframer).
// gr-satellites: Daniel Estevez EA4GPZ and contributors, GPL-3.0-or-later; re-implemented in C++ from its description and code, verified
// against its qa test vectors (tests/data/usp, tests/data/mobitex).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

// ---------------------------------------------------------------- convolutional code K = 7, r = 1/2
enum ConvKind { ConvCcsds = 0, ConvNasaDsn, ConvCcsdsUninv, ConvNasaDsnUninv, ConvU482c = ConvNasaDsnUninv };
// bits (0 / 1) -> 2 symbols per bit (0 / 1). state: the encoder's register (newest bit lowest), carried over between calls when given.
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits, int kind, uint32_t* state = nullptr);
// Soft-decision Viterbi over a block of n symbols (soft: > 0 is a 1), starting in state 0; terminated: the path must end in state 0.
bool convViterbi(const float* soft, size_t n, int kind, bool terminated, std::vector<uint8_t>& bits);

// A continuous Viterbi decoder (traceback 96 bits): push symbol pairs, read decided bits from out() as they become final.
class ConvStream {
public:
    explicit ConvStream(int kind = ConvCcsds);
    void setKind(int kind);
    void reset();
    void push(float a, float b);
    std::vector<uint8_t>& out() { return out_; }
private:
    int kind_ = 0;
    uint8_t outTab_[64][2];
    float pm_[64];
    std::vector<uint64_t> dec_;         // one decision word per step (bit s: the predecessor of state s had its top bit set)
    size_t steps_ = 0;
    std::vector<uint8_t> out_;
    void traceback(size_t emit);
};

// ---------------------------------------------------------------- Reed-Solomon with any number of parity bytes (GF 0x187, roots 112 + i, step 11)
void rsParityN(const uint8_t* data, size_t n, int nroots, uint8_t* parity);
int rsDecodeN(uint8_t* block, size_t n, int nroots);      // block = data + parity, n <= 255; errors put right or -1

// ---------------------------------------------------------------- CRCs
uint16_t crc16X25(const uint8_t* d, size_t n);          // CRC-16 CCITT X.25: reflected 0x1021, start 0xFFFF, final XOR 0xFFFF
uint16_t crc16Xmodem(const uint8_t* d, size_t n);       // 0x1021, start 0, no reflection

// ---------------------------------------------------------------- U482C
constexpr uint32_t kSyncU482c = 0xC3AA6655u;
constexpr int kU482cConv = 0x100, kU482cScrambler = 0x200, kU482cRs = 0x400;
// the bytes after the preamble: sync word, Golay field, coded packet (flags: kU482c*; the packet at most 223 bytes with RS, 255 without)
std::vector<uint8_t> u482cEncode(const std::vector<uint8_t>& packet, int flags);
// after the sync word: the 3 byte Golay field and what follows (up to 255 bytes). False: Golay, Viterbi or RS failed.
bool u482cDecode(const uint8_t* afterSync, size_t n, std::vector<uint8_t>& packet, int& rsErr, int& flags);

// ---------------------------------------------------------------- CCSDS concatenated
struct ConcatParams {
    int frameSize = 223;            // data bytes of the frame (parity not included)
    int interleave = 1;
    bool dualBasis = true;
    bool differential = false;      // NRZ-M precoding of the bits before the convolutional code
    bool scrambler = true;
    int conv = ConvCcsds;
    int blockBytes() const { return frameSize + 32 * interleave; }
};
// the code symbols (0 / 1) of the sync word and the code block (a few zero bits of tail at the end, the encoder is not terminated)
std::vector<uint8_t> ccsdsConcatEncode(const std::vector<uint8_t>& frame, const ConcatParams& p);
// block: the blockBytes() bytes after the sync word (Viterbi output, NRZ-M decoded). The frame data; rsErr: byte errors put right (sum).
bool ccsdsConcatDecode(const uint8_t* block, const ConcatParams& p, std::vector<uint8_t>& frame, int& rsErr);

// ---------------------------------------------------------------- USP
extern const uint8_t kUspSync[8];
constexpr int kUspPlsBits = 64, kUspMaxSymbols = 4080;
// the code symbols (0 / 1) after the preamble: sync word, PLS, coded block. The AX.25 frame (FCS not included) at most 219 bytes.
std::vector<uint8_t> uspEncode(const std::vector<uint8_t>& ax25);
// soft: the 64 PLS symbols and then the coded block (at least 64 + 4080 symbols, > 0 = 1). The AX.25 frame; rsErr: byte errors put right.
bool uspDecode(const float* soft, size_t n, std::vector<uint8_t>& ax25, int& rsErr, int* dataLen = nullptr);

// ---------------------------------------------------------------- NGHam
constexpr uint32_t kSyncNgham = 0x5DE62A7Eu;
// sync word, size tag and the scrambled code word. payload at most 220 bytes; flags 3 bits; rs: false sends without parity ("no RS" variant)
std::vector<uint8_t> nghamEncode(const std::vector<uint8_t>& payload, int flags = 0, bool rs = true);
// afterSync: the size tag and the code word (up to 3 + 255 bytes). rsErr -1 when the RS code was not used (the CRC alone decided).
bool nghamDecode(const uint8_t* afterSync, size_t n, std::vector<uint8_t>& payload, int& rsErr, int* flags = nullptr);
int nghamCodewordBytes(int sizeIndex);                  // 47, 79, ..., 255

// ---------------------------------------------------------------- Mobitex
enum MobitexVariant { MobitexDefault = 0, MobitexBeesat1, MobitexBeesat9 };
constexpr uint16_t kSyncMobitex = 0x5765, kSyncMobitexNx = 0x0EF0;
struct MobitexFrame {
    uint8_t control[2] = {0, 0};
    std::string callsign;           // "" for BEESAT-1
    std::vector<std::vector<uint8_t>> blocks;   // 18 data bytes each
    std::vector<bool> blockOk;      // CRC of each block
    int bitErrors = 0;              // bits the FEC codes put right
    // the frame as gr-satellites' tubix20_reframer outputs it: control, callsign, its CRC, the blocks, 0xAA, a 32 bit little endian error map, 0xBB
    std::vector<uint8_t> reframed;
};
// the bytes after the preamble: sync word, header, blocks. blocks: 18 bytes each (zero padded), 1 .. 32 of them; control[0] low 5 bits are set
// to the block count - 1
std::vector<uint8_t> mobitexEncode(const MobitexFrame& f, bool nx, int variant);
// afterSync: header and blocks. False when the control FEC, the callsign CRC (up to callsignBits flipped) or every block CRC fails.
bool mobitexDecode(const uint8_t* afterSync, size_t n, int variant, MobitexFrame& f, int callsignBits = 2);
size_t mobitexMaxBytes(int variant);

// ---------------------------------------------------------------- a stream of soft bits through every sync word hunter above
struct MoreFrame {
    int framing = 0;                // cubesat::Framing (U482c, CcsdsConcat, Usp, Ngham, Mobitex)
    std::string name;               // "U482C", "CCSDS concatenated", "USP", "NGHam", "Mobitex-NX", "BEESAT-1" ...
    std::vector<uint8_t> data;      // the packet (U482C), the frame (CCSDS), the AX.25 frame (USP), the payload (NGHam), the reframed frame (Mobitex)
    int rsErrors = -1;
    std::string how;                // what validated it
    bool inverted = false;
    uint64_t bitPos = 0;            // the bit at the end of the sync word
    std::string callsign;           // Mobitex
    int blocksOk = -1, blocksAll = -1;   // Mobitex: data blocks with a good CRC, data blocks in the frame
};

// Settings that the satellite table can choose (one frame size per hunter; the rest is tried at once)
struct MoreConfig {
    bool u482c = true, ngham = true, mobitex = true, usp = true, concat = true;
    ConcatParams concatParams;      // CCSDS concatenated
    int mobitexVariant = MobitexDefault;
    int syncErrU482c = 4, syncErrNgham = 4, syncErrMobitex = 1, syncErrUsp = 13, syncErrConcat = 4;
};

class MoreHunter {
public:
    MoreHunter();
    ~MoreHunter();
    MoreHunter(const MoreHunter&) = delete;
    MoreHunter& operator=(const MoreHunter&) = delete;
    void configure(const MoreConfig& c);
    const MoreConfig& config() const { return cfg_; }
    void reset();
    void push(float soft);          // one bit (BPSK or FSK symbol after the slicer's sampler), > 0 = 1
    std::function<void(const MoreFrame&)> onFrame;
    uint64_t syncs = 0, failed = 0, frames = 0;
    bool busy() const;              // a frame is being collected
private:
    struct Impl;
    Impl* p_;
    MoreConfig cfg_;
};

} // namespace cubesat
} // namespace dect2
