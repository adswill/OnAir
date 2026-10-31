// HFDL band hopping: HF only carries a band at a time of day, so the receiver follows the ground stations to the band that works now.
// The hopper is pure logic (no radio, no clock): every second or so it is given the time, the band the radio is on, the count of good
// PDUs so far, and the frequencies the heard squitters announce as active (the HFDL system table: each ground station's SPDU lists the
// frequencies in use), and it answers which band to tune to (or -1: stay).
//
// Rules: stay on a band that yields PDUs (good rate); on a poor band dwell one look (longer when something is heard), then move to the band
// with the best estimate: its recent PDU rate (fading with age), or for a band not yet visited an optimistic guess, plus a bonus for the
// frequencies other stations announce there. When everything is silent every band is visited in turn (least recently visited first).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct HfdlBand { const char* name; double centreMhz; int loKhz, hiKhz; bool common; };   // common: part of the tour; the others (a few stations' extra frequencies) only when announced
const std::vector<HfdlBand>& hfdlBands();               // the eight common HFDL bands, lowest first, then the rare ones
int hfdlBandOfKhz(int khz);                             // index, -1 outside every band
int hfdlBandOfMhz(double mhz);                          // the band whose range holds mhz (centre +- 0.1 MHz also counts), -1 none

class HfdlBandHopper {
public:
    struct Config {
        double dwellSec = 50;        // first look at a band: a squitter comes every 32 s frame from each station
        double longDwellSec = 120;   // a band with some traffic below the good rate
        double goodPerMin = 4;       // stay when the band gives at least this many good PDUs per minute
        double rateTauSec = 120;     // smoothing of the measured rate
        double memorySec = 900;      // how long a band's old rate is believed (fades with this time constant)
        double unvisitedGuess = 3;   // PDUs per minute assumed for a band never looked at
        double hintWeight = 0.75;    // bonus per announced active frequency in a band (at most hintMax of them count)
        int hintMax = 4;
        double hintAgeSec = 600;     // an announcement older than this is forgotten
        double margin = 1.0;         // another band must beat the current one by this much
    };
    struct Obs {
        double nowSec = 0;
        int band = -1;                       // the band the radio is tuned to now (-1: not an HFDL band)
        uint64_t goodPdus = 0;               // good PDUs since the receiver started (any band)
        std::vector<int> announcedKhz;       // active frequencies of the heard ground stations (squitters), may repeat
    };
    HfdlBandHopper() = default;
    explicit HfdlBandHopper(const Config& c) : c_(c) {}
    void reset();
    // The band to tune to now, or -1 to stay where the radio is
    int update(const Obs& o);
    double rate(int band) const;             // estimated good PDUs per minute (0 not known)
    bool visited(int band) const;
    std::string status() const;              // one line for the screen: "8.9 MHz: 7 PDU/min, staying" / "10.06 MHz: silent, next look in 20 s"
    const Config& config() const { return c_; }
private:
    struct B { bool visited = false; double rate = 0, seenSec = 0, hintSec = 0; int hint = 0; };
    Config c_;
    std::vector<B> b_;
    int cur_ = -1;
    double enteredSec = 0, lastSec = 0;
    uint64_t lastGood = 0;
    bool started_ = false;
    int pending_ = -1; double pendingSec_ = 0;   // a hop asked for and not yet seen arrive
    std::string status_;
    double decayed(int i, double now) const;
};

}  // namespace dect2
