// The Ham station helpers (4.0): see ham_station.h.
#include "dect2/ham/ham_station.h"
#include "dect2/bandplan/bandplan.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_ops.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2::ham {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
}  // namespace

// ---- the mode catalog ----
const std::vector<HamModeInfo>& hamModeCatalog() {
    using R = HamRun;
    static const std::vector<HamModeInfo> c = {
        // weak signal: the slot engine of the Ham screen (auto sequence, auto log)
        {"ft8", "FT8", "Weak signal", R::Slot, 0, nullptr, nullptr, "FT8", "", "dB", true},
        {"ft4", "FT4", "Weak signal", R::Slot, 1, nullptr, nullptr, "MFSK", "FT4", "dB", true},
        {"ft2", "FT2", "Weak signal", R::Slot, 2, nullptr, nullptr, "MFSK", "FT2", "dB", true},
        {"q65", "Q65", "Weak signal", R::Slot, 6, nullptr, nullptr, "MFSK", "Q65", "dB", true},
        {"fst4", "FST4", "Weak signal", R::Slot, 7, nullptr, nullptr, "MFSK", "FST4", "dB", true},
        {"msk144", "MSK144", "Weak signal", R::Slot, 8, nullptr, nullptr, "MSK144", "", "dB", true},
        {"jt65", "JT65", "Weak signal", R::Slot, 4, nullptr, nullptr, "JT65", "", "dB", true},
        {"jt9", "JT9", "Weak signal", R::Slot, 5, nullptr, nullptr, "JT9", "", "dB", true},
        {"jt4", "JT4", "Weak signal", R::Slot, 10, nullptr, nullptr, "JT4", "", "dB", true},
        {"wspr", "WSPR beacon", "Weak signal", R::Slot, 3, nullptr, nullptr, "WSPR", "", "dB", true},
        {"js8", "JS8Call", "Weak signal", R::Screen, -1, nullptr, "JS8Call", "MFSK", "JS8", "dB", false},
        {"fsk441", "FSK441", "Weak signal", R::Screen, -1, nullptr, "FSK441", "FSK441", "", "RS", false},
        {"iscat", "ISCAT", "Weak signal", R::Screen, -1, nullptr, "ISCAT", "ISCAT", "", "dB", false},
        {"jt6m", "JT6M", "Weak signal", R::Screen, -1, nullptr, "JT6M", "JT6M", "", "RS", false},
        // keyboard modes: the keyboard panel of the Ham screen (type-ahead, macros, callsign pick, contest exchange)
        {"cw", "CW", "Keyboard", R::Keyboard, -1, "cw", nullptr, "CW", "", "RST", true},
        {"rtty", "RTTY", "Keyboard", R::Keyboard, -1, "rtty", nullptr, "RTTY", "", "RST", true},
        {"psk31", "PSK31", "Keyboard", R::Keyboard, -1, "psk31", nullptr, "PSK", "PSK31", "RST", true},
        {"psk63", "PSK63", "Keyboard", R::Keyboard, -1, "psk63", nullptr, "PSK", "PSK63", "RST", true},
        {"psk125", "PSK125", "Keyboard", R::Keyboard, -1, "psk125", nullptr, "PSK", "PSK125", "RST", true},
        {"olivia", "Olivia", "Keyboard", R::Keyboard, -1, "olivia-8-250", nullptr, "OLIVIA", "OLIVIA 8/250", "RST", true},
        {"contestia", "Contestia", "Keyboard", R::Keyboard, -1, "contestia-8-250", nullptr, "CONTESTI", "CONTESTI 8/250", "RST", true},
        {"mfsk", "MFSK", "Keyboard", R::Keyboard, -1, "mfsk16", nullptr, "MFSK", "MFSK16", "RST", true},
        {"thor", "THOR", "Keyboard", R::Keyboard, -1, "thor16", nullptr, "THOR", "THOR16", "RST", true},
        {"dominoex", "DominoEX", "Keyboard", R::Keyboard, -1, "dominoex11", nullptr, "DOMINO", "DOMINOEX", "RST", true},
        {"hell", "Hellschreiber", "Keyboard", R::Keyboard, -1, "feldhell", nullptr, "HELL", "FMHELL", "RST", true},
        {"mt63", "MT63", "Keyboard", R::Keyboard, -1, "mt63-1000l", nullptr, "MT63", "", "RST", true},
        {"fsq", "FSQ", "Keyboard", R::Keyboard, -1, "fsq3", nullptr, "MFSK", "FSQCALL", "RST", true},
        {"ifkp", "IFKP", "Keyboard", R::Keyboard, -1, "ifkp10", nullptr, "IFKP", "", "RST", true},
        // image
        {"sstv", "SSTV", "Image", R::Screen, -1, nullptr, "SSTV", "SSTV", "", "RS", false},
        // voice: the receiver screens (digital voice codecs are user-installed)
        {"ssb", "SSB (voice)", "Voice", R::Screen, -1, nullptr, "General receiver", "SSB", "", "RS", false},
        {"am", "AM (voice)", "Voice", R::Screen, -1, nullptr, "General receiver", "AM", "", "RS", false},
        {"fm", "FM (voice)", "Voice", R::Screen, -1, nullptr, "General receiver", "FM", "", "RS", false},
        {"freedv", "FreeDV", "Voice", R::Screen, -1, nullptr, "FreeDV", "DIGITALVOICE", "FREEDV", "RS", false},
        {"m17", "M17", "Voice", R::Screen, -1, nullptr, "M17", "DIGITALVOICE", "M17", "RS", false},
        {"dstar", "D-STAR", "Voice", R::Screen, -1, nullptr, "D-STAR", "DIGITALVOICE", "DSTAR", "RS", false},
        {"dmr", "DMR", "Voice", R::Screen, -1, nullptr, "DMR", "DIGITALVOICE", "DMR", "RS", false},
        {"c4fm", "C4FM (Yaesu Fusion)", "Voice", R::Screen, -1, nullptr, "Yaesu Fusion", "DIGITALVOICE", "C4FM", "RS", false},
        // packet
        {"aprs", "APRS (AFSK 1200)", "Packet", R::Screen, -1, nullptr, "APRS (AFSK)", "PKT", "", "RS", false},
        {"hfpacket", "HF packet 300 Bd", "Packet", R::Screen, -1, nullptr, "HF packet 300 Bd", "PKT", "", "RST", false},
        {"ardop", "ARDOP", "Packet", R::Screen, -1, nullptr, "ARDOP", "ARDOP", "", "RST", false},
    };
    return c;
}
const HamModeInfo* hamModeInfo(const std::string& id) {
    const std::string l = lower(id);
    for (const auto& m : hamModeCatalog()) if (l == m.id) return &m;
    return nullptr;
}
const HamModeInfo* hamModeBySlot(int slotMode) {
    for (const auto& m : hamModeCatalog()) if (m.run == HamRun::Slot && m.slotMode == slotMode) return &m;
    return nullptr;
}
const HamModeInfo* hamModeByKbd(const std::string& kbdId) {
    for (const auto& m : hamModeCatalog()) if (m.run == HamRun::Keyboard && m.kbdId && kbdId == m.kbdId) return &m;
    // a variant ("olivia-16-500", "thor22", "psk250"): the family by its prefix
    for (const auto& m : hamModeCatalog())
        if (m.run == HamRun::Keyboard && kbdId.rfind(m.id, 0) == 0 && std::string(m.id) != "psk31") return &m;
    if (kbdId.rfind("psk", 0) == 0 || kbdId.rfind("qpsk", 0) == 0) return hamModeInfo("psk31");
    if (kbdId.find("hell") != std::string::npos) return hamModeInfo("hell");
    return nullptr;
}
std::vector<std::string> hamModeGroups() {
    std::vector<std::string> g;
    for (const auto& m : hamModeCatalog()) if (std::find(g.begin(), g.end(), m.group) == g.end()) g.push_back(m.group);
    return g;
}
std::string defaultRst(const HamModeInfo& m) {
    const std::string s = m.rst;
    return s == "dB" ? "-10" : s == "RS" ? "59" : "599";
}

