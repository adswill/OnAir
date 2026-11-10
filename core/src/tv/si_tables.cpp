#include <map>
#include <cmath>
// DVB service information beyond the multiplex being received (see si_tables.h).
#include "dect2/tv/si_tables.h"
#include "dect2/tv/ts.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace dect2 {

namespace {
int bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
// n BCD digits from the start of p
double bcdDigits(const uint8_t* p, int digits) {
    double v = 0;
    for (int i = 0; i < digits; i++) v = v * 10 + ((i & 1) ? (p[i / 2] & 15) : (p[i / 2] >> 4));
    return v;
}
// 40 bits: MJD (16) and hh mm ss BCD -> unix seconds
int64_t mjdUtc(const uint8_t* p) {
    const int mjd = p[0] << 8 | p[1];
    if (mjd == 0xFFFF) return 0;
    return ((int64_t)mjd - 40587) * 86400 + bcd(p[2]) * 3600 + bcd(p[3]) * 60 + bcd(p[4]);
}
}

const char* siPolName(int p) { static const char* n[4] = {"horizontal", "vertical", "left circular", "right circular"}; return p >= 0 && p < 4 ? n[p] : "?"; }
const char* siFecName(int f) {
    static const char* n[16] = {"not defined", "1/2", "2/3", "3/4", "5/6", "7/8", "8/9", "3/5", "4/5", "9/10", "reserved", "reserved", "reserved", "reserved", "reserved", "none"};
    return f >= 0 && f < 16 ? n[f] : "?";
}
const char* siModName(int kind, int m) {
    if (kind == 1 || kind == 5) { static const char* n[4] = {"auto", "QPSK", "8PSK", "16QAM"}; return m >= 0 && m < 4 ? n[m] : "?"; }
    if (kind == 2) { static const char* n[6] = {"not defined", "16QAM", "32QAM", "64QAM", "128QAM", "256QAM"}; return m >= 0 && m < 6 ? n[m] : "?"; }
    return "";
}
const char* siCaSystemName(int id) {
    const int f = id & 0xFF00;
    switch (f) {
    case 0x0100: return "SECA / Mediaguard";
    case 0x0200: return "CCETT";
    case 0x0300: return "Deutsche Telekom";
    case 0x0400: return "Eurodec";
    case 0x0500: return "Viaccess";
    case 0x0600: return "Irdeto";
    case 0x0700: return "Jerrold / General Instrument";
    case 0x0900: return "NDS / Videoguard";
    case 0x0A00: return "Nokia";
    case 0x0B00: return "Conax";
    case 0x0C00: return "NTL";
    case 0x0D00: return "Cryptoworks";
    case 0x0E00: return "Power VU";
    case 0x1000: return "RAS";
    case 0x1200: return "NetGem";
    case 0x1700: return "BetaCrypt";
    case 0x1800: return "Nagravision";
    case 0x2600: return "BISS";
    case 0x4A00: return "DigiCipher";
    case 0x4AE0: return "DRE-Crypt";
    case 0x4AE1: return "DRE-Crypt";
    case 0x5500: return "Latens";
    default: return "";
    }
}

static void descriptors(const uint8_t* d, int len, const std::function<void(int tag, const uint8_t* p, int n)>& fn) {
    int i = 0;
    while (i + 2 <= len) {
        const int tag = d[i], n = d[i + 1];
        if (i + 2 + n > len) break;
        fn(tag, d + i + 2, n);
        i += 2 + n;
    }
}

void SiCollector::section(int, const uint8_t* s, int len) {
    if (len < 8) return;
    sections_++;
    switch (s[0]) {
    case 0x01: cat(s, len); break;
    case 0x40: case 0x41: nit(s, len); break;
    case 0x42: case 0x46: sdt(s, len); break;
    case 0x4A: bat(s, len); break;
    case 0x4F: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: eit(s, len); break;
    case 0x73: tot(s, len); break;
    default: break;
    }
}

