// Single-tone HF serial modems: MIL-STD-188-110A/C fixed-frequency serial tone (75 to 4800 bit/s) and STANAG 4285 (75 to 3600 bit/s), with the
// STANAG 5066 D_PDU framing that often rides on them. Written from the published documents (MIL-STD-188-110C with Change 1, section 5.3.2; STANAG 4285
// Annexes A and E; the D_PDU layout as read by Wireshark's packet-s5066dts.c). Nothing is decrypted: payload stays as hex.
//
// Input: complex baseband at 24 kHz with the 1800 Hz sub-carrier at +1800 Hz (upper sideband of the dial).
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

constexpr double kBaseRate = 24000.0;

// K = 7, rate 1/2 (133, 171 octal, T1 first), streaming. Soft values: positive favours 0, 0 = erased.
class Viterbi {
public:
    Viterbi() { reset(); }
    void reset();
    void push(float t1, float t2);
    void take(std::vector<uint8_t>& out);    // the bits that are decided (decision depth 96 steps)
    void flush(std::vector<uint8_t>& out);   // everything left, from the best end state
    float metricPerStep() const;             // recent average branch agreement, about 1 for a clean signal, 0 for the wrong hypothesis
private:
    void trace(size_t endStep, size_t keep, int state, std::vector<uint8_t>* out);
    std::vector<float> m_, n_;
    std::vector<uint64_t> dec_;
    size_t emitted_ = 0;
    double good_ = 0, total_ = 0;
};

enum class SerialStd { Mil110, Stanag4285 };

// One STANAG 5066 D_PDU found in a decoded bit stream
struct D5066 {
    int type = -1;
    std::string typeName;
    int eot = 0;
    std::string src, dst;
    int headerBytes = 0, payload = 0;
    bool headerCrcOk = false, dataCrcOk = false;
    std::string hex;     // the first bytes of a C_PDU segment
};

// Finds D_PDUs in a stream of bits (each byte sent least significant bit first)
class Parser5066 {
public:
    void pushBit(uint8_t b, const std::function<void(const D5066&)>& cb);
    uint64_t found() const { return found_; }
private:
    std::vector<uint8_t> buf_;
    uint64_t found_ = 0;
    uint32_t window_ = 0;
    int want_ = -1;
    void tryParse(const std::function<void(const D5066&)>& cb);
};
uint16_t crc5066Header(const uint8_t* p, size_t n);
uint32_t crc5066Data(const uint8_t* p, size_t n);
const char* d5066TypeName(int t);

struct SerialEvent {
    std::string text;
    double timeSec = 0;
};

struct SerialStatus {
    int state = 0;              // 0 searching, 1 signal found, 2 decoding
    double cfoHz = 0, snrDb = 0;
    std::string mode;           // e.g. "2400 bit/s, short interleaver"
    uint64_t frames = 0, blocks = 0, bitsOut = 0;
    float bestMetric = 0;
};

class SerialRx {
public:
    explicit SerialRx(SerialStd s);
    ~SerialRx();
    // STANAG 4285 has no rate signalling: -1 tries every rate and interleaver and follows the one whose code fits; otherwise pick one of these:
    // 0 2400 coded, 1 1200 coded, 2 600, 3 300, 4 150, 5 75 (each short, or + 10 for long), 6 3600 uncoded, 7 2400 uncoded, 8 1200 uncoded
    void forceProfile(int p);
    void reset();
    void push(const cf32* x, size_t n);
    void setBitSink(std::function<void(uint8_t bit)> cb);                    // decoded user bits, in order
    void setEventSink(std::function<void(const SerialEvent&)> cb);
    void setD5066Sink(std::function<void(const D5066&)> cb);
    SerialStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Test signals (24 kHz complex, no noise; nothing is transmitted)
struct SerialGenCfg {
    SerialStd std = SerialStd::Mil110;
    int bps = 2400;             // 110: 75 .. 4800; 4285: 75 .. 3600 (see 'coded')
    bool longIntl = false;
    bool zeroIntl = false;      // 110 only: 0.0 s interleaver at the short setting
    bool coded = true;          // 4285 only: false = the uncoded 1200 / 2400 / 3600 formats
    double leadSec = 0.05, tailSec = 0.05;
};
std::vector<cf32> genSerial(const SerialGenCfg& c, const std::vector<uint8_t>& dataBits);

// Wrap payload bytes as a STANAG 5066 NON_ARQ_DATA D_PDU (for tests); returns the bytes
std::vector<uint8_t> make5066NonArq(const std::vector<uint8_t>& payload, uint32_t src, uint32_t dst);

}  // namespace dect2::hfu