// ---- usual dial frequencies ----
double usualDialMhz(const std::string& band, const std::string& modeId, int region) {
    const std::string id = lower(modeId);
    const HamModeInfo* m = hamModeInfo(id);
    if (m && m->run == HamRun::Slot && id != "msk144") return dialFor(band, m->slotMode);
    struct D { const char* mode; const char* band; double r1, r2; };   // r2 0: the same as r1; region 3 uses r1 unless r2 is the only one
    static const D kD[] = {
        {"psk31", "160m", 1.838, 0}, {"psk31", "80m", 3.580, 0}, {"psk31", "40m", 7.040, 7.070}, {"psk31", "30m", 10.142, 0}, {"psk31", "20m", 14.070, 0},
        {"psk31", "17m", 18.100, 0}, {"psk31", "15m", 21.070, 0}, {"psk31", "12m", 24.920, 0}, {"psk31", "10m", 28.120, 0},
        {"rtty", "80m", 3.590, 3.580}, {"rtty", "40m", 7.043, 7.080}, {"rtty", "20m", 14.085, 0}, {"rtty", "15m", 21.085, 0}, {"rtty", "10m", 28.085, 0},
        // the QRP CW calling frequencies (IARU)
        {"cw", "160m", 1.836, 1.810}, {"cw", "80m", 3.560, 0}, {"cw", "40m", 7.030, 0}, {"cw", "30m", 10.116, 0}, {"cw", "20m", 14.060, 0},
        {"cw", "17m", 18.086, 0}, {"cw", "15m", 21.060, 0}, {"cw", "12m", 24.906, 0}, {"cw", "10m", 28.060, 0}, {"cw", "6m", 50.090, 0},
        {"js8", "160m", 1.842, 0}, {"js8", "80m", 3.578, 0}, {"js8", "40m", 7.078, 0}, {"js8", "30m", 10.130, 0}, {"js8", "20m", 14.078, 0},
        {"js8", "17m", 18.104, 0}, {"js8", "15m", 21.078, 0}, {"js8", "12m", 24.922, 0}, {"js8", "10m", 28.078, 0}, {"js8", "6m", 50.318, 0},
        {"sstv", "80m", 3.735, 3.845}, {"sstv", "40m", 7.165, 7.171}, {"sstv", "20m", 14.230, 0}, {"sstv", "15m", 21.340, 0}, {"sstv", "10m", 28.680, 0},
        {"msk144", "6m", 50.260, 0}, {"msk144", "2m", 144.360, 144.150},
    };
    for (const D& d : kD) {
        if (id != d.mode || band != d.band) continue;
        return region == 2 && d.r2 > 0 ? d.r2 : d.r1;
    }
    return 0;
}

