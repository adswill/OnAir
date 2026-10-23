// Mercury (the HERMES HF modem of Rhizomatica, github.com/Rhizomatica/mercury, GPL-3.0): the physical layer of its OFDM data modes,
// as OnAir receives them. Written for OnAir from Mercury's source (branch mercuryv2, commit c72997be9d3c, read 2026-10-23): the mode
// parameters, the frame geometry, the pilots, preamble and postamble, the unique word, the interleaver and the LDPC codes follow
// Mercury's modem/freedv (a fork of codec2's OFDM data modes, David Rowe et al., LGPL-2.1) bit for bit; the receiver (acquisition,
// channel estimation, the LDPC decoder) is OnAir's own.
//
// The waveform. Every mode is OFDM on nc carriers, rs = 1 / ts apart, centred on 1500 Hz of the radio's upper sideband audio (8 kHz):
// carrier k (0 .. nc + 1, the two edge carriers stay empty) sits at (nlower + k) * rs, nlower = round(1500 / rs - nc / 2) - 1. A symbol
// is M = 8000 * ts samples with a cyclic prefix of Ncp = 8000 * tcp samples. A modem frame is ns symbols: a row of BPSK pilots (the
// fixed list kPilots), then ns - 1 rows of data; a packet (one LDPC codeword, one Mercury frame) is np modem frames. A burst on the air
// is the preamble (one modem frame of known symbols, ofdm_rand seed 2), the packet, and the postamble (seed 3), whose first row of
// pilots also closes the packet's last frame. The packet carries the unique word (nuwbits known bits at fixed symbol positions) and
// the codeword: the information bits that are used, then the parity bits, Gray QPSK (QAM16C2: 16-QAM), interleaved by symbols
// (golden prime). The information bits end with a CRC-16 (CCITT-FALSE over the bytes before it); inside a connected session the CRC is
// XORed with the session's seed (see mercury_proto.h). Codes: the IRA codes of codec2 (accumulator parity), shortened where a mode
// uses fewer information bits (the unused ones are known to be 1).
//
// Not here: the MFSK floor of the ARQ ladder (Mercury's own non-coherent 32-FSK), see mercury_mfsk.h.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace mercury {

using cf = std::complex<float>;

// The OFDM data modes, in the order of this list (not Mercury's -l numbers: see OfdmMode::mercuryIndex)
enum OfdmModeId : int {
    kDatac0 = 0, kDatac1, kDatac3, kDatac4, kDatac13, kDatac14, kDatac15, kDatac16, kDatac17, kQam16c2,
    kNumOfdmModes,
    // Mercury's own non-coherent MFSK (mercury_mfsk.h): the floor of the ARQ ladder (1 kHz) and of a BW500 session (500 Hz)
    kMfsk = kNumOfdmModes, kMfsk500,
    kNumModes
};
const char* modeName(int id);          // every mode: "DATAC16", "MFSK", "MFSK500"
int modePayloadBytes(int id);          // payload bytes of a frame of the mode (without the CRC)

// The LDPC codes (all IRA: parity check i covers some information bits and parity bits i - 1 and i)
enum LdpcCodeId : int { kH128x256 = 0, kH256x512, kH256x768, kH1024x2048, kH4096x8192, kH16200x9720, kHra56x56, kMfsk1600, kNumLdpcCodes };

struct OfdmMode {
    const char* name;        // "DATAC16"
    int mercuryIndex;        // `mercury -l` / -m index (0 DATAC1, 1 DATAC3, 2 DATAC0, 3 DATAC4, 4 DATAC13, 5 DATAC14, 7 DATAC15, 8 DATAC16, 9 DATAC17, 10 QAM16C2)
    int nc, np, ns, bps;     // carriers, modem frames per packet, symbols per modem frame, bits per symbol
    float ts, tcp;           // symbol and cyclic prefix duration, s
    int nuwbits;             // unique word bits
    int code;                // LdpcCodeId
    int ladderRung;          // the carousel ARQ rung that rides this mode (-1 none; DATAC16 is the control mode)
    const char* use;         // what Mercury uses it for
};
const OfdmMode& ofdmMode(int id);
int ofdmModeByName(const std::string& name);   // case-insensitive, -1 when unknown

