// DAB / DAB+ receiver (ETSI EN 300 401, TS 102 563), transmission mode I (Band III, 1.536 MHz, 1536 carriers).
#pragma once
#include "dab_dmb.h"
#include "dab_tel.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace dab {

// ---- mode I geometry (samples at 2.048 Msps)
constexpr double kRate = 2048000.0;
constexpr int kTu = 2048, kTg = 504, kTs = 2552, kSymbols = 76, kTnull = 2656, kCarriers = 1536, kFrame = 196608;
constexpr int kFicSymbols = 3, kCifPerFrame = 4, kCuBits = 64, kCifCu = 864, kCifBits = kCifCu * kCuBits;

// ---- tables and bit-level helpers (dab_fec.cpp)
const std::vector<int>& freqInterleaver();            // carrier number (-768..768) of the n-th bit pair
const std::vector<cf32>& phaseReference();             // frequency-domain phase reference symbol, kTu bins (bin = k mod kTu)
uint16_t crc16(const uint8_t* d, int n);               // CRC-CCITT, init 0xFFFF, result inverted (FIB, access units)
uint16_t fireCode(const uint8_t* d, int n);            // DAB+ superframe firecode
void descramble(uint8_t* bits, int n);                 // energy dispersal PRBS x^9 + x^5 + 1, in place on one bit per byte

// Convolutional code, rate 1/4, constraint length 7 (generators 133 171 145 133 octal), soft-decision Viterbi.
// `mother` holds 4 * (nbits + 6) soft values (positive = bit 1, 0 = punctured), result: nbits decoded bits (one per byte).
void viterbiDecode(const int8_t* mother, int nbits, uint8_t* bits);
void convEncode(const uint8_t* bits, int nbits, uint8_t* mother);   // 4 * (nbits + 6) code bits, tail included

// Puncturing. Both directions work on the "received side" count: the punctured length of the code word.
bool ficDepuncture(const int8_t* rx, int8_t* mother);                    // 2304 -> 3096
void ficPuncture(const uint8_t* mother, uint8_t* out);                   // 3096 -> 2304
// EEP sub-channel protection: returns false when size/option/level do not form a valid profile
bool eepGeometry(int size, int option, int level, int& n, int& bitrate, int& infoBits);
bool eepDepuncture(const int8_t* rx, int size, int option, int level, int8_t* mother);
bool eepPuncture(const uint8_t* mother, int size, int option, int level, uint8_t* out);
// UEP sub-channel protection (short form, table 6 index 0..63): size in CUs, bitrate, protection level 1..5
bool uepGeometry(int index, int& size, int& bitrate, int& level, int& infoBits);
bool uepDepuncture(const int8_t* rx, int index, int8_t* mother);
bool uepPuncture(const uint8_t* mother, int index, uint8_t* out);   // padding bits at the end are 0

// Reed-Solomon (120,110) of DAB+, fcr 0, polynomial 0x11d. Returns number of corrected bytes or -1.
int rsDecode120(uint8_t* cw);
void rsEncode120(const uint8_t* data110, uint8_t* cw120);

} // namespace dab

class DabAudio;

class DabReceiver {
public:
    DabReceiver();
    ~DabReceiver();
    void configure(double inputRateHz);       // any rate: resampled to 2.048 Msps
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(DabTelemetry& out, uint64_t lastSeq);
    DabEnsemble ensemble() const;
    // Decode and play this sub-channel (-1 = none). A DMB video sub-channel goes to the DMB decoder, whose transport stream comes out of
    // the packet callback (24 ms of stream per call); the others go to DabAudio.
    void select(int subId);
    int selected() const;
    DabAudio& audio();
    DmbDecoder& dmb();
    void setPacketCallback(std::function<void(const uint8_t* pk, size_t n, double secs)> cb);
    // Called with every logical frame of the selected sub-channel (after Viterbi and descrambling), for tests
    void setFrameTap(std::function<void(int sub, const uint8_t* bytes, int n)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The audio side of a sub-channel: DAB+ superframes (Reed-Solomon, firecode, access units) and DAB MP2 frames, decoded with FFmpeg
// and played through AudioOut.
class DabAudio {
public:
    DabAudio();
    ~DabAudio();
    void select(int sub, bool dabPlus, int bitrate);     // starts from scratch
    void push(const uint8_t* frame, int n);              // one 24 ms logical frame
    void flushSync();                                    // the stream was interrupted
    DabAudioStats stats() const;
    void setVolume(float v);
    void setMuted(bool m);
    // Tests: receives every access unit with a good CRC (raw AAC frame) and its superframe header
    void setAuTap(std::function<void(const uint8_t* au, int n, uint8_t header)> cb);
    void setSilent(bool s);                               // decode but do not open the audio device (tests)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---- test transmitter: a complete DAB mode I signal with a small ensemble
struct DabGenService {
    uint16_t sid = 0x4001;
    std::string label;
    int subId = 1;
    bool dabPlus = true;     // DAB+ (fake access units unless `payload` is set) or DAB (MP2 tone)
    int bitrate = 48;        // kbit/s, a multiple of 8 (EEP-A, protection level 3-A)
};
struct DabGenConfig {
    std::string ensembleLabel = "OnAir Test";
    uint16_t eid = 0xC001;
    std::vector<DabGenService> services;
    double snrDb = 30, cfoHz = 0, sroPpm = 0;
    unsigned seed = 1;
    double toneHz = 440;
};

class DabGenerator {
public:
    explicit DabGenerator(const DabGenConfig& cfg);
    ~DabGenerator();
    void generate(size_t n, std::vector<cf32>& out);      // appends n samples at 2.048 Msps
    // Test hook: the exact bytes carried by the next DAB+ superframe of a sub-channel (110 * bitrate/8 bytes of payload incl. header)
    uint64_t framesSent() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