// ---- the frequency memory ----
namespace { std::string fmKey(const std::string& b, const std::string& m) { return lower(b) + "|" + lower(m); } }
void FreqMemory::remember(const std::string& band, const std::string& modeId, double hz) {
    if (band.empty() || modeId.empty() || !(hz > 0) || !std::isfinite(hz)) return;
    m_[fmKey(band, modeId)] = hz;
}
double FreqMemory::recall(const std::string& band, const std::string& modeId, double fallbackHz) const {
    const auto it = m_.find(fmKey(band, modeId));
    return it == m_.end() ? fallbackHz : it->second;
}
bool FreqMemory::has(const std::string& band, const std::string& modeId) const { return m_.count(fmKey(band, modeId)) > 0; }
void FreqMemory::forget(const std::string& band, const std::string& modeId) { m_.erase(fmKey(band, modeId)); }
std::string FreqMemory::toText() const {
    std::string s;
    char b[32];
    for (const auto& kv : m_) {
        if (!s.empty()) s += ';';
        snprintf(b, sizeof b, "%.0f", kv.second);
        s += kv.first + "=" + b;
    }
    return s;
}
void FreqMemory::fromText(const std::string& s) {
    m_.clear();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        const size_t eq = item.find('='), bar = item.find('|');
        if (eq == std::string::npos || bar == std::string::npos || bar > eq || bar == 0 || bar + 1 == eq) continue;
        char* end = nullptr;
        const double hz = std::strtod(item.c_str() + eq + 1, &end);
        if (!(hz > 0) || !std::isfinite(hz)) continue;
        m_[lower(item.substr(0, eq))] = hz;
    }
}