// Everything the modulator and the receiver derive from a mode (computed once, see geometry())
struct Geometry {
    int id = 0;
    int nc = 0, np = 0, ns = 0, bps = 0;
    int M = 0, Ncp = 0, sps = 0;         // samples per symbol body, cyclic prefix, symbol (at 8 kHz)
    float rs = 0;                        // symbol rate (carrier spacing), Hz
    int nlower = 0;                      // carrier k (0 .. nc + 1) is at (nlower + k) * rs Hz of the audio
    int bitsPerFrame = 0, bitsPerPacket = 0;
    int symsPerPacket = 0;               // data symbols of a packet (np * (ns - 1) * nc)
    int nuwbits = 0, nuwsyms = 0;
    std::vector<int> uwSym;              // symbol index (in the packet's data symbols) of each unique word symbol
    std::vector<uint8_t> uwBits;
    std::vector<cf> uwSyms;              // the unique word as symbols
    std::vector<uint8_t> isUw;           // per data symbol: 1 when it carries the unique word
    int codeN = 0, codeK = 0, parity = 0;    // the LDPC code as designed
    int dataBits = 0;                    // information bits that are used (the rest of codeK is shortened away, known 1)
    int codedBits = 0;                   // dataBits + parity: what goes on the air
    int codedSyms = 0;                   // codedBits / bps
    int interleaverB = 0;                // golden prime step over codedSyms
    int frameBytes = 0;                  // dataBits / 8, including the 2 CRC bytes
    int payloadBytes = 0;                // frameBytes - 2: Mercury's "payload bytes per modem frame"
    int samplesPerFrame = 0;             // ns * sps
    int burstSamples = 0;                // preamble + packet + postamble, at 8 kHz
    double burstSeconds = 0;
    double bandwidthHz = 0;              // nc * rs
    double centreHz = 0;                 // mean frequency of the data carriers
    std::vector<float> pilots;           // nc + 2 values (edge ones 0)
    std::vector<cf> preamble, postamble; // the known symbols of the preamble / postamble frame: ns rows of nc + 2 carriers, row-major
};
const Geometry& geometry(int id);

// QPSK / 16-QAM maps of codec2 (bits most significant first: the first bit of a symbol is the top bit of the table index)
cf qpskMap(int b0, int b1);
cf qam16Map(int index4);                 // index 0 .. 15, its four bits MSB first
const cf* qam16Table();
// golden prime interleaver step for n symbols (codec2 choose_interleaver_b)
int interleaverStep(int n);
// codec2's ofdm_rand_seed: n values 0 .. 32767
void ofdmRand(uint16_t* r, int n, uint64_t seed);

// ------------------------------------------------------------------------------------------------------------------ LDPC

struct LdpcCode {
    const char* name;    // codec2's name: "H_256_768_22"
    int n = 0, k = 0, m = 0;
    std::vector<int> rowStart, rowCols;   // check i covers information bits rowCols[rowStart[i] .. rowStart[i + 1])
};
const LdpcCode& ldpcCode(int id);
// parity[0 .. m) from info[0 .. k) (bits 0 / 1)
void ldpcEncode(const LdpcCode& c, const uint8_t* info, uint8_t* parity);

// Layered normalised min-sum over the IRA graph. llr[0 .. n): information bits then parity bits, > 0 means 0.
// Returns true when every check is satisfied; bits[0 .. n) gets the decisions, iters the iterations used.
class LdpcDecoder {
public:
    explicit LdpcDecoder(int codeId);
    bool decode(const float* llr, int maxIter, uint8_t* bits, int* iters = nullptr, int* unsatisfied = nullptr);
    const LdpcCode& code() const { return c_; }
private:
    const LdpcCode& c_;
    std::vector<int> chkStart_, chkVar_;   // every check's variables (information and parity)
    std::vector<float> msg_, post_;
};

// --------------------------------------------------------------------------------------------------------- the frame

