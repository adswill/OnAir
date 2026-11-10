// Telemetry parsing: see cubesat_telem.h.
#include "dect2/cubesat/cubesat_telem.h"
#include "dect2/cubesat/cubesat_proto.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace dect2 {
namespace cubesat {

namespace {

struct BitReader {
    const std::vector<uint8_t>& d;
    size_t pos = 0;   // bit position
    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, pos++) {
            const size_t by = pos >> 3;
            const int bit = by < d.size() ? (d[by] >> (7 - (pos & 7))) & 1 : 0;
            v = (v << 1) | (uint32_t)bit;
        }
        return v;
    }
    bool ok() const { return pos <= d.size() * 8; }
};

std::string fmt(const char* f, double v) { char b[64]; snprintf(b, sizeof b, f, v); return b; }

struct Out {
    TelResult& r;
    std::string group;
    void num(const std::string& name, double v, const char* unit = "", const char* f = "%.2f") {
        TelField t; t.group = group; t.name = name; t.num = v; t.numeric = true;
        t.text = fmt(f, v) + (unit[0] ? std::string(" ") + unit : std::string());
        r.fields.push_back(t);
    }
    void raw(const std::string& name, uint32_t v, const char* unit = "") { num(name, (double)v, unit, "%.0f"); }
    void flag(const std::string& name, bool v, const char* yes = "yes", const char* no = "no") {
        TelField t; t.group = group; t.name = name; t.num = v ? 1 : 0; t.numeric = true; t.text = v ? yes : no;
        r.fields.push_back(t);
    }
};

double pwrMw(uint32_t x) { return 5e-3 * std::pow((double)x, 2.0629); }

void rf(BitReader& b, Out& o) {
    o.group = "RF";
    o.raw("RX Doppler", b.u(8));
    o.raw("RX RSSI", b.u(8));
    o.num("RF temperature", -0.857 * b.u(8) + 193.672, "degC", "%.1f");
    o.num("RX current", 0.636 * b.u(8), "mA", "%.1f");
    o.num("TX 3.3 V current", 0.636 * b.u(8), "mA", "%.1f");
    o.num("TX 5 V current", 1.272 * b.u(8), "mA", "%.1f");
}

void pa(BitReader& b, Out& o) {
    o.group = "PA";
    o.num("Reverse power", pwrMw(b.u(8)), "mW", "%.1f");
    o.num("Forward power", pwrMw(b.u(8)), "mW", "%.1f");
    o.raw("Board temperature", b.u(8));
    o.num("Board current", 0.5496 * b.u(8) + 2.5435, "mA", "%.1f");
}

void sw(BitReader& b, Out& o, bool full) {
    o.group = "Software";
    o.raw("Sequence number", b.u(24));
    o.raw("DTMF command count", b.u(6));
    o.raw("DTMF last command", b.u(5));
    o.flag("DTMF command success", b.u(1) != 0);
    if (!full) return;
    static const char* const kValid[7] = {"data valid 1", "data valid 2", "data valid 3", "data valid 4", "data valid 5", "data valid 6", "data valid 7"};
    for (int i = 0; i < 7; i++) o.flag(kValid[i], b.u(1) != 0);
    o.flag("Eclipse", b.u(1) != 0);
    o.flag("Safe mode", b.u(1) != 0);
    o.flag("Hardware ABF", b.u(1) != 0);
    o.flag("Software ABF", b.u(1) != 0);
    o.flag("Deployment wait", b.u(1) != 0);
}

double tXp(uint32_t x) { return -0.2073 * x + 158.239; }
double tXm(uint32_t x) { return -0.2083 * x + 159.227; }
double tYp(uint32_t x) { return -0.2076 * x + 158.656; }
double tYm(uint32_t x) { return -0.2087 * x + 159.045; }

