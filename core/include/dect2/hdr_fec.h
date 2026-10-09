// HD Radio (NRSC-5) channel coding shared by the receiver and the test signal generator: the check sums of the layers, the L1 scrambler,
// the tail-biting convolutional codes with their soft Viterbi decoder, the Reed-Solomon code of the audio PDU header and the interleaver
// position tables of the FM (MP1) and AM (MA1) waveforms.
//
// The bit orders, code polynomials, puncture patterns and interleaver equations follow the open-source nrsc5 receiver (GPL-3.0,
// github.com/theori-io/nrsc5: src/decode.c, src/frame.c, src/pids.c, src/defines.h), which decodes real stations, checked against the
// NRSC-5 Layer 1 documents 1011s (FM) and 1012s (AM). Bits are one per byte (0 or 1).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 { namespace hdr {

// ---- frame sizes (nrsc5 defines.h)
constexpr int kFftFm = 2048, kCpFm = 112, kSymFm = kFftFm + kCpFm;     // FM: 744187.5 Hz, 363.4 Hz subcarrier spacing
constexpr int kFftAm = 256, kCpAm = 14, kSymAm = kFftAm + kCpAm;        // AM: 744187.5 / 16 Hz, the same spacing / 2
constexpr int kBlk = 32;                                                // OFDM symbols per L1 block
constexpr double kRateFm = 744187.5;                                    // 1488375 / 2
constexpr double kRateAm = 744187.5 / 16;
constexpr int kP1LenFm = 146176;                                        // P1 bits per L1 frame (MP1, 1.486 s)
constexpr int kP1EncFm = kP1LenFm * 5 / 2;                              // after the rate 2/5 code
constexpr int kPidsLen = 80;
constexpr int kPidsEncFm = kPidsLen * 5 / 2;
constexpr int kP1LenAm = 3750;                                          // P1 bits per AM L1 block (8 per frame)
constexpr int kP3LenMa1 = 24000;                                        // P3 bits per AM L1 frame in MA1
constexpr int kPmBlock = 32 * 720;                                      // soft bits of the primary main sidebands per FM L1 block
constexpr int kAmCols = 25;                                             // subcarriers per AM partition
constexpr int kDiversityAm = 18000 * 3;                                 // MA1: the M bits of the primary lead the B bits by 3 frames

// ---- check sums
uint8_t crc8(const uint8_t* p, size_t n);                               // audio packets: poly 0x31, start 0xFF; packet + its CRC byte gives 0
uint16_t fcs16(const uint8_t* p, size_t n);                             // HDLC frames (the PPP FCS), start 0xFFFF
constexpr uint16_t kFcsGood = 0xF0B8;                                   // fcs16 over a frame with its FCS
void appendFcs(std::vector<uint8_t>& frame);                            // the complemented FCS, low byte first
uint16_t crc12(const uint8_t* bits);                                    // PIDS frame bits 0..67 (in frame order); bits 68..79 carry it
void putCrc12(uint8_t* bits80);

// ---- L1 scrambler: XOR with the sequence of the 11-stage generator (self-inverse; restarts for every transfer frame)
void scramble(uint8_t* bits, size_t n);

// ---- convolutional codes: rate 1/3 mother codes, tail-biting
struct ConvCode { int k; unsigned gen[3]; };
extern const ConvCode kCodeFm;      // K = 7, 133 171 165 (FM P1, PIDS, P3)
extern const ConvCode kCodeE1;      // K = 9, 561 657 711 (AM P1)
extern const ConvCode kCodeE2;      // K = 9, 561 753 711 (AM P3 and PIDS)
// out gets 3 bits per input bit (g0 g1 g2 of each bit in turn); the encoder starts in the state of the last k-1 bits.
void convEncode(const ConvCode& c, const uint8_t* bits, size_t n, uint8_t* out);

// Soft decision decoder of the tail-biting codes: soft has 3 values per bit (positive means 1, 0 for a punctured position).
class Viterbi {
public:
    void decode(const ConvCode& c, const float* soft, size_t n, uint8_t* out);
private:
    std::vector<float> m0_, m1_;
    std::vector<uint64_t> dec_;
};
// Bit errors of the coded stream against the re-encoded decision (hard decisions of the non-zero soft values): the channel BER estimate.
int reencodeErrors(const ConvCode& c, const float* soft, const uint8_t* bits, size_t n, int* counted);