// ---- band plan segments ----
const std::vector<BandSegment>& bandSegments(int region) {
    using U = SegUse;
    // IARU Region 1 HF band plan (simplified: the CW-only parts, the narrow-band parts, the beacon guard bands), and the VHF / UHF plans
    static const std::vector<BandSegment> r1 = {
        {1810000, 1838000, U::Cw, "CW"}, {1838000, 1840000, U::Narrow, "narrow-band modes"}, {1840000, 2000000, U::All, "all modes"},
        {3500000, 3570000, U::Cw, "CW"}, {3570000, 3600000, U::Narrow, "narrow-band modes"}, {3600000, 3800000, U::All, "all modes"},
        {5351500, 5354000, U::Cw, "CW"}, {5354000, 5366000, U::All, "all modes"}, {5366000, 5366500, U::Narrow, "weak signal narrow-band"},
        {7000000, 7040000, U::Cw, "CW"}, {7040000, 7050000, U::Narrow, "narrow-band modes"}, {7050000, 7200000, U::All, "all modes"},
        {10100000, 10130000, U::Cw, "CW"}, {10130000, 10150000, U::Narrow, "narrow-band modes"},
        {14000000, 14070000, U::Cw, "CW"}, {14070000, 14099000, U::Narrow, "narrow-band modes"}, {14099000, 14101000, U::Beacon, "IBP beacons"},
        {14101000, 14350000, U::All, "all modes"},
        {18068000, 18095000, U::Cw, "CW"}, {18095000, 18109000, U::Narrow, "narrow-band modes"}, {18109000, 18111000, U::Beacon, "IBP beacons"},
        {18111000, 18168000, U::All, "all modes"},
        {21000000, 21070000, U::Cw, "CW"}, {21070000, 21149000, U::Narrow, "narrow-band modes"}, {21149000, 21151000, U::Beacon, "IBP beacons"},
        {21151000, 21450000, U::All, "all modes"},
        {24890000, 24915000, U::Cw, "CW"}, {24915000, 24929000, U::Narrow, "narrow-band modes"}, {24929000, 24931000, U::Beacon, "IBP beacons"},
        {24931000, 24990000, U::All, "all modes"},
        {28000000, 28070000, U::Cw, "CW"}, {28070000, 28190000, U::Narrow, "narrow-band modes"}, {28190000, 28225000, U::Beacon, "beacons"},
        {28225000, 29300000, U::All, "all modes"}, {29300000, 29510000, U::Satellite, "satellite down-link"}, {29510000, 29700000, U::Fm, "FM, repeaters"},
        {50000000, 50100000, U::Cw, "CW and beacons"}, {50100000, 50400000, U::All, "SSB, CW and weak-signal digital"}, {50400000, 50500000, U::Beacon, "beacons"},
        {50500000, 52000000, U::All, "all modes"},
        {70000000, 70500000, U::All, "all modes"},
        {144000000, 144025000, U::Satellite, "satellite"}, {144025000, 144150000, U::Narrow, "CW, EME and weak-signal digital"},
        {144150000, 144400000, U::All, "SSB, CW, meteor scatter, digital"}, {144400000, 144491000, U::Beacon, "beacons"},
        {144491000, 144990000, U::All, "all modes (digital, APRS 144.800)"}, {144990000, 145800000, U::Fm, "FM, repeaters"},
        {145800000, 146000000, U::Satellite, "satellite"},
        {430000000, 432000000, U::All, "all modes"}, {432000000, 432025000, U::Narrow, "CW and EME"}, {432400000, 432490000, U::Beacon, "beacons"},
        {432025000, 432600000, U::All, "SSB, CW, digital"}, {432600000, 435000000, U::Fm, "FM, repeaters, digital voice"},
        {435000000, 438000000, U::Satellite, "satellite"}, {438000000, 440000000, U::Fm, "FM, repeaters, digital voice"},
    };
    // IARU Region 2 (the Americas), simplified the same way
    static const std::vector<BandSegment> r2 = {
        {1800000, 1840000, U::Narrow, "CW and narrow-band modes"}, {1840000, 2000000, U::All, "all modes"},
        {3500000, 3580000, U::Cw, "CW"}, {3580000, 3600000, U::Narrow, "narrow-band modes"}, {3600000, 4000000, U::All, "all modes"},
        {5351500, 5366500, U::All, "WRC-15 band (national channels differ)"},
        {7000000, 7040000, U::Cw, "CW"}, {7040000, 7050000, U::Narrow, "narrow-band modes"}, {7050000, 7300000, U::All, "all modes"},
        {10100000, 10130000, U::Cw, "CW"}, {10130000, 10150000, U::Narrow, "narrow-band modes"},
        {14000000, 14070000, U::Cw, "CW"}, {14070000, 14099000, U::Narrow, "narrow-band modes"}, {14099000, 14101000, U::Beacon, "IBP beacons"},
        {14101000, 14350000, U::All, "all modes"},
        {18068000, 18095000, U::Cw, "CW"}, {18095000, 18109000, U::Narrow, "narrow-band modes"}, {18109000, 18111000, U::Beacon, "IBP beacons"},
        {18111000, 18168000, U::All, "all modes"},
        {21000000, 21070000, U::Cw, "CW"}, {21070000, 21149000, U::Narrow, "narrow-band modes"}, {21149000, 21151000, U::Beacon, "IBP beacons"},
        {21151000, 21450000, U::All, "all modes"},
        {24890000, 24915000, U::Cw, "CW"}, {24915000, 24929000, U::Narrow, "narrow-band modes"}, {24929000, 24931000, U::Beacon, "IBP beacons"},
        {24931000, 24990000, U::All, "all modes"},
        {28000000, 28070000, U::Cw, "CW"}, {28070000, 28190000, U::Narrow, "narrow-band modes"}, {28190000, 28225000, U::Beacon, "beacons"},
        {28225000, 29300000, U::All, "all modes"}, {29300000, 29510000, U::Satellite, "satellite down-link"}, {29510000, 29700000, U::Fm, "FM, repeaters"},
        {50000000, 50100000, U::Cw, "CW and beacons"}, {50100000, 54000000, U::All, "all modes"},
        {144000000, 144100000, U::Narrow, "CW, EME"}, {144100000, 144275000, U::All, "SSB, CW, weak-signal digital"},
        {144275000, 144300000, U::Beacon, "beacons"}, {144300000, 145800000, U::All, "all modes"}, {145800000, 146000000, U::Satellite, "satellite"},
        {146000000, 148000000, U::Fm, "FM, repeaters"},
        {420000000, 432000000, U::All, "all modes"}, {432000000, 432070000, U::Narrow, "CW and EME"}, {432300000, 432400000, U::Beacon, "beacons"},
        {432070000, 435000000, U::All, "SSB, CW, digital"}, {435000000, 438000000, U::Satellite, "satellite"}, {438000000, 450000000, U::Fm, "FM, repeaters"},
    };
    return region == 2 ? r2 : r1;   // region 3: the region 1 table (its HF plan is close; 80 m and 40 m are wider there)
}
const BandSegment* segmentAt(int region, double hz) {
    for (const auto& s : bandSegments(region)) if (hz >= s.loHz && hz < s.hiHz) return &s;
    return nullptr;
}
const char* segUseName(SegUse u) {
    switch (u) {
    case SegUse::Cw: return "CW";
    case SegUse::Narrow: return "narrow-band";
    case SegUse::All: return "all modes";
    case SegUse::Beacon: return "beacons";
    case SegUse::Satellite: return "satellite";
    case SegUse::Fm: return "FM";
    }
    return "";
}
std::string segmentWarning(int region, double hz, const std::string& modeClass) {
    const BandSegment* s = segmentAt(region, hz);
    if (!s) return "";
    const std::string c = upper(modeClass);
    char f[32]; snprintf(f, sizeof f, "%.4f MHz", hz / 1e6);
    const int r = region == 2 || region == 3 ? region : 1;
    char rg[48]; snprintf(rg, sizeof rg, " (simplified IARU Region %d plan)", r);
    switch (s->use) {
    case SegUse::Cw:
        if (c != "CW") return std::string(f) + " is in the CW part of the band plan" + rg + ": other modes are not expected here.";
        break;
    case SegUse::Narrow:
        if (c == "PHONE") return std::string(f) + " is in the narrow-band part of the band plan" + rg + ": phone is not expected here.";
        break;
    case SegUse::Beacon: return std::string(f) + " is a beacon segment" + rg + ": please keep it clear.";
    case SegUse::Satellite: return std::string(f) + " is a satellite segment" + rg + ": transmit here only to work a satellite.";
    case SegUse::Fm:
        if (c == "CW") return std::string(f) + " is in the FM and repeater part of the band plan" + rg + ".";
        break;
    case SegUse::All: break;
    }
    return "";
}

