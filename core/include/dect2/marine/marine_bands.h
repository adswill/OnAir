// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The frequencies and protocols of the marine bands (ITU Radio Regulations Appendices 15, 17, 18; ITU-R M.493, M.625, M.1371; IMO NAVTEX manual),
// and the scanner that steps a single radio through the DSC channels.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

enum class DscGroup { Distress, WorldwideShip, WorldwideCoast, SeriesAShip, SeriesACoast, SeriesBShip, SeriesBCoast, Vhf, Mf455 };

struct DscFreq {
    double hz;                // the carrier (the dial frequency on a communications receiver)
    DscGroup group;
    const char* label;        // "distress and safety", "worldwide ship", ...
    int weight;               // dwell weight in the scanner: distress channels are visited more often
};
const std::vector<DscFreq>& dscFrequencies();      // 47 CFR 80.359 / RR Appendix 17: distress and safety, worldwide and series A / B calling channels
const char* dscGroupName(DscGroup g);

// One row of the "Marine bands" table.
struct MarineBand {
    const char* band;         // "MF", "HF 8 MHz", "VHF" ...
    double hz;                // where a click tunes; 0 = no tuning (text only)
    const char* name;         // "DSC distress and safety"
    const char* protocols;    // what is used there
    const char* notes;
    int family;               // OnAir mode family to open (14 AIS, 15 marine, 63 VDES, 92 COSPAS-SARSAT), -1 = tune only
    const char* sub;          // protocol entry inside the family ("dsc", "navtex", "wefax"), or nullptr
};
const std::vector<MarineBand>& marineBands();

// Steps through a list of channels: each is visited for `dwellSec`; a locked decoder or a call heard within `holdSec` keeps the radio there.
class DscScan {
public:
    struct Entry { double hz = 0; int weight = 1; };
    void setList(const std::vector<Entry>& l);
    void setDwell(double dwellSec, double holdSec) { dwell_ = dwellSec; hold_ = holdSec; }
    void restart() { idx_ = -1; }
    // Call about once a second. Returns true when the radio should be retuned to hz.
    bool update(double nowSec, bool locked, double lastCallSec, double& hz);
    double current() const { return idx_ >= 0 && !seq_.empty() ? seq_[(size_t)idx_] : 0.0; }
    size_t slots() const { return seq_.size(); }
private:
    std::vector<double> seq_;
    int idx_ = -1;
    double dwell_ = 4, hold_ = 20, slotStart_ = 0, busyUntil_ = 0;
};

// Channels of `want` that fit into a capture centred on `centreHz` with `rateHz` of bandwidth (a margin is kept at the edges).
std::vector<double> dscInCapture(const std::vector<double>& want, double centreHz, double rateHz);

} // namespace marine
} // namespace dect2
