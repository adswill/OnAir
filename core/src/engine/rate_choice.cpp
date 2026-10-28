#include "dect2/engine/rate_choice.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

static std::string msps(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.3f", hz / 1e6);
    std::string s = b;   // 2.048, 10, 0.25: no trailing zeros
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

RateLimits rateLimitsOf(const DeviceInfo& d) {
    RateLimits L;
    if (!d.isRadio()) return L;
    L.minHz = d.minRateHz; L.maxHz = d.maxRateHz; L.ranges = d.rateRanges;
    if (d.kind == DeviceInfo::HackRF) { if (L.minHz <= 0) L.minHz = 2e6; if (L.maxHz <= 0) L.maxHz = 20e6; }   // hackrf_set_sample_rate: 2 to 20 Msps
    for (auto& r : L.ranges) if (r.first > r.second) std::swap(r.first, r.second);
    std::sort(L.ranges.begin(), L.ranges.end());
    return L;
}

static bool allSingle(const RateLimits& L) {
    if (L.ranges.empty()) return false;
    for (const auto& r : L.ranges) if (r.second - r.first > 1.0) return false;
    return true;
}

double deliveredRate(const RateLimits& L, double hz) {
    if (!L.ranges.empty()) {
        double above = 0, fastest = 0;
        for (const auto& r : L.ranges) {
            if (L.maxHz > 0 && r.first > L.maxHz + 1) continue;   // a range beyond what the driver lets through (the RTL-SDR's 3.2 Msps)
            const double hi = L.maxHz > 0 ? std::min(r.second, L.maxHz) : r.second;
            const double c = std::min(std::max(hz, r.first), hi);
            fastest = std::max(fastest, c);
            if (c >= hz - 1.0 && (above == 0 || c < above)) above = c;
        }
        if (above > 0 || fastest > 0) return above > 0 ? above : fastest;
    }
    double r = hz;
    if (L.maxHz > 0) r = std::min(r, L.maxHz);
    if (L.minHz > 0) r = std::max(r, L.minHz);
    return r;
}

std::vector<RateEntry> rateEntries(const RateLimits& L, double modeMinHz) {
    static const double kLadder[] = {0.25e6, 0.5e6, 1e6, 1.024e6, 2e6, 2.048e6, 2.4e6, 2.56e6, 3.2e6, 4e6, 5e6, 6e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
    std::vector<double> ask;
    if (allSingle(L)) for (const auto& r : L.ranges) ask.push_back(r.first);
    else ask.assign(std::begin(kLadder), std::end(kLadder));
    // the wide captures of multi-channel TV (a LimeSDR, USRP, BladeRF, PlutoSDR): only for a radio that says it runs above 20 Msps
    if (!allSingle(L) && L.maxHz > 20e6 + 1) {
        static const double kWide[] = {25e6, 30.72e6, 40e6, 50e6, 56e6, 61.44e6};
        for (double w : kWide) if (w <= L.maxHz + 1) ask.push_back(w);
    }
    std::vector<RateEntry> out;
    for (double a : ask) {
        if (L.minHz > 0 && a < L.minHz - 1) continue;
        if (L.maxHz > 0 && a > L.maxHz + 1) continue;
        const double g = deliveredRate(L, a);
        if (modeMinHz > 0 && g < modeMinHz - 1) continue;
        if (!L.ranges.empty() && !allSingle(L) && std::fabs(g - a) > 1.0) continue;   // in a gap between the radio's ranges
        bool dup = false;
        for (const auto& e : out) if (std::fabs(e.getHz - g) <= 1.0) dup = true;
        if (!dup) out.push_back({a, g});
    }
    std::sort(out.begin(), out.end(), [](const RateEntry& x, const RateEntry& y) { return x.getHz < y.getHz; });
    return out;
}

RateCheck checkManualRate(const RateLimits& L, double modeMinHz, double hz) {
    // 3.4: nothing is refused here. A rate outside what the radio lists, in a gap between its ranges or below what the decoder works with is
    // handed to the driver as typed; `why` is then a warning for the user (the driver has the last word and its own error message is shown).
    RateCheck c;
    if (!(hz > 0) || !std::isfinite(hz)) { c.why = "enter a rate in Msps"; return c; }
    std::string warn;
    const bool lo = L.minHz > 0 && hz < L.minHz - 1, hi = L.maxHz > 0 && hz > L.maxHz + 1;
    if (lo || hi) {
        if (L.minHz > 0 && L.maxHz > 0) warn = "this radio lists " + msps(L.minHz) + " to " + msps(L.maxHz) + " Msps; the driver may refuse " + msps(hz) + " Msps";
        else warn = lo ? "this radio lists at least " + msps(L.minHz) + " Msps; the driver may refuse " + msps(hz) + " Msps" : "this radio lists at most " + msps(L.maxHz) + " Msps; the driver may refuse " + msps(hz) + " Msps";
    } else if (!L.ranges.empty() && !allSingle(L)) {
        double below = 0, above = 0;
        bool inside = false;
        for (const auto& r : L.ranges) {
            if (hz >= r.first - 1 && hz <= r.second + 1) inside = true;
            if (r.second < hz) below = std::max(below, r.second);
            if (r.first > hz && (above == 0 || r.first < above)) above = r.first;
        }
        if (!inside) {
            if (below > 0 && above > 0) warn = "this radio lists nothing between " + msps(below) + " and " + msps(above) + " Msps; the driver may refuse " + msps(hz) + " Msps";
            else warn = "this radio does not list " + msps(hz) + " Msps; the driver may refuse it";
        }
    }
    c.hz = warn.empty() ? deliveredRate(L, hz) : hz;
    if (warn.empty() && modeMinHz > 0 && c.hz < modeMinHz - 1) warn = "below " + msps(modeMinHz) + " Msps this decoder may not work";
    c.why = warn;
    c.ok = true;
    return c;
}

double basebandFilterFor(double filterHz, double autoRateHz, double rateHz) {
    if (!(filterHz > 0) || filterHz > rateHz) return 0;
    if (autoRateHz > 0 && rateHz > autoRateHz * 1.01) return 0;
    return filterHz;
}

static bool streamsCs8(const DeviceInfo& d, const TuneSettings& t) {
    for (const auto& r : d.settings) if (r.key == "iqformat") return radioOption(t, "iqformat", "cs16") == "cs8";
    return false;
}
static bool canCs8(const DeviceInfo& d) {
    for (const auto& r : d.settings) if (r.key == "iqformat") return true;
    return false;
}

double linkCapHz(const DeviceInfo& d, const TuneSettings& t) {
    double steady = d.steadyRateHz;
    for (const auto& r : d.settings) if (r.key == "link") {   // a Pluto at its default address: the user says USB cable or Ethernet (#96)
        const std::string l = radioOption(t, "link", "auto");
        if (l == "ethernet") steady = 0; else if (l == "usb" && steady <= 0) steady = 3e6;
    }
    if (steady <= 0) return 0;
    return steady * (streamsCs8(d, t) ? 2 : 1);
}

double linkRateFor(const DeviceInfo& d, const TuneSettings& t, double wantHz, double needHz) {
    const double hard = d.maxRateHz > 0 ? std::min(wantHz, d.maxRateHz) : wantHz;
    const double link = linkCapHz(d, t);
    if (link <= 0) return hard;
    if (needHz <= link) return std::min(hard, link);   // the mode fits the link: nothing lost
    return hard;                                       // it does not: the mode's own rate, some samples lost (linkNote says so)
}

double dvbNativeRate(double bandwidthMhz) { return 64e6 / 7 * bandwidthMhz / 8; }

std::string usbLinkNote(double rateHz, double linkHz, bool tezuka, bool cs8) {
    if (linkHz <= 0 || rateHz <= linkHz * 1.01) return "";
    std::string fix;
    if (cs8) fix = "Connect the Pluto over Ethernet (a USB Ethernet adapter on the Pluto) to stop it.";
    else if (tezuka) fix = "Set the radio's IQ format to CS8 (half the data, up to 8 Msps over USB), or connect the Pluto over Ethernet.";
    else fix = "Connect the Pluto over Ethernet, or install the Tezuka firmware and set the IQ format to CS8 (up to 8 Msps over USB).";
    return "PlutoSDR over its USB cable carries about " + msps(linkHz) + " Msps without loss; this mode runs it at " + msps(rateHz) +
           " Msps, so samples are lost and the picture or sound may break up. " + fix;
}

std::string linkNote(const DeviceInfo& d, const TuneSettings& t, double rateHz) {
    return usbLinkNote(rateHz, linkCapHz(d, t), canCs8(d), streamsCs8(d, t));
}

} // namespace dect2
