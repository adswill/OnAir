// MIL-STD-188-110B/C Appendix C high-rate serial tone (3200 to 12800 bit/s), the waveform STANAG 4539 also uses: receiver and test generator.
// Written from the published text of MIL-STD-188-110B Appendix C (and the same appendix in 110C with Change 1): 2400 Bd on 1800 Hz, a
// 287-symbol preamble (184 sync symbols, then 103 symbols carrying the data rate and interleaver length in three Barker-coded QPSK symbols
// D0 D1 D2), then frames of 256 data symbols and a 31-symbol mini-probe, with the 103-symbol block reinserted after every 72 frames.
// QPSK / 8PSK / 16QAM / 32QAM / 64QAM data, scrambled by x^9 + x^4 + 1, a K = 7 rate 1/2 code made tail-biting and punctured to 3/4, a
// block interleaver of 1 to 72 frames, an optional 32-bit end-of-message pattern.
//
// Figure C-5 (the scrambler drawing) is not text: the shift direction used here is an interpretation, see docs/HF-MIL-3.9.md.
// Multipath is handled by a decision-feedback equaliser (feed-forward -2 T .. +6 T, 8 feedback taps, RLS) trained on the known symbols.
// Receive only; nothing is decrypted (the payload stays as bytes). Input: complex baseband at 24 kHz with the sub-carrier at +1800 Hz.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/hfutil/hfu_serial.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

constexpr uint32_t kHdrEom = 0x4B65A5B2u;     // C.5.4.3, the leftmost bit sent first
int hdrBitsPerSymbol(int bps);                // 3200: 2, 4800: 3, 6400: 4, 8000: 5, 9600 and 12800: 6; 0 if unknown
bool hdrValid(int bps, int frames);           // the 30 coded combinations, and 12800 with 1 frame (no interleaving)
int hdrInterleaverBits(int bps, int frames);  // table C-XIII (12800: 1536 per frame, uncoded)
int hdrInputBits(int bps, int frames);        // table C-XII
int hdrIncrement(int bps, int frames);        // table C-XIV
const char* hdrInterleaverName(int frames);   // "ultra short" .. "very long"
// D0, D1, D2 of table C-VIII for a rate and interleaver; and back (false: not one of the 31 combinations)
void hdrDValues(int bps, int frames, int d[3]);
bool hdrFromD(const int d[3], int& bps, int& frames);
// the coding of one interleaver block: in = input bits (hdrInputBits), out = the punctured bits in fetch order (hdrInterleaverBits)
std::vector<uint8_t> hdrEncodeBlock(const std::vector<uint8_t>& in, int bps, int frames);
// and its decoder: soft values in fetch order (positive favours 0, 0 erased), returns the input bits
std::vector<uint8_t> hdrDecodeBlock(const std::vector<float>& soft, int bps, int frames);
// the scrambler: the value added (PSK) or XORed (QAM) to symbol i of a data frame
int hdrScrambleValue(int symbolIndex, int bitsPerSym);

struct HdrStatus {
    int state = 0;              // 0 searching, 2 decoding
    double cfoHz = 0, snrDb = 0;
    int bps = 0, frames = 0;
    std::string mode;           // e.g. "9600 bit/s, short interleaver (9 frames)"
    uint64_t messages = 0, superframes = 0, framesRx = 0, blocks = 0, bytesOut = 0, eoms = 0;
};

class HighRateRx {
public:
    HighRateRx();
    ~HighRateRx();
    void reset();
    void push(const cf32* x, size_t n);
    void setByteSink(std::function<void(uint8_t)> cb);                     // decoded bytes, least significant bit first, aligned to the start of a message
    void setEventSink(std::function<void(const SerialEvent&)> cb);
    void setD5066Sink(std::function<void(const D5066&)> cb);
    // the decision-feedback equaliser (on by default): RLS, trained on the preamble, the mini-probes and the reinserted preambles.
    // Off: the channel is interpolated between the mini-probes (one tap), as before 3.9.
    void setEqualizer(bool on);
    HdrStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Test signal (24 kHz complex, sub-carrier at +1800 Hz, no noise; nothing is transmitted)
struct HdrGenCfg {
    int bps = 9600;
    int frames = 1;             // interleaver length: 1, 3, 9, 18, 36 or 72 frames
    int agcBlocks = 0;          // N blocks of the conjugated first 184 preamble symbols (0 .. 7)
    bool eom = true;
    double leadSec = 0.05, tailSec = 0.05;
};
std::vector<cf32> genHighRate(const HdrGenCfg& c, const std::vector<uint8_t>& dataBits);
// bytes as bits, least significant bit first (the way the receiver reads them back)
std::vector<uint8_t> hdrBytesToBits(const std::string& s);

}  // namespace dect2::hfu
