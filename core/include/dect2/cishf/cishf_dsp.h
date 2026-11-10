// CIS (Russian) HF data modes: demodulators, frame readers, detectors and test generators. Receive only.
// Written from public descriptions only (sigidwiki, Wavecom decoder notes, SHOC mode sheets, the Rivet decoder's notes on CIS 36-50; see
// docs/CIS-HF-3.9.md). What each kind reads:
//   Bee      CIS-36-50 / CIS-50-50 (BEE, T600): 2-FSK, 36 / 50 Bd idle, 50 / 100 Bd traffic. Bit sync, 44-bit start word (21 ones), the 70-bit
//            session key sent twice, 7-bit 3:4 characters (encrypted: shown as hex), end of message 0x77 0x77 0x77. Nothing is decrypted.
//   Cis14    CIS-14 (AMOR, AMOR96): 2-FSK 96 Bd; the 14-bit frame (2 state bits, two bit-interleaved 5-bit M2 code words, 2 parity bits) is found
//            from its period; the fields are shown raw (the parity rule and the state codes are not published, so they are not checked).
//   Cis12    CIS-12 (MS5, AT-3004D, FIRE): 12 carriers 700 - 2900 Hz, 200 Hz apart, 120 Bd DBPSK / DQPSK, pilot at 3300 Hz. Physical layer: the
//            pilot, the carriers, the symbol clock, the PSK order and the raw phase steps of every carrier (no frame format is published).
//   Cis36    CIS-36 (MFSK-36 duplex ARQ): identification only: tone count, 40 Hz spacing, 10 / 20 / 40 Bd, the three tone groups.
//   Hdr      CIS OFDM HDR modem (CIS-45, CIS-60, CIS-93): identification only, from the OFDM symbol (useful part, guard, rate), carrier count and PSK order.
//   C128     CIS-128: identification only: about 128 OFDM carriers at about 21 Bd in 3 kHz (the 6 kHz form does not fit the 12 kHz stream).
//   C3000    CIS-3000: identification only: single carrier 8-PSK at 3000 Bd.
// Input: complex baseband at kRate (12 kHz), the audio frequency f of the upper sideband at +f Hz.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::cishf {

constexpr double kRate = 12000.0;
enum Kind { Bee = 0, Cis14 = 1, Cis12 = 2, Cis36 = 3, Hdr = 4, C128 = 5, C3000 = 6 };
constexpr int kKinds = 7;

struct Param { std::string key, value; };

struct Status {
    int state = 0;              // 0 searching, 1 signal found (parameters measured), 2 frames / symbols being read
    std::string mode;           // one line: what was found
    std::vector<Param> params;  // measured parameters, in display order
    uint64_t good = 0, bad = 0; // characters / frames / symbols read, and the ones that failed a check
    double cfoHz = 0;           // CIS-12: the pilot's offset from 3300 Hz
    double snrDb = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual void reset() = 0;
    virtual void push(const cf32* x, size_t n) = 0;
    virtual Status status() const = 0;
    void setTextSink(std::function<void(const std::string&)> cb) { text_ = std::move(cb); }
    void setEventSink(std::function<void(const std::string&)> cb) { event_ = std::move(cb); }
    // Advanced settings, 0 = automatic. FSK kinds: baud and shift; the others ignore them.
    virtual void setForced(double baud, double shiftHz) { (void)baud; (void)shiftHz; }
protected:
    void text(const std::string& s) { if (text_) text_(s); }
    void event(const std::string& s) { if (event_) event_(s); }
private:
    std::function<void(const std::string&)> text_, event_;
};

std::unique_ptr<Decoder> makeDecoder(int kind);

// ---- frame facts, shared by the decoder, the generator and the tests ----
inline int ones7(int c) { int n = 0; for (int i = 0; i < 7; i++) n += (c >> i) & 1; return n; }
inline bool beeValidChar(int c) { return c >= 0 && c < 128 && ones7(c) == 3; }   // 7-bit 3:4 characters
constexpr int kBeeEom = 0x77;                                           // end of message: 1110111, sent at least three times

// ---- test generators (12 kHz complex) ----
struct BeeMsg {
    uint64_t sync = 0;              // 44-bit start word; 0 = a fixed word with 21 ones (the real word is not published, only its weight)
    std::vector<int> key;           // 10 characters (3:4)
    std::vector<int> chars;         // the message (3:4 characters)
    bool withStart = true;          // false: a follow-on message, sent without the start sequence
};
struct GenOpts {
    double centreHz = 1500, shiftHz = 200;
    double idleBaud = 36, trafficBaud = 50;   // Bee
    double idleSec = 2.0;                     // Bee idle reversals before the traffic
    bool invert = false;                      // FSK: false = the 1 bits on the lower tone (as Rivet reads real BEE recordings), true = on the higher
    // Cis36: trafficBaud is the tone rate (10, 20 or 40)
    int variant = 0;                          // Cis12: PSK order 4 (0) or 2 (1). Hdr: 0 CIS-45 40 Bd, 1 CIS-45 33.3 Bd, 2 CIS-60, 3 CIS-93. C128: 0 3 kHz, 1 6 kHz
    int symbols = 600;                        // Cis12 / Cis36 / Hdr / C128 / C3000: number of symbols
    uint32_t seed = 1;
    double leadSec = 0.3, tailSec = 0.3;      // silence around the burst
};
// Bee: the messages in one transmission (the first must have the start sequence)
std::vector<cf32> genBee(const GenOpts& o, const std::vector<BeeMsg>& msgs);
// CIS-14: the frames' 14 bits each, sent as given (bit 0 first)
std::vector<cf32> genCis14(const GenOpts& o, const std::vector<uint16_t>& frames);
uint16_t cis14Frame(int state2, int a5, int b5, int parity2);   // assemble: bits 0-1 state, 2..11 a/b interleaved (a on even), 12-13 parity
// CIS-12: returns the transmitted phase steps (per symbol, 12 carriers, values 0 .. order-1) in steps
std::vector<cf32> genCis12(const GenOpts& o, std::vector<std::vector<uint8_t>>* steps);
std::vector<cf32> genCis36(const GenOpts& o);
std::vector<cf32> genOfdm(int kind, const GenOpts& o);    // Hdr or C128
std::vector<cf32> genCis3000(const GenOpts& o);
// Test impairments: carrier offset, white noise for an SNR in a 3 kHz band (>= 90: none), sample clock error in ppm
std::vector<cf32> impair(const std::vector<cf32>& x, double cfoHz, double snrDb3k, double ppm, uint32_t seed);

const char* kindName(int k);

}  // namespace dect2::cishf
