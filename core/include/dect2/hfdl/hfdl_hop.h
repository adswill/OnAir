// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL band hopping: HF only carries a band at a time of day, so the receiver follows the ground stations to the band that works now.
// The hopper is pure logic (no radio, no clock): every second or so it is given the time, the band the radio is on, the count of good
// PDUs so far, and the frequencies the heard squitters announce as active (the HFDL system table: each ground station's SPDU lists the
// frequencies in use), and it answers which band to tune to (or -1: stay).
//
// Two strategies (4.0, docs/HFDL-STATIONS-4.0.md): HfdlBandHopper "finds a working band" (it leaves a silent band and stays on the first one that
// gives PDUs), HfdlBusiestBand surveys the bands and stays on the busiest one, surveying again from time to time.
//
// Rules of HfdlBandHopper: stay on a band that yields PDUs (good rate); on a poor band dwell one look (longer when something is heard), then move to the band
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

// "Busiest band": a survey listens to each candidate band for dwellSec and scores it (good PDUs per minute, distinct aircraft and ground stations
// heard, mean SNR of the good bursts), then the radio goes to the best one and stays. While it stays, the score is measured again over windows of
// dwellSec; after rescanSec it surveys again (the band it stays on keeps its latest window score), and a window with nothing at all starts a survey
// at once. Candidates: when hints are given (the active frequencies of the nearby ground stations: squitters and the live list), the bands that
// hold them, best hinted first, plus the band it is on; without hints every common band. A user's tuning to another HFDL band starts a stay there.
class HfdlBusiestBand {
public:
    struct Config {
        double dwellSec = 75;        // two squitter frames (32 s) of every station of the band, and a little to settle
        double rescanSec = 1200;     // survey again after this long on a band
        double wPdu = 1.0;           // score per good PDU per minute
        double wAircraft = 2.0;      // per distinct aircraft heard in the window
        double wStation = 3.0;       // per distinct ground station heard in the window
        double wSnr = 0.1;           // per dB of mean SNR (only when PDUs were decoded)
        double hintAgeSec = 900;     // hints older than this are forgotten
        bool onlyHinted = true;      // with hints, survey only the hinted bands (and the current one)
        double retuneSec = 6;        // a hop is given this long to arrive
    };
    struct Heard { uint32_t key = 0; double lastSec = 0; };   // an aircraft or a ground station with its last-heard time (any clock that grows)
    struct Obs {
        double nowSec = 0;
        int band = -1;                       // the band the radio is on (-1 not an HFDL band)
        uint64_t goodPdus = 0;               // good PDUs since the receiver started
        float snrDb = 0;                     // SNR of the last good burst
        std::vector<Heard> aircraft, stations;
        std::vector<int> hintKhz;            // active frequencies near the user, most wanted first (empty: none known)
    };
    struct Score { bool measured = false; double perMin = 0; int aircraft = 0, stations = 0; double snrDb = 0, score = 0, atSec = 0; };
    enum class Phase { Idle, Survey, Stay };

    HfdlBusiestBand() = default;
    explicit HfdlBusiestBand(const Config& c) : c_(c) {}
    void reset();
    int update(const Obs& o);                // the band to tune to now, or -1 to stay
    Phase phase() const { return phase_; }
    const Score& score(int band) const;      // the last complete measurement of a band
    double scoreOf(const Score& s) const;    // the weighted sum (exposed for the tests and the panel)
    std::vector<int> candidates() const { return cand_; }
    int bestBand() const { return best_; }
    std::string status() const { return status_; }
    const Config& config() const { return c_; }
private:
    struct Win {                             // one measuring window on the current band
        double startSec = 0, lastSec = 0;
        uint64_t startPdus = 0, lastPdus = 0;
        double snrSum = 0; int snrN = 0;
        std::vector<std::pair<uint32_t, double>> acBase, gsBase;   // last-heard times at the start
        std::vector<uint32_t> ac, gs;        // heard during the window
    };
    void startWindow(const Obs& o);
    void feedWindow(const Obs& o);
    Score closeWindow(double nowSec) const;
    std::vector<int> makeCandidates(int current) const;
    int nextSurveyBand() const;
    Config c_;
    Phase phase_ = Phase::Idle;
    std::vector<Score> sc_;
    std::vector<int> cand_;
    std::vector<bool> surveyed_;
    int cur_ = -1, best_ = -1;
    int pending_ = -1, asked_ = -1; double pendingSec_ = 0;
    double stayStartSec_ = 0;
    std::vector<int> hintKhz_; double hintSec_ = -1e18;
    Win w_;
    std::string status_;
};

}  // namespace dect2
