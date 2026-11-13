// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/marine/marine_bands.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace marine {

const char* dscGroupName(DscGroup g) {
    switch (g) {
    case DscGroup::Distress: return "distress and safety";
    case DscGroup::WorldwideShip: return "worldwide, ship";
    case DscGroup::WorldwideCoast: return "worldwide, coast";
    case DscGroup::SeriesAShip: return "series A, ship";
    case DscGroup::SeriesACoast: return "series A, coast";
    case DscGroup::SeriesBShip: return "series B, ship";
    case DscGroup::SeriesBCoast: return "series B, coast";
    case DscGroup::Vhf: return "VHF channel 70";
    case DscGroup::Mf455: return "455 kHz band";
    }
    return "";
}

const std::vector<DscFreq>& dscFrequencies() {
    static const std::vector<DscFreq> v = [] {
        std::vector<DscFreq> r;
        auto k = [&](double khz, DscGroup g, const char* l, int w) { r.push_back({khz * 1e3, g, l, w}); };
        k(2187.5, DscGroup::Distress, "MF distress and safety", 4);
        k(4207.5, DscGroup::Distress, "HF distress and safety", 3);
        k(6312.0, DscGroup::Distress, "HF distress and safety", 3);
        k(8414.5, DscGroup::Distress, "HF distress and safety", 4);
        k(12577.0, DscGroup::Distress, "HF distress and safety", 3);
        k(16804.5, DscGroup::Distress, "HF distress and safety", 3);
        r.push_back({156.525e6, DscGroup::Vhf, "VHF channel 70", 4});
        const double ws[] = {2189.5, 4208.0, 6312.5, 8415.0, 12577.5, 16805.0, 18898.5, 22374.5, 25208.5};
        const double wc[] = {2177.0, 4219.5, 6331.0, 8436.5, 12657.0, 16903.0, 19703.5, 22444.0, 26121.0};
        const double as[] = {4208.5, 6313.0, 8415.5, 12578.0, 16805.5, 18899.0, 22375.0, 25209.0};
        const double ac[] = {4220.0, 6331.5, 8437.0, 12657.5, 16903.5, 19704.0, 22444.5, 26121.5};
        const double bs[] = {4209.0, 6313.5, 8416.0, 12578.5, 16806.0, 18899.5, 22375.5, 25209.5};
        const double bc[] = {4220.5, 6332.0, 8437.5, 12658.0, 16904.0, 19704.5, 22445.0, 26122.0};
        for (double f : ws) k(f, DscGroup::WorldwideShip, "worldwide calling", 1);
        for (double f : wc) k(f, DscGroup::WorldwideCoast, "worldwide calling", 1);
        for (double f : as) k(f, DscGroup::SeriesAShip, "series A calling (Atlantic, Gulf of Mexico, Caribbean)", 1);
        for (double f : ac) k(f, DscGroup::SeriesACoast, "series A calling (Atlantic, Gulf of Mexico, Caribbean)", 1);
        for (double f : bs) k(f, DscGroup::SeriesBShip, "series B calling (other areas)", 1);
        for (double f : bc) k(f, DscGroup::SeriesBCoast, "series B calling (other areas)", 1);
        k(458.5, DscGroup::Mf455, "455 kHz band ship (US)", 1);
        k(455.5, DscGroup::Mf455, "455 kHz band coast (US)", 1);
        return r;
    }();
    return v;
}

