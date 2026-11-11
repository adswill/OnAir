// DVB-SH (ETSI EN 302 583 V1.2.1): the common channel coding (CRC-16, EHEADER, EScrambler, the 3GPP2 turbo code with the twelve
// puncturing patterns, the bit-wise interleaver with rate adaptation, the 48-branch convolutional time interleaver) and the OFDM (SH-A)
// physical layer: the DVB-T OFDM with the extra 1K mode, QPSK / 16-QAM (uniform and non-uniform), the symbol interleaver and the TPS
// with the DVB-SH bits (s53 = 1: code rates, time interleaver profile and super-frame number). The TDM (SH-B) path is in dvbsh_tdm.h.
// Reuses the DVB-T reference sequence, constellations and demapper (dect2/dvbt/dvbt.h).
#pragma once
#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2 {
namespace dvbsh {

using cf32 = std::complex<float>;

constexpr int kTcInput = 12282;    // LTC-input, payload EFRAME
constexpr int kSigInput = 1146;    // LTC-input, the TDM signalling field
constexpr int kIuBits = 126;       // interleaving unit
constexpr int kCuBits = 2016;      // capacity unit = 16 IU
constexpr int kShFrameCu = 816;    // OFDM SH frame
constexpr int kShFrameIu = kShFrameCu * kCuBits / kIuBits;   // 13056 = 272 x 48
constexpr int kBranches = 48;
constexpr int kPackets = 8;        // MPEG-TS packets per EFRAME

// ---- puncturing patterns (tables 5.2, 5.3) and the block sizes (tables 5.7, 5.8, 5.11)
constexpr int kNumPatterns = 12;   // Punct_Pat_ID 0..11
const char* rateName(int pat);     // "1/5" ... "2/3"
double codeRate(int pat);          // nominal
int ntcb(int pat, int K = kTcInput);   // coded bits at the turbo encoder output ((K + 6) / CR)
int nbil(int pat);                 // after the bit-wise interleaver and the 126-of-128 puncturing (payload)
int dataCu(int pat);               // DATA part of the OFDM SH frame in CU
int codewordsPerShFrame(int pat);  // NCW
inline int codewordIu(int pat) { return nbil(pat) / kIuBits; }

// ---- CRC-16 (polynomial 0x1021, register 0xFFFF, MSB first) over bits (one per byte)
uint16_t crc16Bits(const uint8_t* bits, size_t n);
uint16_t crc16Bytes(const uint8_t* bytes, size_t n);
// PRBS 1 + X^14 + X^15 loaded with 100101010000000 (the EScrambler and the SH frame padding): n bits
void prbsBits(size_t n, uint8_t* out);

// ---- mode and stream adaptation
// Builds the 12282-bit scrambled EFRAME from eight 188-byte packets (sync bytes 0x47) and the CBCOUNTER_FB value (position in the SH frame)
void buildEframe(const uint8_t* packets188x8, int cbcounterFb, uint8_t* bits12282);
struct EframeInfo {
    bool headerOk = false;
    int cbcounter = 0;       // 24 bits
    int packetsOk = 0;       // packets whose CRC-16 checked
    std::array<bool, kPackets> pktOk{};
};
// De-scrambles a decoded EFRAME (12282 hard bits), checks the CRCs and rebuilds the 188-byte packets (transport_error_indicator set on
// packets whose CRC fails)
EframeInfo parseEframe(const uint8_t* bits12282, uint8_t* packets188x8);
// The same checks without rebuilding (the turbo decoder's stopping rule): true when the header and every packet CRC check
bool eframeChecks(const uint8_t* scrambledBits12282);

// ---- 3GPP2 turbo code (5.3)
// Interleaver output addresses: encoder 2 reads d[addr[k]], k = 0..K-1 (K = 12282 or 1146)
const std::vector<int>& turboInterleaver(int K);
// Encodes K info bits (one per byte) with pattern pat: ntcb(pat, K) coded bits in transmission order (data periods then tail periods)
void turboEncode(const uint8_t* info, int K, int pat, std::vector<uint8_t>& coded);
// Soft decoder (log-MAP, linear correction). llr: ntcb(pat, K) channel LLRs (> 0 means bit 0; 0 = erased) in the encoder's output order.
// Returns the iterations used; stops early when `stop` (called with the hard decisions) returns true.
int turboDecode(const float* llr, int K, int pat, int maxIter, std::vector<uint8_t>& info, const std::function<bool(const uint8_t*)>& stop = nullptr, std::vector<float>* appOut = nullptr);

// ---- bit-wise interleaver and rate adaptation (5.4.2): NTCB coded bits -> NBIL transmitted bits (payload)
void bitInterleave(const uint8_t* coded, int pat, uint8_t* out);
// The receiver side: NBIL LLRs -> NTCB LLRs in encoder order (the punctured 2 of every 128 come back as zeros)
void bitDeinterleave(const float* llr, int pat, float* coded);
// The full codeword: EFRAME bits -> NBIL bits (turbo + bit interleaver + rate adaptation)
void encodeCodeword(const uint8_t* eframe, int pat, std::vector<uint8_t>& out);

// ---- time interleaver (5.4.3, TPS 5.7.4.3.2.13, TS 102 584 7.2.3.3)
struct TiConfig {
    int commonMultiplier = 1;   // 1..63, IU
    int lateTaps = 48;          // 0..48
    int slices = 1;             // 1..63
    int sliceDistance = 0;      // 0..255 SH frames
    int nonLateIncrement = 0;   // 0..63 x common_multiplier
    bool operator==(const TiConfig& o) const { return commonMultiplier == o.commonMultiplier && lateTaps == o.lateTaps && slices == o.slices && sliceDistance == o.sliceDistance && nonLateIncrement == o.nonLateIncrement; }
};
// The 48 branch delays L(0..47) from the receiver's point of view, in interleaver cycles (one cycle = 48 IU). dataCuPerSh: 816 for OFDM.
// false for an invalid parameter set.
bool tiDelays(const TiConfig& c, int dataCuPerSh, std::vector<int>& L);
// Convolutional interleaver on IUs: the transmitter's branch j delays Lmax - L(j) cycles, the receiver's L(j), so every IU goes through
// Lmax cycles in all. T is a bit (uint8_t) on the transmitter and a quantised LLR (int8_t) on the receiver.
template <class T>
class TimeInterleaver {
public:
    void configure(const std::vector<int>& L, bool receiver) {
        lmax_ = 0;
        for (int v : L) lmax_ = std::max(lmax_, v);
        depth_.resize(kBranches);
        fifo_.assign(kBranches, {});
        pos_.assign(kBranches, 0);
        for (int j = 0; j < kBranches; j++) {
            depth_[j] = receiver ? L[j] : lmax_ - L[j];
            fifo_[j].assign((size_t)depth_[j] * kIuBits, T{});
        }
        branch_ = 0;
    }
    int lmax() const { return lmax_; }
    size_t memoryBytes() const { size_t s = 0; for (auto& f : fifo_) s += f.size() * sizeof(T); return s; }
    void setBranch(int b) { branch_ = b % kBranches; }
    int branch() const { return branch_; }
    // One IU in, one IU out (in and out may be the same buffer)
    void process(const T* in, T* out) {
        const int j = branch_;
        branch_ = (branch_ + 1) % kBranches;
        if (depth_[j] == 0) { if (out != in) std::copy(in, in + kIuBits, out); return; }
        T* cell = fifo_[j].data() + (size_t)pos_[j] * kIuBits;
        for (int i = 0; i < kIuBits; i++) { const T t = cell[i]; cell[i] = in[i]; out[i] = t; }
        pos_[j] = (pos_[j] + 1) % depth_[j];
    }

private:
    int lmax_ = 0, branch_ = 0;
    std::vector<int> depth_, pos_;
    std::vector<std::vector<T>> fifo_;
};

// ---- OFDM (5.7)
enum Mode { k2K = 0, k8K = 1, k4K = 2, k1K = 3 };   // the TPS field s38 s39
enum Mod { kQpsk = 0, k16Qam = 1 };
inline int fftN(int mode) { return mode == k8K ? 8192 : mode == k4K ? 4096 : mode == k1K ? 1024 : 2048; }
inline int carriersK(int mode) { return fftN(mode) / 1024 * 852 + 1; }   // 853, 1705, 3409, 6817
inline int dataCarriers(int mode) { return fftN(mode) / 1024 * 756; }
inline int guardSamples(int mode, int guard) { return fftN(mode) >> (5 - guard); }   // guard 0..3 = 1/32, 1/16, 1/8, 1/4 (dvbt::Guard)
const char* modeName(int mode);
const std::vector<int>& continualPilots(int mode);
const std::vector<int>& tpsCarriers(int mode);
const std::vector<int>& symbolPermutation(int mode);   // H(q), q = 0..dataCarriers-1
void carrierRoles(int mode, int symIdx, std::vector<uint8_t>& roles);   // 0 data, 1 scattered, 2 continual, 3 TPS
// The elementary period: 8 MHz 7/64 us ... 1.7 MHz 7/12.8 us. bwMhz 8, 7, 6, 5 or 1.7.
double sampleRate(double bwMhz);

struct Params {
    int mode = k2K;
    int guard = 2;          // 1/8
    int mod = kQpsk;
    int hier = 0;           // 0 non-hierarchical, 1/2/3 = alpha 1/2/4 (16-QAM only)
    int patHp = 4;          // Punct_Pat_ID of the HP (or only) stream
    int patLp = 4;          // the LP stream (hierarchical)
    int cellId = 0;
    TiConfig ti;
    double bwMhz = 8;
};
// Bits that one stream carries per data cell (2: QPSK and each stream of hierarchical 16-QAM; 4: non-hierarchical 16-QAM)
inline int streamBitsPerCell(const Params& p) { return p.mod == k16Qam && p.hier == 0 ? 4 : 2; }
inline int streamBitsPerSymbol(const Params& p) { return dataCarriers(p.mode) * streamBitsPerCell(p); }
// OFDM frames per SH frame (16 .. 1; 8K 16-QAM: one); 0 when a SH frame is shorter than an OFDM frame (never: 8K 16-QAM is exactly one)
inline int framesPerShFrame(const Params& p) { return kShFrameCu * kCuBits / (streamBitsPerSymbol(p) * 68); }
inline int superFramesPerShFrame(const Params& p) { return std::max(1, framesPerShFrame(p) / 4); }
// Useful bit rate (MPEG-TS, HP stream) for the OFDM sample rate fs
double netBitrate(const Params& p, double fs, bool lp = false);

// ---- TPS (5.7.4.3)
// s0..s67 of frame frameIdx (0..3) of a super-frame that is number sfInSh in its SH frame
std::array<uint8_t, 68> tpsBits(const Params& p, int frameIdx, int sfInSh);
struct TpsFrame {
    int frameIdx = 0, mode = 0, guard = 0, mod = 0, hier = 0;
    int length = 0;
    bool sh = false;          // s53
    int s30_33 = 0, s34 = 0, s35 = 0, s48_52 = 0, cellByte = 0;
};
// Validates the sync word and the BCH code (maxFix errors corrected) and splits the fields
bool tpsDecode(const uint8_t bits[68], TpsFrame& f, int maxFix = 2);
// Assembles a super-frame's four TPS frames (indexed by frameIdx) into the parameters: rates, interleaver profile, cell id, super-frame number
bool tpsCombine(const TpsFrame f[4], Params& p, int& sfInSh);

} // namespace dvbsh
} // namespace dect2