// ---- the S-meter ----
double sUnits(double dbm, double hz) {
    const double s9 = hz < 30e6 ? -73.0 : -93.0;
    return 9.0 + (dbm - s9) / 6.0;
}
std::string sMeterText(double dbm, double hz) {
    const double s9 = hz < 30e6 ? -73.0 : -93.0;
    char b[16];
    const double over = dbm - s9;
    if (over >= 5) { snprintf(b, sizeof b, "S9+%d", (int)std::lround(over / 10.0) * 10); return b; }
    const long s = std::lround(sUnits(dbm, hz));
    snprintf(b, sizeof b, "S%ld", std::max(0L, std::min(9L, s)));
    return b;
}

// ---- region ----
int regionForCountry(const std::string& country) {
    const std::string u = upper(trim(country)), l = lower(trim(country));
    if (u.empty()) return 0;
    for (const auto& p : bandplan::builtinPlans()) {
        if (p.ituRegion < 1 || p.ituRegion > 3) continue;
        if (p.id == l) return p.ituRegion;
        for (const auto& c : p.countries) if (upper(c) == u) return p.ituRegion;
    }
    return 0;
}

// ---- the setup checklist ----
std::vector<std::string> setupHints(const StationSetup& s) {
    std::vector<std::string> h;
    if (!hamCallValid(s.call)) h.push_back("Enter your callsign: it goes into every message and every log entry.");
    if (!hamGridValid(s.grid)) h.push_back("Enter your locator (4 or 6 characters), or take it from your position.");
    if (!s.licence) h.push_back("Licence not confirmed: OnAir still transmits when you arm it; you are responsible.");
    if (trim(s.country).empty()) h.push_back("Choose your country, so the band plan warnings fit where you are.");
    if (s.region < 1 || s.region > 3) h.push_back("Choose your IARU region (it sets the band plan hints).");
    if (!s.rig) h.push_back("No rig chosen: VOX keying (the radio keys itself from the audio).");
    if (!s.audioOut) h.push_back("No audio output chosen: the system default is used.");
    if (trim(s.power).empty()) h.push_back("Your power is not set (optional: logged as TX_PWR).");
    if (s.antennas.empty()) h.push_back("No antenna listed (optional: it goes into the log and PSK Reporter).");
    return h;
}