const std::vector<MarineBand>& marineBands() {
    static const std::vector<MarineBand> v = {
        {"MF", 2187.5e3, "DSC distress and safety", "DSC 100 bd FSK (170 Hz shift), M.493", "Also used for routine ship-to-ship calls with priority to distress.", 15, "dsc"},
        {"MF", 2177e3, "DSC calling, coast", "DSC 100 bd FSK", "Coast stations; ships call on 2189.5 kHz.", 15, "dsc"},
        {"MF", 2189.5e3, "DSC calling, ship", "DSC 100 bd FSK", "", 15, "dsc"},
        {"MF", 518e3, "NAVTEX international", "NAVTEX: SITOR-B FEC 100 bd, English", "Navigational and weather warnings, scheduled by station letter.", 15, "navtex"},
        {"MF", 490e3, "NAVTEX national", "NAVTEX: SITOR-B FEC 100 bd, national language", "Used by a few countries for local language.", 15, "navtex"},
        {"MF", 424e3, "NAVTEX 424 kHz", "NAVTEX: SITOR-B FEC", "Some Baltic and Turkish stations.", 15, "navtex"},
        {"MF", 2182e3, "Radiotelephony distress", "Voice J3E (USB), alarm signal", "International distress, safety and calling frequency. No decoder, tune and listen.", -1, nullptr},
        {"MF", 2174.5e3, "NBDP distress", "NBDP (SITOR-ARQ/FEC) 100 bd", "Distress telex. OnAir has no plain NBDP decoder; the NAVTEX mode reads only NAVTEX-format messages.", -1, nullptr},
        {"HF 4 MHz", 4207.5e3, "DSC distress and safety", "DSC 100 bd FSK", "", 15, "dsc"},
        {"HF 4 MHz", 4208e3, "DSC calling, ship (worldwide)", "DSC 100 bd FSK", "Coast side 4219.5 kHz.", 15, "dsc"},
        {"HF 4 MHz", 4209.5e3, "NAVTEX HF", "NAVTEX: SITOR-B FEC", "Tropical-zone NAVTEX in English.", 15, "navtex"},
        {"HF 4 MHz", 4125e3, "Radiotelephony distress", "Voice J3E (USB)", "Distress and safety working frequency.", -1, nullptr},
        {"HF 4 MHz", 4177.5e3, "NBDP distress", "NBDP 100 bd", "Distress telex; no plain NBDP decoder.", -1, nullptr},
        {"HF 6 MHz", 6312e3, "DSC distress and safety", "DSC 100 bd FSK", "", 15, "dsc"},
        {"HF 6 MHz", 6312.5e3, "DSC calling, ship (worldwide)", "DSC 100 bd FSK", "Coast side 6331 kHz.", 15, "dsc"},
        {"HF 6 MHz", 6215e3, "Radiotelephony distress", "Voice J3E (USB)", "", -1, nullptr},
        {"HF 6 MHz", 6268e3, "NBDP distress", "NBDP 100 bd", "Distress telex; no plain NBDP decoder.", -1, nullptr},
        {"HF 8 MHz", 8414.5e3, "DSC distress and safety", "DSC 100 bd FSK", "Best all-round DSC distress channel.", 15, "dsc"},
        {"HF 8 MHz", 8415e3, "DSC calling, ship (worldwide)", "DSC 100 bd FSK", "Coast side 8436.5 kHz.", 15, "dsc"},
        {"HF 8 MHz", 8291e3, "Radiotelephony distress", "Voice J3E (USB)", "", -1, nullptr},
        {"HF 8 MHz", 8376.5e3, "NBDP distress", "NBDP 100 bd", "Distress telex; no plain NBDP decoder.", -1, nullptr},
        {"HF 12 MHz", 12577e3, "DSC distress and safety", "DSC 100 bd FSK", "", 15, "dsc"},
        {"HF 12 MHz", 12577.5e3, "DSC calling, ship (worldwide)", "DSC 100 bd FSK", "Coast side 12657 kHz.", 15, "dsc"},
        {"HF 12 MHz", 12290e3, "Radiotelephony distress", "Voice J3E (USB)", "", -1, nullptr},
        {"HF 12 MHz", 12520e3, "NBDP distress", "NBDP 100 bd", "Distress telex; no plain NBDP decoder.", -1, nullptr},
        {"HF 16 MHz", 16804.5e3, "DSC distress and safety", "DSC 100 bd FSK", "", 15, "dsc"},
        {"HF 16 MHz", 16805e3, "DSC calling, ship (worldwide)", "DSC 100 bd FSK", "Coast side 16903 kHz.", 15, "dsc"},
        {"HF 16 MHz", 16420e3, "Radiotelephony distress", "Voice J3E (USB)", "", -1, nullptr},
        {"HF 16 MHz", 16695e3, "NBDP distress", "NBDP 100 bd", "Distress telex; no plain NBDP decoder.", -1, nullptr},
        {"HF 18-26 MHz", 18898.5e3, "DSC calling, ship (worldwide)", "DSC 100 bd FSK", "Also 22374.5 and 25208.5 kHz; coast side 19703.5, 22444, 26121 kHz.", 15, "dsc"},
        {"HF", 0, "Weather fax", "Radiofax (IOC 576, 120 lpm)", "Stations and schedules are in the Fax stations view.", 15, "wefax"},
        {"VHF", 156.525e6, "DSC channel 70", "DSC 1200 bd AFSK (1300 / 2100 Hz) on FM", "Distress, safety and calling on VHF.", 15, "dsc"},
        {"VHF", 156.8e6, "Channel 16", "Voice FM", "International distress, safety and calling. No decoder, tune and listen.", -1, nullptr},
        {"VHF", 161.975e6, "AIS 1 (channel 87B)", "AIS GMSK 9.6 kbit/s, M.1371", "Ship positions and static data.", 14, nullptr},
        {"VHF", 162.025e6, "AIS 2 (channel 88B)", "AIS GMSK 9.6 kbit/s", "", 14, nullptr},
        {"VHF", 161.95e6, "VDES / ASM", "VDES (M.2092): ASM and VDE-TER", "Application-specific messages: ASM 1 on channel 2027 (161.950 MHz), ASM 2 on channel 2028 (162.000 MHz).", 63, nullptr},
        {"UHF", 406.037e6, "EPIRB / PLB beacons", "COSPAS-SARSAT 406 MHz, 400 bps BPSK", "Beacon bursts; the first-generation beacons sit between 406.025 and 406.04 MHz.", 92, nullptr},
        {"UHF", 121.5e6, "EPIRB / PLB homing", "AM sweep tone", "Aircraft and homing signal of the beacons.", -1, nullptr},
        {"L band", 1537.1e6, "Inmarsat-C (EGC)", "Inmarsat-C TDM", "SafetyNET maritime safety information, 1.5 GHz.", 17, nullptr},
    };
    return v;
}