// The CRC-16 Mercury puts after a frame's payload (freedv_gen_crc16: CRC-16/CCITT-FALSE)
uint16_t frameCrc(const uint8_t* p, size_t n);
// frameBytes bytes for the air: payload (payloadBytes, shorter is zero padded) and the CRC XOR seed (0 = plain), high byte first
std::vector<uint8_t> makeFrame(int modeId, const uint8_t* payload, size_t n, uint16_t seed = 0);

// --------------------------------------------------------------------------------------------------------- modulator

struct ModOptions {
    bool clip = true;          // the mode's Hilbert clipper and band-pass, as Mercury transmits (QAM16C2 is never clipped)
    float peak = 0.5f;         // the peak magnitude after the clipper (OFDM_PEAK scaled to this)
};
// One burst (preamble, packet, postamble) as the complex analytic signal of the 8 kHz audio: the real part is the audio Mercury sends,
// the carriers sit at their audio frequencies (500 .. 2500 Hz). frameBytes as makeFrame() gives them.
std::vector<cf> modulateBurst(int modeId, const std::vector<uint8_t>& frameBytes, const ModOptions& o = ModOptions());

// --------------------------------------------------------------------------------------------------------- receiver

struct OfdmFrame {
    int mode = -1;               // OfdmModeId (kMfsk, kMfsk500: an MFSK burst)
    int64_t startSample = 0;     // the preamble's first sample (8 kHz samples since the receiver's start)
    int64_t endSample = 0;       // the postamble's last
    double cfoHz = 0;            // the burst's frequency offset
    float snrDb = 0;             // estimated SNR in 3 kHz (from the pilots and the decided symbols)
    float esNoDb = 0;            // per data symbol
    float metric = 0;            // acquisition correlation (0 .. 1)
    bool viaPostamble = false;   // found by its postamble (the preamble was lost)
    bool ldpcOk = false;         // every parity check satisfied
    int ldpcIters = 0;
    int rawErrors = -1;          // coded bits whose hard decision the decoder changed (when ldpcOk)
    int uwErrors = 0;            // unique word bits wrong
    bool crcPlain = false;       // the CRC matches without a seed
    uint16_t crcRx = 0, crcCalc = 0;   // the CRC as received and as computed: the XOR is the seed of a seeded frame
    std::vector<uint8_t> payload;      // payloadBytes (without the CRC)
    std::vector<cf> constellation;     // up to 400 equalised data symbols, for the display
};

struct RxOptions {
    uint32_t modeMask = 0xFFFFFFFFu;   // which modes (OfdmModeId bits, kMfsk and kMfsk500 too) to look for
    float maxCfoHz = 60;               // acquisition range (Mercury itself searches +-50 Hz)
    float threshold = 1.0f;            // scales the detection thresholds (lower: more decode attempts)
    int maxIter = 60;                  // LDPC
};

// Complex 8 kHz samples of the sideband, the 1500 Hz audio frequency at 0 Hz (see mercury_rx.cpp), in any chunk sizes.
class OfdmReceiver {
public:
    explicit OfdmReceiver(const RxOptions& o = RxOptions());
    ~OfdmReceiver();
    void reset();
    void push(const cf* x, size_t n);
    // the same from the analytic signal of the audio (carriers at their audio frequencies, as modulateBurst gives them)
    void pushSideband(const cf* x, size_t n);
    void flush();                                   // decode what is waiting (the end of a recording)
    std::vector<OfdmFrame> take();                  // frames decoded since the last call, oldest first
    int64_t samples() const;                        // 8 kHz samples pushed
    float levelDb() const;                          // input power, dB
    uint64_t detections() const;                    // bursts acquired (decoded or not)
    uint64_t failures() const;                      // acquired bursts that did not decode
    void setOptions(const RxOptions& o);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// From 8 kHz audio (real) to the receiver's input: mixed by -1500 Hz and low-passed (the negative frequencies removed)
class AudioToBaseband {
public:
    AudioToBaseband();
    void reset();
    void process(const float* x, size_t n, std::vector<cf>& out);   // appends
private:
    std::vector<float> taps_;
    std::vector<cf> hist_;
    int pos_ = 0;
    uint64_t n_ = 0;
};

} // namespace mercury
} // namespace dect2