// ---- CW speed matching ----
void CwSpeedMatch::heard(double w) {
    if (!(w > 0) || !std::isfinite(w) || w > 99) return;
    if (n_ >= 3 && (w < est_ * 0.5 || w > est_ * 2.0)) return;   // a misread element
    est_ = n_ == 0 ? w : est_ + 0.35 * (w - est_);
    n_++;
}
int CwSpeedMatch::wpm(int currentWpm) const {
    if (!ready()) return currentWpm;
    return (int)std::max<long>(minWpm, std::min<long>(maxWpm, std::lround(est_)));
}

// ---- the band-hop schedule ----
const HopEntry* HopSchedule::at(double utc) const {
    const double d = std::fmod(utc, 86400.0);
    const int m = (int)std::floor((d < 0 ? d + 86400.0 : d) / 60.0);
    for (const auto& e : entries) {
        const bool in = e.startMin == e.endMin ? true : e.startMin < e.endMin ? (m >= e.startMin && m < e.endMin) : (m >= e.startMin || m < e.endMin);
        if (in) return &e;
    }
    return nullptr;
}
bool HopSchedule::parse(const std::string& text, HopSchedule& out, std::string& err) {
    out.entries.clear();
    std::stringstream ss(text);
    std::string line;
    int n = 0;
    auto hhmm = [](const std::string& t, int& mins) {
        if (t.size() != 4 || !std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; })) return false;
        const int h = std::atoi(t.substr(0, 2).c_str()), m = std::atoi(t.substr(2).c_str());
        if (m > 59 || h > 24 || (h == 24 && m)) return false;
        mins = h * 60 + m;
        return true;
    };
    while (std::getline(ss, line)) {
        n++;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        std::stringstream ls(line);
        std::string range, band, mode;
        ls >> range >> band >> mode;
        const size_t dash = range.find('-');
        HopEntry e;
        if (dash == std::string::npos || !hhmm(range.substr(0, dash), e.startMin) || !hhmm(range.substr(dash + 1), e.endMin)) {
            err = "line " + std::to_string(n) + ": the time range should read like 0600-1800";
            return false;
        }
        if (e.startMin == 1440) e.startMin = 0;
        if (e.endMin == 1440) e.endMin = 0;
        band = lower(band);
        const bool bandOk = band == "auto" || (band.size() >= 2 && std::isdigit((unsigned char)band[0]) && band.back() == 'm');
        if (!bandOk) { err = "line " + std::to_string(n) + ": the band should read like 20m, 70cm or auto"; return false; }
        e.band = band; e.modeId = lower(mode);
        out.entries.push_back(e);
    }
    err.clear();
    return true;
}
std::string HopSchedule::toText() const {
    std::string s;
    char b[96];
    for (const auto& e : entries) {
        snprintf(b, sizeof b, "%02d%02d-%02d%02d %s%s%s\n", e.startMin / 60, e.startMin % 60, e.endMin == 0 && e.startMin != 0 ? 24 : e.endMin / 60,
                 e.endMin == 0 && e.startMin != 0 ? 0 : e.endMin % 60, e.band.c_str(), e.modeId.empty() ? "" : " ", e.modeId.c_str());
        s += b;
    }
    return s;
}

// ---- split ----
SplitTx fakeItSplit(double tx, double lo, double hi) {
    SplitTx s;
    const double step = hi > lo ? hi - lo : 500.0;
    if (tx >= lo && tx < lo + step) { s.audioHz = tx; return s; }
    s.dialShiftHz = std::floor((tx - lo) / step) * step;
    s.audioHz = tx - s.dialShiftHz;
    return s;
}

}  // namespace dect2::ham