void DscScan::setList(const std::vector<Entry>& l) {
    seq_.clear();
    int maxW = 1;
    for (const Entry& e : l) if (e.weight > maxW) maxW = e.weight;
    for (int r = 0; r < maxW; r++) for (const Entry& e : l) if (e.weight > r) seq_.push_back(e.hz);
    idx_ = -1;
}

bool DscScan::update(double now, bool locked, double lastCallSec, double& hz) {
    if (seq_.empty()) return false;
    if (idx_ < 0) { idx_ = 0; slotStart_ = now; busyUntil_ = 0; hz = seq_[0]; return true; }
    if (locked) busyUntil_ = std::max(busyUntil_, now + hold_ * 0.25);
    if (lastCallSec > 0 && now - lastCallSec < hold_ && lastCallSec >= slotStart_) busyUntil_ = std::max(busyUntil_, lastCallSec + hold_);
    if (now < busyUntil_ || now - slotStart_ < dwell_) return false;
    idx_ = (idx_ + 1) % (int)seq_.size();
    slotStart_ = now;
    hz = seq_[(size_t)idx_];
    return true;
}

std::vector<double> dscInCapture(const std::vector<double>& want, double centre, double rate) {
    std::vector<double> r;
    const double half = rate * 0.5 - 8000.0;      // keep the 24 kHz channel well inside the capture
    for (double f : want) if (std::fabs(f - centre) <= half) r.push_back(f);
    return r;
}

} // namespace marine
} // namespace dect2