void ants(BitReader& b, Out& o) {
    o.group = "Antennas";
    o.raw("Antenna temperature 0", b.u(8));
    o.raw("Antenna temperature 1", b.u(8));
    for (int i = 0; i < 4; i++) o.flag("Antenna " + std::to_string(i + 1) + " deployed", b.u(1) != 0, "yes", "no");
}

} // namespace

bool parseFuncube(const std::vector<uint8_t>& f, TelResult& r) {
    if (f.size() != 256) return false;
    BitReader b{f};
    const uint32_t satid = b.u(2), type = b.u(6);
    static const char* const kSat[4] = {"FUNcube-1 EM", "FUNcube-2", "FUNcube-1 (AO-73)", "extended"};
    uint32_t ext = 0;
    if (satid == 3) ext = b.u(8);
    if (satid == 3 && ext != 0x08) return false;           // only Nayif-1 is known
    if (type > 23) return false;
    static const char* const kFt[3] = {"WO", "HR", "FM"};
    // frame type names: FUNcube: WO1-12 (0-11), HR1 (12), FM1-3 (13-15), HR2 (16), FM4-6 (17-19), HR3 (20), FM7-9 (21-23); Nayif-1: WO 0-11, HR 12-16, FM 17-23
    std::string ft;
    if (satid != 3) {
        static const int kIdx[24][2] = {{0,1},{0,2},{0,3},{0,4},{0,5},{0,6},{0,7},{0,8},{0,9},{0,10},{0,11},{0,12},{1,1},{2,1},{2,2},{2,3},{1,2},{2,4},{2,5},{2,6},{1,3},{2,7},{2,8},{2,9}};
        ft = std::string(kFt[kIdx[type][0]]) + std::to_string(kIdx[type][1]);
    } else {
        if (type <= 11) ft = "WO" + std::to_string(type + 1);
        else if (type <= 16) ft = "HR" + std::to_string(type - 11);
        else ft = "FM" + std::to_string(type - 16);
    }
    r.satellite = kSat[satid];
    r.kind = "FUNcube realtime, frame " + ft;
    Out o{r, ""};
    o.group = "Frame";
    o.flag("Frame type", true, ft.c_str(), ft.c_str());
    if (satid == 0 || satid == 2) {           // FC-1
        o.group = "EPS";
        for (int i = 0; i < 3; i++) o.raw("Solar panel voltage " + std::to_string(i + 1), b.u(16), "mV");
        o.raw("Photo current", b.u(16), "mA");
        o.raw("Battery voltage", b.u(16), "mV");
        o.raw("System current", b.u(16), "mA");
        o.raw("Reboot count", b.u(16));
        o.raw("Software errors", b.u(16));
        for (int i = 0; i < 3; i++) o.raw("Boost converter temperature " + std::to_string(i + 1), b.u(8), "degC");
        o.raw("Battery temperature", b.u(8), "degC");
        o.raw("Latch-up count 5 V", b.u(8));
        o.raw("Latch-up count 3.3 V", b.u(8));
        o.raw("Reset cause", b.u(8));
        o.raw("MPPT mode", b.u(8));
        o.group = "BOB";
        for (int i = 0; i < 3; i++) o.raw("Sun sensor " + std::to_string(i + 1), b.u(10));
        o.num("Panel temperature X+", tXp(b.u(10)), "degC", "%.1f");
        o.num("Panel temperature X-", tXm(b.u(10)), "degC", "%.1f");
        o.num("Panel temperature Y+", tYp(b.u(10)), "degC", "%.1f");
        o.num("Panel temperature Y-", tYm(b.u(10)), "degC", "%.1f");
        o.num("3.3 V bus", 4.0 * b.u(10), "mV", "%.0f");
        o.raw("3.3 V current", b.u(10));
        o.num("5 V bus", 6.0 * b.u(10), "mV", "%.0f");
        rf(b, o); pa(b, o); ants(b, o); sw(b, o, true);
    } else if (satid == 1) {                  // FC-2
        o.group = "EPS";
        o.flag("Sunlight", b.u(1) != 0);
        for (int i = 0; i < 12; i++) o.raw("Solar current " + std::to_string(i + 1), b.u(10));
        o.raw("Solar temperature", b.u(8), "degC");
        for (int i = 0; i < 3; i++) {
            const std::string n = "Battery " + std::to_string(i);
            o.flag(n + " direction", b.u(1) != 0, "charging", "discharging");
            o.raw(n + " current", b.u(8));
            o.raw(n + " voltage", b.u(8));
            o.raw(n + " temperature", b.u(8));
        }
        o.flag("Battery heater", b.u(1) != 0, "on", "off");
        o.group = "Antennas";
        o.raw("Deployment timeout", b.u(8));
        o.raw("Deployment status", b.u(12));
        o.raw("Antenna temperature", b.u(8));
        rf(b, o); pa(b, o);
        o.group = "Platform";
        o.raw("AMAC mode", b.u(3));
        for (int i = 0; i < 3; i++) o.raw(std::string("Magnetometer ") + "XYZ"[i], b.u(12));
        o.flag("FUNtrx enabled", b.u(1) != 0);
        o.flag("FUNtrx sampling", b.u(1) != 0);
        o.raw("Mode manager mode", b.u(3));
        o.flag("Comms nominal", b.u(1) != 0);
        o.raw("Comms state", b.u(2));
        b.u(1 + 1 + 3 + 3);
        o.raw("OBC soft reset count", b.u(8));
        o.raw("EPS hard reset count", b.u(8));
        b.u(20);
        sw(b, o, false);
    } else {                                  // Nayif-1
        o.group = "EPS";
        for (int i = 0; i < 3; i++) o.raw("Solar panel voltage " + std::to_string(i + 1), b.u(14), "mV");
        o.raw("Battery voltage", b.u(14), "mV");
        for (int i = 0; i < 3; i++) o.raw("Photo current " + std::to_string(i + 1), b.u(10), "mA");
        o.raw("Total photo current", b.u(10), "mA");
        o.raw("System current", b.u(10), "mA");
        o.raw("Reboot count", b.u(8));
        for (int i = 0; i < 3; i++) o.raw("Boost converter temperature " + std::to_string(i + 1), b.u(8), "degC");
        o.raw("Battery temperature", b.u(8), "degC");
        o.raw("Latch-up count 5 V", b.u(8));
        o.raw("5 V channel current", b.u(8));
        o.raw("Reset cause", b.u(4));
        o.raw("MPPT mode", b.u(4));
        o.group = "iMTQ";
        o.raw("Mode (0 idle, 1 self test, 2 detumble)", b.u(2));
        o.raw("Error code", b.u(3));
        o.flag("Configuration set", b.u(1) != 0);
        o.raw("MCU temperature", b.u(8), "degC");
        o.group = "ASIB";
        for (int i = 0; i < 6; i++) o.raw("Sun sensor " + std::to_string(i + 1), b.u(10));
        o.num("3.3 V bus", 4.0 * b.u(10), "mV", "%.0f");
        o.raw("iMTQ uptime", b.u(20), "s");
        o.num("5 V bus", 6.0 * b.u(10), "mV", "%.0f");
        rf(b, o); pa(b, o); ants(b, o); sw(b, o, true);
    }
    return b.ok();
}