static void parseTransportDescriptors(const uint8_t* d, int len, SiTransponder& t) {
    descriptors(d, len, [&](int tag, const uint8_t* p, int n) {
        if (tag == 0x43 && n >= 11) {          // satellite delivery system
            t.kind = 1;
            t.freqMhz = bcdDigits(p, 8) / 100.0;                // GHz xxx.xxxxx -> MHz (units of 10 kHz)
            t.orbitDeg = bcdDigits(p + 4, 4) / 10.0;
            const bool east = (p[6] & 0x80) != 0;
            if (!east) t.orbitDeg = -t.orbitDeg;
            t.pol = (p[6] >> 5) & 3;
            t.s2 = (p[6] & 0x04) != 0;
            t.rollOff = t.s2 ? (p[6] >> 3) & 3 : -1;
            t.modulation = p[6] & 3;
            t.symbolRateK = bcdDigits(p + 7, 7) / 10.0;          // 28 bits: 7 BCD digits, units of 100 sym/s
            t.fecInner = p[10] & 15;
        } else if (tag == 0x44 && n >= 11) {   // cable delivery system
            t.kind = 2;
            t.freqMhz = bcdDigits(p, 8) / 10000.0;
            t.modulation = p[6];
            t.symbolRateK = bcdDigits(p + 7, 7) / 10.0;
            t.fecInner = p[10] & 15;
        } else if (tag == 0x5A && n >= 11) {   // terrestrial delivery system
            t.kind = 3;
            const uint32_t f = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
            t.freqMhz = f * 10.0 / 1e6;
            static const double bw[4] = {8, 7, 6, 5};
            t.bandwidthMhz = bw[(p[4] >> 5) & 3];
        } else if (tag == 0x7F && n >= 12 && p[0] == 0x04) {   // extension: T2 delivery system
            t.kind = 4;
            static const double bw[16] = {8, 7, 6, 5, 10, 1.712, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            t.bandwidthMhz = bw[(p[4] >> 2) & 15];
            const bool tfs = (p[5] & 1) != 0;
            const int o = tfs ? 9 : 8;
            if (n >= o + 4) { const uint32_t f = (uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | p[o + 3]; t.freqMhz = f * 10.0 / 1e6; }
        } else if (tag == 0x7F && n >= 2 && p[0] == 0x17) {   // extension: S2X satellite delivery system (named, not read in detail)
            t.kind = 5; t.s2 = true;
        } else if (tag == 0x41) {              // service list
            for (int k = 0; k + 3 <= n; k += 3) t.services.push_back(p[k] << 8 | p[k + 1]);
        }
    });
}

void SiCollector::nit(const uint8_t* s, int len) {
    const bool actual = s[0] == 0x40;
    const int nid = s[3] << 8 | s[4];
    if (len < 16) return;
    const int ndl = ((s[8] & 0x0F) << 8) | s[9];
    if (10 + ndl + 2 > len) return;
    SiNetwork& net = networks_[nid];
    net.id = nid; net.actual = net.actual || actual;
    descriptors(s + 10, ndl, [&](int tag, const uint8_t* p, int n) { if (tag == 0x40) net.name = dvbText(p, n); });
    const int tll = ((s[10 + ndl] & 0x0F) << 8) | s[11 + ndl];
    int i = 12 + ndl;
    const int end = std::min(len - 4, i + tll);
    while (i + 6 <= end) {
        SiTransponder t;
        t.tsid = s[i] << 8 | s[i + 1];
        t.onid = s[i + 2] << 8 | s[i + 3];
        const int dl = ((s[i + 4] & 0x0F) << 8) | s[i + 5];
        i += 6;
        if (i + dl > end) break;
        parseTransportDescriptors(s + i, dl, t);
        i += dl;
        bool replaced = false;
        for (auto& e : net.transponders) if (e.tsid == t.tsid && e.onid == t.onid && e.freqMhz == t.freqMhz) { e = t; replaced = true; break; }
        if (!replaced && net.transponders.size() < 4096) net.transponders.push_back(t);
    }
}

void SiCollector::bat(const uint8_t* s, int len) {
    if (len < 16) return;
    const int id = s[3] << 8 | s[4];
    const int bdl = ((s[8] & 0x0F) << 8) | s[9];
    if (10 + bdl + 2 > len) return;
    SiBouquet& b = bouquets_[id];
    b.id = id;
    descriptors(s + 10, bdl, [&](int tag, const uint8_t* p, int n) { if (tag == 0x47) b.name = dvbText(p, n); });
    const int tll = ((s[10 + bdl] & 0x0F) << 8) | s[11 + bdl];
    int i = 12 + bdl;
    const int end = std::min(len - 4, i + tll);
    while (i + 6 <= end) {
        const int tsid = s[i] << 8 | s[i + 1], onid = s[i + 2] << 8 | s[i + 3];
        const int dl = ((s[i + 4] & 0x0F) << 8) | s[i + 5];
        i += 6;
        if (i + dl > end) break;
        descriptors(s + i, dl, [&](int tag, const uint8_t* p, int n) {
            if (tag != 0x41) return;
            for (int k = 0; k + 3 <= n; k += 3) {
                SiBouquet::Svc v; v.onid = onid; v.tsid = tsid; v.sid = p[k] << 8 | p[k + 1]; v.type = p[k + 2];
                bool have = false;
                for (const auto& e : b.services) if (e.onid == v.onid && e.tsid == v.tsid && e.sid == v.sid) { have = true; break; }
                if (!have && b.services.size() < 8192) b.services.push_back(v);
            }
        });
        i += dl;
    }
}

void SiCollector::sdt(const uint8_t* s, int len) {
    if (s[0] != 0x46) return;           // the actual transport stream is TsDemux's own
    if (len < 15) return;
    const int tsid = s[3] << 8 | s[4], onid = s[8] << 8 | s[9];
    int i = 11;
    const int end = len - 4;
    while (i + 5 <= end) {
        SiOtherService v;
        v.onid = onid; v.tsid = tsid; v.sid = s[i] << 8 | s[i + 1];
        v.running = s[i + 3] >> 5;
        v.freeCa = (s[i + 3] & 0x10) != 0;
        const int dl = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
        i += 5;
        if (i + dl > end) break;
        descriptors(s + i, dl, [&](int tag, const uint8_t* p, int n) {
            if (tag != 0x48 || n < 3) return;
            v.type = p[0];
            const int pl = p[1];
            if (2 + pl + 1 > n) return;
            v.provider = dvbText(p + 2, pl);
            const int nl = p[2 + pl];
            if (3 + pl + nl > n) return;
            v.name = dvbText(p + 3 + pl, nl);
        });
        i += dl;
        if (others_.size() < 16384) others_[{onid, tsid, v.sid}] = v;
    }
}

void SiCollector::eit(const uint8_t* s, int len) {
    if (len < 18) return;
    const int sid = s[3] << 8 | s[4], tsid = s[8] << 8 | s[9], onid = s[10] << 8 | s[11];
    int i = 14;
    const int end = len - 4;
    SiEitOther& e = eit_[{onid, tsid, sid}];
    e.onid = onid; e.tsid = tsid; e.sid = sid;
    const bool sched = s[0] != 0x4F || false;
    (void)sched;
    while (i + 12 <= end) {
        const int64_t start = mjdUtc(s + i + 2);
        const int dl = ((s[i + 10] & 0x0F) << 8) | s[i + 11];
        const int running = s[i + 10] >> 5;
        i += 12;
        if (i + dl > end) break;
        e.events++;
        if (start) { if (!e.first || start < e.first) e.first = start; if (start > e.last) e.last = start; }
        if (running == 4) descriptors(s + i, dl, [&](int tag, const uint8_t* p, int n) {
            if (tag != 0x4D || n < 5) return;
            const int nl = p[3];
            if (4 + nl > n) return;
            e.nowTitle = dvbText(p + 4, nl);
        });
        i += dl;
    }
    if (eit_.size() > 16384) eit_.erase(eit_.begin());
}

void SiCollector::tot(const uint8_t* s, int len) {
    if (len < 14) return;
    totUtc_ = mjdUtc(s + 3);
    const int dl = ((s[8] & 0x0F) << 8) | s[9];
    if (10 + dl + 4 > len) return;
    std::vector<SiTimeOffset> v;
    descriptors(s + 10, dl, [&](int tag, const uint8_t* p, int n) {
        if (tag != 0x58) return;
        for (int k = 0; k + 13 <= n; k += 13) {
            SiTimeOffset o;
            o.country.assign((const char*)p + k, 3);
            o.region = p[k + 3] >> 2;
            const int sign = (p[k + 3] & 1) ? -1 : 1;
            o.offsetMin = sign * (bcd(p[k + 4]) * 60 + bcd(p[k + 5]));
            o.changeAt = mjdUtc(p + k + 6);
            o.nextOffsetMin = sign * (bcd(p[k + 11]) * 60 + bcd(p[k + 12]));
            v.push_back(o);
        }
    });
    if (!v.empty()) tot_ = v;
}

void SiCollector::cat(const uint8_t* s, int len) {
    descriptors(s + 8, len - 12, [&](int tag, const uint8_t* p, int n) {
        if (tag != 0x09 || n < 4) return;
        SiCaSystem c;
        c.caSystemId = p[0] << 8 | p[1];
        c.emmPid = ((p[2] & 0x1F) << 8) | p[3];
        c.name = siCaSystemName(c.caSystemId);
        if (ca_.size() < 256) ca_[c.caSystemId * 8192 + c.emmPid] = c;
    });
}

SiSnapshot SiCollector::snapshot() const {
    SiSnapshot o;
    for (const auto& n : networks_) o.networks.push_back(n.second);
    for (const auto& b : bouquets_) o.bouquets.push_back(b.second);
    for (const auto& v : others_) o.otherServices.push_back(v.second);
    for (const auto& v : eit_) o.eitOther.push_back(v.second);
    o.timeOffsets = tot_;
    for (const auto& c : ca_) o.ca.push_back(c.second);
    o.totUtc = totUtc_; o.sections = sections_;
    return o;
}

SiOrbitGuess siGuessOrbit(const SiSnapshot& s, const std::vector<double>& found, double tol) {
    SiOrbitGuess g;
    std::map<int, int> votes;
    std::vector<const SiTransponder*> sat;
    for (const auto& n : s.networks) for (const auto& t : n.transponders) if (t.kind == 1) { votes[(int)std::lround(t.orbitDeg * 10)]++; sat.push_back(&t); }
    if (sat.empty()) return g;
    int best = 0, at = 0;
    for (const auto& kv : votes) if (kv.second > best) { best = kv.second; at = kv.first; }
    g.valid = true; g.orbitDeg = at / 10.0; g.transponders = best; g.carriers = (int)found.size();
    for (double f : found) for (const auto* t : sat) if (std::fabs(t->freqMhz - f) <= tol && (int)std::lround(t->orbitDeg * 10) == at) { g.matched++; break; }
    return g;
}

} // namespace dect2
