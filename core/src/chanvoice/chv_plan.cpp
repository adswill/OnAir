// Channel plans of the channelised analog voice services (see chv_plan.h).
#include "dect2/chanvoice/chv_plan.h"
#include "dect2/ham/ham_voice.h"
#include "dect2/tones/tones_proto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace dect2 {
namespace chv {

const char* serviceId(Service s) {
    static const char* k[] = {"cb", "frs", "pmr446", "hamfm", "hfvoice", "mdc1200", "wmic"};
    return (unsigned)s < (unsigned)kServiceCount ? k[(int)s] : "";
}
const char* serviceName(Service s) {
    static const char* k[] = {"CB radio", "FRS / GMRS", "PMR446 analog", "Ham FM repeaters", "HFGCS / HF voice", "MDC-1200", "Wireless mic finder"};
    return (unsigned)s < (unsigned)kServiceCount ? k[(int)s] : "";
}
const char* demodName(Demod d) {
    switch (d) { case Demod::Nfm: return "FM"; case Demod::Am: return "AM"; case Demod::Usb: return "USB"; case Demod::Lsb: return "LSB"; case Demod::Wfm: return "WFM"; default: return "AM / FM / SSB"; }
}

const std::vector<RegionInfo>& regions(Service s) {
    static const std::vector<RegionInfo> cb = {{"eu", "Europe (CEPT, 40 channels)"}, {"eu80", "Germany, Czechia, Hungary (80 channels)"}, {"uk", "United Kingdom (CEPT + 27/81)"}, {"us", "USA, Canada (40 channels)"}};
    static const std::vector<RegionInfo> frs = {{"us", "USA, Canada (FRS 1 - 22, GMRS)"}};
    static const std::vector<RegionInfo> pmr = {{"eu", "Europe (16 channels)"}};
    static const std::vector<RegionInfo> ham = {{"r1", "IARU Region 1 (Europe, Africa, Middle East)"}, {"r2", "IARU Region 2 (the Americas)"}, {"r3", "IARU Region 3 (Asia, Pacific)"}};
    static const std::vector<RegionInfo> hf = {{"world", "World"}};
    static const std::vector<RegionInfo> mdc = {{"world", "Any frequency"}};
    static const std::vector<RegionInfo> mic = {{"eu", "Europe"}, {"us", "USA, Canada"}};
    switch (s) {
    case Service::Cb: return cb; case Service::Frs: return frs; case Service::Pmr: return pmr; case Service::Ham: return ham;
    case Service::HfVoice: return hf; case Service::Mdc: return mdc; default: return mic;
    }
}
int regionIndex(Service s, const std::string& id) {
    const auto& r = regions(s);
    for (size_t i = 0; i < r.size(); i++) if (id == r[i].id) return (int)i;
    return -1;
}

int regionForCountry(Service s, const std::string& iso2) {
    std::string c; for (char ch : iso2) c += (char)std::toupper((unsigned char)ch);
    if (c.size() != 2) return -1;
    static const char* r2[] = {"US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "VE", "UY", "PY", "BO", "EC", "CR", "PA", "GT", "DO", "PR", "CU", "JM"};
    static const char* r3[] = {"AU", "NZ", "JP", "KR", "CN", "TW", "HK", "SG", "MY", "TH", "VN", "ID", "PH", "IN", "PK", "BD", "LK", "NP"};
    auto in = [&](const char* const* a, size_t n) { for (size_t i = 0; i < n; i++) if (c == a[i]) return true; return false; };
    const bool am = in(r2, sizeof r2 / sizeof *r2), as = in(r3, sizeof r3 / sizeof *r3);
    switch (s) {
    case Service::Cb:
        if (c == "GB" || c == "UK") return 2;
        if (c == "DE" || c == "CZ" || c == "HU") return 1;
        if (c == "US" || c == "CA") return 3;
        return 0;
    case Service::Ham: return am ? 1 : as ? 2 : 0;
    case Service::Mic: return (c == "US" || c == "CA") ? 1 : 0;
    default: return 0;
    }
}
int regionForPosition(Service s, double lat, double lon) {
    const int iaru = ham::hamIaruRegionForPosition(lat, lon);
    switch (s) {
    case Service::Cb:
        if (iaru == 2 && lat > 15 && lon < -50) return 3;                          // North America
        if (lat > 49.5 && lat < 61 && lon > -8.7 && lon < 2.0) return 2;           // Great Britain and Northern Ireland (Ireland is inside this box: CEPT there)
        if (lat > 47.2 && lat < 55.1 && lon > 5.8 && lon < 19.0) return 1;         // Germany, Czechia and Hungary roughly
        return 0;
    case Service::Ham: return iaru >= 1 && iaru <= 3 ? iaru - 1 : 0;
    case Service::Mic: return (iaru == 2 && lat > 15 && lon < -50) ? 1 : 0;
    default: return 0;
    }
}

namespace {

Channel mk(int num, const std::string& label, double mhz, Demod d, double bw, unsigned flags, const std::string& name, const std::string& note = "") {
    Channel c; c.number = num; c.label = label; c.mhz = mhz; c.demod = d; c.bwHz = bw; c.flags = flags; c.name = name; c.note = note; return c;
}

// The 40 CB channels 26.965 - 27.405 MHz. cept: channels 23, 24, 25 in number order (27.235, 27.245, 27.255), else the FCC order (27.255, 27.235, 27.245)
double cbMhz(int n, bool cept) {
    static const double us[40] = {26.965, 26.975, 26.985, 27.005, 27.015, 27.025, 27.035, 27.055, 27.065, 27.075, 27.085, 27.105, 27.115, 27.125, 27.135, 27.155,
                                  27.165, 27.175, 27.185, 27.205, 27.215, 27.225, 27.255, 27.235, 27.245, 27.265, 27.275, 27.285, 27.295, 27.305, 27.315, 27.325,
                                  27.335, 27.345, 27.355, 27.365, 27.375, 27.385, 27.395, 27.405};
    if (cept && n >= 23 && n <= 25) return 27.235 + 0.010 * (n - 23);
    return us[n - 1];
}

std::vector<Channel> buildCb(int region) {
    std::vector<Channel> v;
    const bool cept = region != 3;
    for (int n = 1; n <= 40; n++) {
        Channel c = mk(n, std::to_string(n), cbMhz(n, cept), Demod::Auto, 10000, kSsb, "Channel " + std::to_string(n));
        if (n == 9) { c.flags |= kEmergency; c.name = "9 Emergency"; c.note = region == 3 ? "FCC: emergency communications and roadside assistance only." : "The emergency channel of the CEPT plan."; }
        if (n == 19) { c.flags |= kCalling; c.name = "19 Truckers, road information"; c.note = region == 3 ? "The highway and truckers' channel in the USA." : "The usual calling and truckers' channel."; }
        if (n == 11 && region == 3) { c.name = "11 Calling (some areas)"; }
        if (n == 16 && region == 3) c.name = "16 SSB calling (USB)";
        if (n == 38 && region == 3) c.name = "38 SSB calling (LSB)";
        if (n == 36 && region == 3) c.name = "36 SSB (LSB)";
        if (n == 24 || n == 25) c.note += " Data / remote control use is common on 24 and 25 in some countries.";
        v.push_back(c);
    }
    if (region == 1) {   // 41 .. 80: 26.565 - 26.955 MHz in 10 kHz steps
        for (int n = 41; n <= 80; n++) v.push_back(mk(n, std::to_string(n), 26.565 + 0.010 * (n - 41), Demod::Auto, 10000, kSsb, "Channel " + std::to_string(n), "The extra 40 channels of the German plan, 26.565 - 26.955 MHz."));
        std::sort(v.begin(), v.end(), [](const Channel& a, const Channel& b) { return a.number < b.number; });
    }
    if (region == 2) {   // the 27/81 plan: 40 FM channels, 27.60125 + 10 kHz (n - 1); numbered 41 .. 80 here, "U1" .. "U40" in the label
        for (int n = 1; n <= 40; n++) v.push_back(mk(40 + n, "U" + std::to_string(n), 27.60125 + 0.010 * (n - 1), Demod::Nfm, 10000, 0, "27/81 channel " + std::to_string(n), "UK 27/81 plan (MPT 1320), FM."));
    }
    return v;
}

std::vector<Channel> buildFrs() {
    std::vector<Channel> v;
    for (int n = 1; n <= 22; n++) {
        double mhz; unsigned fl = 0; std::string note;
        if (n <= 7) mhz = 462.5625 + 0.025 * (n - 1);
        else if (n <= 14) { mhz = 467.5625 + 0.025 * (n - 8); fl = kLowPower; note = "FRS only, 0.5 W."; }
        else { mhz = 462.550 + 0.025 * (n - 15); note = "Shared by FRS and GMRS; with a GMRS licence also a repeater output (the input is 467.550 + 25 kHz n)."; }
        Channel c = mk(n, std::to_string(n), mhz, Demod::Nfm, n <= 14 ? 12500 : 25000, fl, "Channel " + std::to_string(n), note);
        if (n >= 15) { c.txMhz = 467.550 + 0.025 * (n - 15); c.flags |= kRepeater | kLicence; c.bwHz = 12500; }
        if (n >= 15 && n <= 22) c.flags |= 0;
        v.push_back(c);
    }
    v[0].note = "FRS channel 1; also GMRS simplex.";
    // 20 kHz tuned: 462.675 (ch 20) is the GMRS emergency / travellers' assistance channel in the 2017 rules? Not stated: no note.
    return v;
}

std::vector<Channel> buildPmr() {
    std::vector<Channel> v;
    for (int n = 1; n <= 16; n++) {
        Channel c = mk(n, std::to_string(n), 446.00625 + 0.0125 * (n - 1), Demod::Nfm, 12500, kLowPower | (n > 8 ? kDigitalAlso : 0u), "Channel " + std::to_string(n),
                       n <= 8 ? "Analogue PMR446 since 1998." : "Analogue since 2016; before that digital only (dPMR / DMR Tier 1 share these frequencies).");
        v.push_back(c);
    }
    return v;
}

void addRpt(std::vector<Channel>& v, double outMhz, double shiftMhz, const std::string& label, double bw) {
    Channel c = mk(0, label, outMhz, Demod::Nfm, bw, kRepeater | kLicence, label, "Repeater output; the input is the output " + std::string(shiftMhz < 0 ? "minus " : "plus ") + std::to_string(std::fabs(shiftMhz)).substr(0, 5) + " MHz.");
    c.txMhz = outMhz + shiftMhz;
    v.push_back(c);
}

std::vector<Channel> buildHam(int region) {
    std::vector<Channel> v;
    char b[24];
    if (region == 0) {   // IARU Region 1
        { Channel c = mk(0, "V40", 145.500, Demod::Nfm, 12500, kCalling | kLicence, "2 m calling 145.500", "FM simplex calling channel of the Region 1 band plan."); v.push_back(c); }
        for (int i = 0; i < 16; i++) { snprintf(b, sizeof b, "RV%d", 48 + i); addRpt(v, 145.600 + 0.0125 * i, -0.6, b, 12500); }
        { Channel c = mk(0, "U272", 433.500, Demod::Nfm, 12500, kCalling | kLicence, "70 cm calling 433.500", "FM simplex calling channel of the Region 1 band plan."); v.push_back(c); }
        for (int i = 0; i < 32; i++) { snprintf(b, sizeof b, "%.4f", 438.650 + 0.025 * i); addRpt(v, 438.650 + 0.025 * i, -7.6, b, 12500); }
    } else if (region == 1) {   // Region 2 (USA, Canada): 15 kHz repeater channels, the 70 cm +5 MHz outputs
        v.push_back(mk(0, "146.520", 146.520, Demod::Nfm, 15000, kCalling | kLicence, "2 m national calling 146.520", "The FM simplex calling frequency of the USA and Canada."));
        for (int i = 0; i < 25; i++) { snprintf(b, sizeof b, "%.3f", 146.610 + 0.015 * i); addRpt(v, 146.610 + 0.015 * i, -0.6, b, 15000); }
        for (int i = 0; i < 27; i++) { snprintf(b, sizeof b, "%.3f", 147.000 + 0.015 * i); addRpt(v, 147.000 + 0.015 * i, +0.6, b, 15000); }
        v.push_back(mk(0, "446.000", 446.000, Demod::Nfm, 25000, kCalling | kLicence, "70 cm national calling 446.000", "The FM simplex calling frequency of the USA and Canada."));
        for (int i = 0; i < 120; i++) { snprintf(b, sizeof b, "%.3f", 442.000 + 0.025 * i); addRpt(v, 442.000 + 0.025 * i, +5.0, b, 25000); }
    } else {   // Region 3 (Australia style): -0.6 MHz on 2 m, -5 MHz on 70 cm
        v.push_back(mk(0, "146.500", 146.500, Demod::Nfm, 12500, kCalling | kLicence, "2 m calling 146.500", "FM simplex calling channel (Australia)."));
        for (int i = 0; i < 15; i++) { snprintf(b, sizeof b, "%.3f", 146.625 + 0.025 * i); addRpt(v, 146.625 + 0.025 * i, -0.6, b, 12500); }
        v.push_back(mk(0, "438.500", 438.500, Demod::Nfm, 25000, kCalling | kLicence, "70 cm calling 438.500", "FM simplex calling channel (Australia)."));
        for (int i = 0; i < 79; i++) { snprintf(b, sizeof b, "%.3f", 438.025 + 0.025 * i); addRpt(v, 438.025 + 0.025 * i, -5.0, b, 25000); }
    }
    std::sort(v.begin(), v.end(), [](const Channel& a, const Channel& c) { return a.mhz < c.mhz; });
    for (size_t i = 0; i < v.size(); i++) v[i].number = (int)i + 1;
    return v;
}

Channel hf(const std::string& label, double kHz, const std::string& name, const std::string& note, unsigned fl, std::vector<int> minutes = {}) {
    Channel c = mk(0, label, kHz / 1000.0, Demod::Usb, 2700, fl, name, note);
    c.minutes = std::move(minutes);
    return c;
}

std::vector<Channel> buildHf() {
    std::vector<Channel> v;
    // HFGCS: the four primary voice frequencies, then the relays named with them
    v.push_back(hf("HFGCS 4724", 4724.0, "HFGCS 4724 kHz", "US Air Force High Frequency Global Communications System, primary voice frequency (USB). Mostly aircraft and Skyking / EAM broadcasts.", kContinuous));
    v.push_back(hf("HFGCS 8992", 8992.0, "HFGCS 8992 kHz", "HFGCS primary voice frequency (USB).", kContinuous));
    v.push_back(hf("HFGCS 11175", 11175.0, "HFGCS 11175 kHz", "HFGCS primary voice frequency (USB); the one heard most in Europe.", kContinuous));
    v.push_back(hf("HFGCS 15016", 15016.0, "HFGCS 15016 kHz", "HFGCS primary voice frequency (USB).", kContinuous));
    v.push_back(hf("MARS 13927", 13927.0, "Military Auxiliary Radio System 13927 kHz", "MARS HF station that US aircraft use to relay messages (USB).", 0));
    v.push_back(hf("CFHF 11232", 11232.0, "Canadian Forces HF 11232 kHz", "Canadian Forces HF station that aircraft use to relay messages (USB).", 0));
    // VOLMET: weather broadcasts for aircraft in flight (METAR / TAF / SIGMET), USB
    for (double k : {3413.0, 5505.0, 8957.0, 13264.0})
        v.push_back(hf("Shannon " + std::to_string((int)k), k, "Shannon VOLMET " + std::to_string((int)k) + " kHz", "Continuous weather broadcast for European airports. 3413 is the night frequency, 13264 the day one.", kScheduled | kContinuous));
    for (double k : {5450.0, 11253.0})
        v.push_back(hf("RAF " + std::to_string((int)k), k, "RAF Volmet (Military One) " + std::to_string((int)k) + " kHz", "Continuous weather broadcast of the Royal Air Force.", kScheduled | kContinuous));
    for (double k : {3485.0, 6604.0, 10051.0, 13270.0}) {
        v.push_back(hf("Gander " + std::to_string((int)k), k, "Gander VOLMET " + std::to_string((int)k) + " kHz", "North Atlantic weather broadcast, minutes 20 to 25 and 50 to 55 of every hour (5 minute slot of the shared family).", kScheduled, {20, 50}));
        v.push_back(hf("New York " + std::to_string((int)k), k, "New York VOLMET " + std::to_string((int)k) + " kHz", "North Atlantic weather broadcast, minutes 0 to 5 and 30 to 35 of every hour; reported out of service in 2025.", kScheduled, {0, 30}));
    }
    std::sort(v.begin(), v.end(), [](const Channel& a, const Channel& b) { return a.mhz < b.mhz || (a.mhz == b.mhz && a.label < b.label); });
    for (size_t i = 0; i < v.size(); i++) v[i].number = (int)i + 1;
    return v;
}

struct Cache {
    std::vector<Channel> cb[4], frs, pmr, ham[3], hf, none;
    Cache() { for (int i = 0; i < 4; i++) cb[i] = buildCb(i); frs = buildFrs(); pmr = buildPmr(); for (int i = 0; i < 3; i++) ham[i] = buildHam(i); hf = buildHf(); }
};
const Cache& cache() { static const Cache c; return c; }

} // namespace

const std::vector<Channel>& channels(Service s, int region) {
    const Cache& c = cache();
    switch (s) {
    case Service::Cb: return c.cb[std::max(0, std::min(3, region))];
    case Service::Frs: return c.frs;
    case Service::Pmr: return c.pmr;
    case Service::Ham: return c.ham[std::max(0, std::min(2, region))];
    case Service::HfVoice: return c.hf;
    default: return c.none;
    }
}

const Channel* findFrequency(Service s, int region, double mhz, double tolMhz, bool matchInput) {
    const Channel* best = nullptr; double bd = tolMhz;
    for (const Channel& c : channels(s, region)) {
        double d = std::fabs(c.mhz - mhz);
        if (matchInput && c.duplex()) d = std::min(d, std::fabs(c.txMhz - mhz));
        if (d <= bd) { bd = d; best = &c; }
    }
    return best;
}

const Channel* findLabel(Service s, int region, const std::string& text) {
    std::string t;
    for (char ch : text) if (!std::isspace((unsigned char)ch)) t += (char)std::tolower((unsigned char)ch);
    if (t.rfind("channel", 0) == 0) t = t.substr(7); else if (t.rfind("ch.", 0) == 0) t = t.substr(3); else if (t.rfind("ch", 0) == 0) t = t.substr(2);
    if (t.empty()) return nullptr;
    for (const Channel& c : channels(s, region)) {
        std::string l; for (char ch : c.label) if (!std::isspace((unsigned char)ch)) l += (char)std::tolower((unsigned char)ch);
        if (l == t) return &c;
    }
    return nullptr;
}

double rasterHz(Service s, int region) {
    switch (s) {
    case Service::Cb: return region == 2 ? 1250.0 * 8 : 10000.0;   // the 27/81 channels sit 1.25 kHz off the 10 kHz grid: the cell is 10 kHz wide either way
    case Service::Frs: return 12500.0;
    case Service::Pmr: return 6250.0;                                // 446.00625 + 12.5 kHz n: the channel centres are on a 6.25 kHz grid
    case Service::Ham: return region == 1 ? 5000.0 : 12500.0;
    case Service::Mdc: return 12500.0;
    case Service::Mic: return 25000.0;
    default: return 1000.0;
    }
}

Span planSpan(Service s, int region) {
    Span sp;
    if (s == Service::Mdc) { sp.loMhz = 30; sp.hiMhz = 960; sp.centreMhz = 155.0; return sp; }
    if (s == Service::Mic) { const auto& b = micBands(region); sp.loMhz = b.front().loMhz; sp.hiMhz = b.back().hiMhz; sp.centreMhz = b.front().loMhz + 4.0; return sp; }
    const auto& v = channels(s, region);
    if (v.empty()) return sp;
    sp.loMhz = 1e9; sp.hiMhz = 0;
    for (const Channel& c : v) { sp.loMhz = std::min(sp.loMhz, c.mhz); sp.hiMhz = std::max(sp.hiMhz, c.mhz); }
    sp.centreMhz = (sp.loMhz + sp.hiMhz) / 2;
    if (s == Service::Cb) sp.centreMhz = region == 1 ? 26.760 : 27.185;
    if (s == Service::Ham) sp.centreMhz = region == 0 ? 145.6 : region == 1 ? 146.76 : 146.8;
    if (s == Service::HfVoice) sp.centreMhz = 11.175;
    return sp;
}

const std::vector<MicBand>& micBands(int region) {
    static const std::vector<MicBand> eu = {
        {470.0, 694.0, "UHF 470 - 694 MHz", "TV band: the main band of professional systems, shared with broadcast; the free gaps between TV channels are used"},
        {823.0, 832.0, "823 - 832 MHz", "The duplex gap used by professional wireless microphones"},
        {863.0, 865.0, "863 - 865 MHz", "Licence-free band for wireless audio (radio microphones, in-ear monitors)"},
    };
    static const std::vector<MicBand> us = {
        {470.0, 608.0, "UHF 470 - 608 MHz", "TV channels 14 to 36: the main band of professional systems"},
        {614.0, 698.0, "UHF 614 - 698 MHz", "TV channels 38 to 51 where still used by wireless microphones"},
        {902.0, 928.0, "902 - 928 MHz", "ISM band used by low-cost wireless microphones"},
        {944.0, 952.0, "944 - 952 MHz", "Studio to transmitter and wireless microphone use"},
    };
    return region == 1 ? us : eu;
}

bool onAirAtMinute(const Channel& c, int m) {
    if (c.minutes.empty()) return true;
    m = ((m % 60) + 60) % 60;
    for (int s : c.minutes) if (((m - s + 60) % 60) < 5) return true;   // a 5 minute slot
    return false;
}
int nextStartMinute(const Channel& c, int m) {
    if (c.minutes.empty()) return -1;
    m = ((m % 60) + 60) % 60;
    int best = -1, bd = 99;
    for (int s : c.minutes) { const int d = (s - m + 60) % 60; if (d < bd) { bd = d; best = s; } }
    return best;
}

std::string toneLabel(double ctcssHz, int dcsCode, bool inv) {
    char b[24];
    if (ctcssHz > 0) { snprintf(b, sizeof b, "%.1f Hz", ctcssHz); return b; }
    if (dcsCode >= 0) return tones::dcsName(dcsCode, inv);
    return "";
}

int handsetToneNumber(double hz) {
    const int idx = tones::ctcssIndex(hz);
    if (idx < 0 || tones::ctcssExtended(idx)) return 0;
    int n = 0;
    for (int i = 0; i <= idx; i++) if (!tones::ctcssExtended(i)) n++;
    return n;
}

} // namespace chv
} // namespace dect2