std::string beaconText(const std::vector<uint8_t>& d) {
    // the longest run of printable ASCII (space .. ~, tab, CR, LF) with at least 12 characters of which most are letters, digits or spaces
    size_t bestS = 0, bestN = 0, s = 0;
    for (size_t i = 0; i <= d.size(); i++) {
        const bool pr = i < d.size() && ((d[i] >= 32 && d[i] < 127) || d[i] == '\r' || d[i] == '\n' || d[i] == '\t');
        if (pr) continue;
        if (i - s > bestN) { bestN = i - s; bestS = s; }
        s = i + 1;
    }
    if (bestN < 12) return "";
    size_t alnum = 0;
    std::string t;
    for (size_t i = 0; i < bestN; i++) {
        const char c = (char)d[bestS + i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ') alnum++;
        t += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    return alnum * 10 >= bestN * 6 ? t : std::string();
}

// GOMX-1 beacon A after its CSP header and time: layout from gr-satellites python/telemetry/gomx_1.py (Daniel Estevez EA4GPZ, GPL-3.0-or-later)
bool parseGomx1(const std::vector<uint8_t>& d, TelResult& out) {
    if (d.size() < 9 + 18 + 22 + 63 + 40 + 9 + 55) return false;   // CSP, time and flags, then OBC, COM, EPS, GATOSS, Hub, ADCS
    BitReader b{d};
    out.kind = "GOMX-1 beacon A";
    out.satellite = "GOMX 1";
    Out o{out, "Beacon"};
    b.pos = 32;   // past the CSP header
    o.raw("Time", b.u(32), "s");
    o.raw("Flags", b.u(8));
    const auto s16 = [](uint32_t v) { return (int)(int16_t)(uint16_t)v; };
    o.group = "OBC";
    o.raw("Boot count", b.u(16));
    for (int i = 0; i < 2; i++) o.num("Temperature " + std::to_string(i + 1), s16(b.u(16)) / 4.0, "degC", "%.2f");
    for (int i = 0; i < 6; i++) o.num("Panel temperature " + std::to_string(i + 1), s16(b.u(16)) / 4.0, "degC", "%.2f");
    o.group = "COM";
    o.raw("Bytes corrected", b.u(16)); o.raw("RX packets", b.u(16)); o.raw("RX errors", b.u(16)); o.raw("TX packets", b.u(16));
    for (int i = 0; i < 2; i++) o.num("Temperature " + std::to_string(i + 1), (double)s16(b.u(16)), "degC", "%.0f");
    o.num("RSSI", (double)s16(b.u(16)), "dBm", "%.0f");
    o.num("RF error", (double)s16(b.u(16)), "Hz", "%.0f");
    o.num("Battery", b.u(16) / 100.0, "V", "%.2f");
    o.num("TX current", (double)b.u(16), "mA", "%.0f");
    o.raw("Boot count", b.u(16));
    o.group = "EPS";
    for (int i = 0; i < 3; i++) o.num("Boost voltage " + std::to_string(i + 1), b.u(16) / 1000.0, "V", "%.3f");
    o.num("Battery voltage", b.u(16) / 1000.0, "V", "%.3f");
    for (int i = 0; i < 6; i++) o.num("Output current " + std::to_string(i + 1), (double)b.u(16), "mA", "%.0f");
    for (int i = 0; i < 3; i++) o.num("Input current " + std::to_string(i + 1), (double)b.u(16), "mA", "%.0f");
    o.num("Boost current", (double)b.u(16), "mA", "%.0f");
    o.num("Battery current", (double)b.u(16), "mA", "%.0f");
    for (int i = 0; i < 6; i++) o.num("Temperature " + std::to_string(i + 1), (double)s16(b.u(16)), "degC", "%.0f");
    return b.ok();
}

bool parseTelemetry(int framing, const std::vector<uint8_t>& data, const std::vector<uint8_t>& payload, TelResult& out, int norad) {
    out = TelResult();
    if (framing == kFramingAo40 && parseFuncube(data, out)) return true;
    if (norad == 39430 && parseGomx1(payload.empty() ? data : payload, out)) return true;
    const std::string t = beaconText(payload.empty() ? data : payload);
    if (!t.empty()) {
        out.kind = "text beacon";
        TelField f; f.name = "Text"; f.text = t;
        out.fields.push_back(f);
        return true;
    }
    return false;
}

} // namespace cubesat
} // namespace dect2