// Puncture patterns over the mother code bits (period 6 or 15)
extern const uint8_t kPunct25[6];   // FM rate 2/5: 1 1 1 1 1 0
extern const uint8_t kPunctE1[15];  // AM P1 rate 5/12
extern const uint8_t kPunctE2[6];   // AM P3 rate 2/3: 1 0 1 1 0 0

// ---- Reed-Solomon (255, 247) over GF(256) (0x11D, first root 1) of the audio PDU header: a shortened 96-byte codeword, bytes 0..7 parity
// (nrsc5 frame.c reverses the bytes into Phil Karn's layout, data first)
void rsEncodeHeader(uint8_t* buf96);                  // fills bytes 0..7 from bytes 8..95
bool rsDecodeHeader(uint8_t* buf96, int* corrected);  // corrects up to 4 bytes; false when it cannot

// ---- interleaver positions (built once, thread safe)
// FM MP1, primary main sidebands: the soft bit matrix of one L1 frame is 16 blocks x 32 rows x 720 columns (20 partitions x 36).
// p1Pos[i] is where the i-th transmitted bit of the punctured P1 code word goes (kP1EncFm of them, nrsc5 interleaver_i);
// pidsPos[i] the position within a block of the i-th of the 200 PIDS bits of that block (interleaver_ii).
const std::vector<int>& fmP1Pos();
const std::vector<int>& fmPidsPos();

// FM extended hybrid (MP2, MP3, MP11): the P3 / P4 channels on the extended partitions go through a convolutional interleaver over 32 blocks
// (nrsc5 interleaver_iv). Every call carries 2 blocks: 2 * frameLen soft bits in, one depunctured code word (3 * frameLen values) out.
constexpr int kP3LenMp2 = 2304, kP3LenMp3 = 4608;
extern const uint8_t kPunctP3[6];   // rate 1/2: 1 0 1 1 0 1
class PxDeinterleaver {
public:
    void reset(int frameLen);
    bool push(const float* in, std::vector<float>& out);   // false until the interleaver has been filled once (32 blocks)
    int frameLen() const { return len_; }
private:
    int len_ = 0, n_ = 0, i_ = 0;
    bool ready_ = false;
    std::vector<float> mem_;
};
// The transmit side: code words in decode order go in, the bits of each call come out (a code word is spread over the 32 blocks before
// the block where the receiver completes it)
class PxInterleaver {
public:
    void reset(int frameLen);
    bool wantsFrame() const;                    // true while another code word is needed before the next call
    void addFrame(const uint8_t* kept);         // the 2 * frameLen kept bits of the next code word
    void call(uint8_t* out);                    // the 2 * frameLen bits of the next two blocks (0 where nothing was placed)
private:
    int len_ = 0, n_ = 0;
    int64_t g_ = 0, t_ = 0;                     // code word bits added, bits sent
    std::vector<uint8_t> ring_;
};

// AM MA1: matrix positions as (block * 32 + row) * 25 + column with the bit number of the word (b, k, p of 1012s section 10.4).
struct AmBit { int elem; int bit; };
struct AmMaps {
    std::vector<AmBit> bl, ml, bu, mu;   // 18000 each: B and M bits of the lower (PL) and upper (PU) primary sidebands
    std::vector<AmBit> el, eu;           // 12000 (T) and 24000 (S): the P3 bits on the tertiary and secondary sidebands
    std::vector<int> pidsRowL, pidsBitL, pidsRowU, pidsBitU;   // 120 each: row and bit of the PIDS words (lower: subcarrier 27, upper: 53)
    std::vector<int> trainRow[kAmCols];  // the two training rows of each column of an interleaver block
};
const AmMaps& amMaps();
// The 12 positions of the combined code word of one group of 3 B and 3 M bits per side (nrsc5 decode.c, 1012s figure 10-4)
extern const int kBlDelay[3], kMlDelay[3], kBuDelay[3], kMuDelay[3];
extern const int kElDelay[2], kEuDelay[4];
extern const int kPidsIlDelay[12], kPidsIuDelay[12];

}} // namespace dect2::hdr
