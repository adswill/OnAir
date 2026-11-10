// HF utility FSK / MFSK text modes (receive only and test generators). Written from public descriptions; each decoder reads only what those describe:
//   ARQ-E and ARQ-N (ITA2-P, 7 bit words), AUTOSPEC and SPREAD 11/21/51 (10 bit Bauer code), G-TOR (Golay, interleaver, CRC),
//   DCF49 / DCF39 EFR teleswitch (200 Bd, DIN 19244 frames; the Semagyr-TOP and Versacom content stays hex), Piccolo Mk VI and Coquelet-8 (tone pairs).
// Input: complex baseband at 24 kHz, the audio frequency f at +f Hz (upper sideband of the dial).
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfu {

enum class FskKind { ArqE, ArqN, Autospec, Spread, GTor, Efr, Piccolo6, Coquelet8 };

struct FskStatus {
    int state = 0;          // 0 searching, 1 tones found, 2 characters decoded
    double centreHz = 0, shiftHz = 0, baud = 0;
    std::string detail;     // e.g. "cycle 8", "SPREAD 21", "200 Bd"
    uint64_t good = 0, bad = 0;
};

class FskRx {
public:
    explicit FskRx(FskKind k);
    ~FskRx();
    void reset();
    void setBaud(double b);        // 0 (default): automatic
    void setShift(double hz);      // 0 (default): automatic
    void push(const cf32* x, size_t n);
    void setTextSink(std::function<void(const std::string&)> cb);        // decoded text as it arrives
    void setEventSink(std::function<void(const std::string&)> cb);       // one line per message, frame or state change
    FskStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct FskGenCfg {
    FskKind kind = FskKind::ArqE;
    double baud = 96, shift = 400, centre = 1500;
    int cycle = 8;              // ARQ-E: 4 or 8 (ARQ-N has no inverted characters)
    int depth = 11;             // SPREAD 11, 21, 51
    int gtorBaud = 300;
    bool gtorGolay = false;     // the Golay form of the data frame
    double toneMs = 50;         // Piccolo, Coquelet
    double leadSec = 0.3, tailSec = 0.2;
    int idleChars = 60;
};
// ITA2 text in upper case (letters and figures with shift codes)
std::vector<cf32> genFsk(const FskGenCfg& c, const std::string& text);

// G-TOR frame layout helpers
void gtorGolayTable(std::vector<uint16_t>& g);
uint16_t gtorCrc(const uint8_t* p, size_t n);

}  // namespace dect2::hfu
