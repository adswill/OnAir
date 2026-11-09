// BTSC multichannel sound for NTSC (FCC OET Bulletin 60): on the FM sound carrier 4.5 MHz above vision, a pilot at the line rate fH = 15734 Hz (5 kHz deviation),
// L+R at 25 kHz peak with 75 us pre-emphasis, L-R on a DSB-SC subcarrier at 2 fH (50 kHz peak), and the second audio program as FM on 5 fH (+-10 kHz).
// L-R and SAP go through the dbx-TV compander. The receiver finds the pilot by itself; BtscTx makes the test signal (nothing is transmitted).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The BTSC compander (OET 60 B(c)), at 150 - 250 kHz. Levels are fractions of full subcarrier modulation.
struct BtscFilters;
class BtscCompander {
public:
    explicit BtscCompander(double rate = 192000);
    void setRate(double rate);
    ~BtscCompander();
    void reset();
    float compress(float x);    // fixed pre-emphasis F(f), wideband compression, spectral compression
    float expand(float y);      // the inverse
    double wideband() const;    // the wideband factor in use (dB in telemetry)
    double spectral() const;    // b
private:
    double rate_;
    std::unique_ptr<BtscFilters> f_;
};

struct BtscStatus {
    bool pilot = false;          // the 15.734 kHz pilot is locked: a stereo broadcast
    bool sap = false;            // a carrier sits at 5 fH: second audio program
    double pilotKhz = 0;         // measured deviation of the pilot (5 kHz nominal)
    double pilotErrHz = 0;       // pilot frequency against 15734.264 Hz
    double sapErrHz = 0;
    float subLevel = 0;          // L-R subcarrier level, fraction of 50 kHz
};

class BtscRx {
public:
    BtscRx();
    ~BtscRx();
    void configure(double fs);  // input sample rate (at least 500 kHz)
    void reset();
    // carrierHz: the sound carrier from the centre of the input. Appends 48 kHz audio: L and R (no pilot: the main channel on both). wantSap: the second
    // program on both channels, while it is there.
    void process(const cf32* x, size_t n, double carrierHz, bool wantSap, std::vector<float>& l, std::vector<float>& r);
    BtscStatus status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct BtscTxConfig {
    double rate = 2e6;
    double carrierHz = 0;
    double toneLHz = 1000, toneRHz = 400, sapHz = 700;
    double level = 0.25;         // of full modulation, the L and R tones before encoding
    bool stereo = true, sap = true;
    double cnrDb = 200;          // in 200 kHz; 200 = no noise
    uint32_t seed = 1;
};

class BtscTx {
public:
    explicit BtscTx(const BtscTxConfig& c);
    ~BtscTx();
    void generate(size_t n, std::vector<cf32>& out);   // appends n samples
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
