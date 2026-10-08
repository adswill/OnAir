// Inmarsat Aero signal-unit layer. Facts from JAERO (github.com/jontio/JAERO, JAERO/aerol.cpp and aerol.h): SU of 12
// bytes with a CRC-16 (AeroLcrc16: reflected 0x8408, init 0xFFFF, inverted) low byte first, SU type octets, system
// table field positions, ISU/SSU numbering (ISUData::update), ACARS user data (ParserISU::parse).
// Interleaver block sizes: aerol.cpp setSettings (600: 6x64, 1200: 9x64, 10500: 78x64 channel bits, rate 1/2 coding).
// Not sourced, left as zero bytes in the builders: the other fields of log-on SUs, fill-in SU content.
#include "dect2/aero_su.h"
#include "dect2/aero_acars.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {

// P channel SU type octets (aerol.h AEROTypeP)
enum : uint8_t {
    kFill = 0x01, kSysGesChan = 0x05, kSysBeam = 0x07, kSysIndex = 0x0A, kSysSat = 0x0C,
    kLogOnReq = 0x10, kLogOnConfirm = 0x11, kLogOffReq = 0x12, kLogOnReject = 0x13, kLogOnInterrog = 0x14,
    kLogAck = 0x15, kLogPrompt = 0x16, kLogReassign = 0x17, kCallAnnounce = 0x21, kEirpTable = 0x28,
    kCallProgress = 0x30, kCAssignDistress = 0x31, kCAssignFlight = 0x32, kCAssignOther = 0x33, kCAssignNon = 0x34,
    kPRControl = 0x40, kTControl = 0x41, kTAssign = 0x51, kRqa = 0x61, kRack = 0x62,
    kUserIsu = 0x71, kUser3 = 0x74, kUser4 = 0x76
};

const char* typeNameOf(uint8_t t) {
    switch (t) {
    case 0x00: return "Reserved 0";
    case kFill: return "Fill-in";
    case kSysGesChan: return "System table: GES channels";
    case kSysBeam: return "System table: GES beam support";
    case kSysIndex: return "System table: index";
    case kSysSat: return "System table: satellite";
    case kLogOnReq: return "Log-on request";
    case kLogOnConfirm: return "Log-on confirm";
    case kLogOffReq: return "Log-off request";
    case kLogOnReject: return "Log-on reject";
    case kLogOnInterrog: return "Log-on interrogation";
    case kLogAck: return "Log-on/off acknowledge";
    case kLogPrompt: return "Log-on prompt";
    case kLogReassign: return "Data channel reassignment";
    case 0x18: case 0x19: case 0x26: return "Reserved";
    case kCallAnnounce: return "Call announcement";
    case kEirpTable: return "EIRP table";
    case kCallProgress: return "Call progress";
    case kCAssignDistress: return "C channel assignment (distress)";
    case kCAssignFlight: return "C channel assignment (flight safety)";
    case kCAssignOther: return "C channel assignment (other safety)";
    case kCAssignNon: return "C channel assignment (non-safety)";
    case kPRControl: return "P/R channel control";
    case kTControl: return "T channel control";
    case kTAssign: return "T channel assignment";
    case kRqa: return "Request for acknowledgement";
    case kRack: return "Acknowledgement";
    case kUserIsu: return "User data ISU";
    case kUser3: return "User data 3-octet LSDU";
    case kUser4: return "User data 4-octet LSDU";
    default: break;
    }
    if ((t & 0xC0) == 0xC0) return "User data SSU";
    return "Unknown";
}

// Types whose octets 1..3 are the AES id and octet 4 the GES id (JAERO: SendLogOnOff, CreateCAssignmentItem, ISUData)
bool hasAesGes(uint8_t t) {
    switch (t) {
    case kLogOnReq: case kLogOnConfirm: case kLogOffReq: case kLogOnReject: case kLogOnInterrog: case kLogAck:
    case kLogPrompt: case kLogReassign: case kCallAnnounce: case kCallProgress: case kCAssignDistress:
    case kCAssignFlight: case kCAssignOther: case kCAssignNon: case kUserIsu:
        return true;
    default:
        return false;
    }
}

double chanToMHz(int ch) { return ch * 0.0025 + 1510.0; }
int mhzToChan(double f) { return int(std::lround((f - 1510.0) / 0.0025)); }

void fillDerived(AeroSu& s) {
    s.type = s.bytes[0];
    s.typeName = s.crcOk ? typeNameOf(s.type) : "Bad CRC";
    s.aesId = 0;
    s.gesId = -1;
    if (hasAesGes(s.type)) {
        s.aesId = (uint32_t(s.bytes[1]) << 16) | (uint32_t(s.bytes[2]) << 8) | s.bytes[3];
        s.gesId = s.bytes[4];
    } else if (s.type == kSysGesChan) {
        s.gesId = s.bytes[3];
    } else if (s.type == kPRControl) {
        s.gesId = s.bytes[4];
    }
}

AeroSu finishSu(AeroSu s) {
    const uint16_t c = aeroSuCrc(s.bytes, 10);
    s.bytes[10] = uint8_t(c & 0xFF);
    s.bytes[11] = uint8_t(c >> 8);
    s.crcOk = true;
    fillDerived(s);
    return s;
}

AeroSu makeSu(uint8_t type) {
    AeroSu s;
    s.bytes[0] = type;
    return s;
}

void putAesGes(AeroSu& s, uint32_t aes, int ges) {
    s.bytes[1] = uint8_t(aes >> 16);
    s.bytes[2] = uint8_t(aes >> 8);
    s.bytes[3] = uint8_t(aes);
    s.bytes[4] = uint8_t(ges < 0 ? 0 : ges);
}

uint8_t nextBlockId(uint8_t bi) {
    if (bi >= '0' && bi <= '9') return uint8_t('0' + (bi - '0' + 1) % 10);
    if (bi >= 'A' && bi <= 'Z') return uint8_t('A' + (bi - 'A' + 1) % 26);
    return uint8_t(bi + 1);
}

int bitRateOfCode(int c) {
    switch (c) {
    case 0: return 600;
    case 1: return 1200;
    case 2: return 2400;
    case 3: return 4800;
    case 4: return 6000;
    case 5: return 5250;
    case 6: return 10500;
    case 7: return 8400;
    case 9: return 21000;
    default: return 0;
    }
}
int codeOfBitRate(int r) {
    switch (r) {
    case 600: return 0;
    case 1200: return 1;
    case 2400: return 2;
    case 4800: return 3;
    case 6000: return 4;
    case 5250: return 5;
    case 10500: return 6;
    case 8400: return 7;
    case 21000: return 9;
    default: return 15;
    }
}

} // namespace

uint16_t aeroSuCrc(const uint8_t* p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0x8408) : uint16_t(crc >> 1);
    }
    return uint16_t(~crc);
}

size_t aeroBlockBytes(int bitRate) {
    switch (bitRate) {
    case 600: return 24;
    case 1200: return 36;
    case 10500: return 312;
    default: return 0;
    }
}
size_t aeroSusPerBlock(int bitRate) { return aeroBlockBytes(bitRate) / 12; }

std::vector<AeroSu> aeroSplitSus(const uint8_t* block, size_t nBytes, int bitRate) {
    std::vector<AeroSu> out;
    if (!block || aeroBlockBytes(bitRate) == 0) return out;
    size_t n = std::min(nBytes, aeroBlockBytes(bitRate)) / 12;
    for (size_t k = 0; k < n; k++) {
        AeroSu s;
        std::memcpy(s.bytes, block + k * 12, 12);
        const uint16_t rec = uint16_t(s.bytes[10] | (s.bytes[11] << 8));
        s.crcOk = aeroSuCrc(s.bytes, 10) == rec;
        if (!s.crcOk && rec == 0) {                 // JAERO accepts SUs that are all zero
            bool zero = true;
            for (int i = 0; i < 10; i++) zero = zero && s.bytes[i] == 0;
            s.crcOk = zero;
        }
        fillDerived(s);
        out.push_back(s);
    }
    return out;
}

AeroSysInfo aeroParseSystemSu(const AeroSu& su) {
    AeroSysInfo r;
    if (!su.crcOk) return r;
    const uint8_t* b = su.bytes;
    auto be16 = [&](int i) { return (int(b[i]) << 8) | b[i + 1]; };
    if (su.type == kSysGesChan) {
        r.kind = 1;
        r.gesId = b[3];
        r.seq = (b[2] >> 2) & 0x3F;
        r.lsu = b[2] & 3;
        for (int i = 0; i < 3; i++) r.freqMHz[i] = chanToMHz(be16(4 + 2 * i));
        if (r.lsu <= 1) {
            r.freqMHz[1] += 101.5;
            r.freqMHz[2] += 101.5;
            r.names = "Psmc Rsmc0 Rsmc1";
        } else {
            for (int i = 0; i < 3; i++) r.freqMHz[i] += 101.5;
            r.names = r.lsu == 2 ? "Rsmc2 Rsmc3 Rsmc4" : "Rsmc5 Rsmc6 Rsmc7";
        }
    } else if (su.type == kSysSat) {
        r.kind = 2;
        r.seq = (b[2] >> 2) & 0x3F;
        r.satId = ((b[2] << 4) & 0x30) | ((b[3] >> 4) & 0x0F);
        const double lon = b[5] * 1.5;
        r.lonDeg = lon > 180.0 ? lon - 360.0 : lon;
        r.freqMHz[0] = chanToMHz(((b[6] & 0x7F) << 8) | b[7]);
        const int ch2 = ((b[8] & 0x7F) << 8) | b[9];
        r.freqMHz[1] = ch2 ? chanToMHz(ch2) : 0.0;
        r.spotBeam = (b[6] & 0x80) != 0;
    } else if (su.type == kPRControl) {
        r.kind = 3;
        r.gesId = b[4];
        r.bitRate = bitRateOfCode((b[7] >> 4) & 0x0F);
        r.freqMHz[0] = chanToMHz(((b[8] & 0x7F) << 8) | b[9]);
        r.spotBeam = (b[8] & 0x80) != 0;
    }
    return r;
}

std::string aeroDescribeSu(const AeroSu& su) {
    char buf[200];
    std::string s = su.typeName;
    if (!su.crcOk) return s;
    if (su.aesId || su.type == kUserIsu || su.type == kLogOnConfirm) {
        std::snprintf(buf, sizeof buf, ": AES %06X GES %02X", su.aesId, su.gesId < 0 ? 0 : su.gesId);
        s += buf;
    }
    const AeroSysInfo i = aeroParseSystemSu(su);
    if (i.kind == 1) {
        // names holds three words, one per frequency
        char a[16], b2[16], c[16];
        std::sscanf(i.names.c_str(), "%15s %15s %15s", a, b2, c);
        std::snprintf(buf, sizeof buf, ": GES %02X %s %.4f, %s %.4f, %s %.4f MHz", i.gesId, a, i.freqMHz[0], b2,
                      i.freqMHz[1], c, i.freqMHz[2]);
        s += buf;
    } else if (i.kind == 2) {
        std::snprintf(buf, sizeof buf, ": satellite %d at %.1f%c, P channel %.4f MHz%s", i.satId, std::fabs(i.lonDeg),
                      i.lonDeg < 0 ? 'W' : 'E', i.freqMHz[0], i.spotBeam ? " (spot beam)" : "");
        s += buf;
        if (i.freqMHz[1] > 0) {
            std::snprintf(buf, sizeof buf, ", %.4f MHz", i.freqMHz[1]);
            s += buf;
        }
    } else if (i.kind == 3) {
        std::snprintf(buf, sizeof buf, ": GES %02X P channel %.4f MHz at %d bit/s%s", i.gesId, i.freqMHz[0], i.bitRate,
                      i.spotBeam ? " (spot beam)" : "");
        s += buf;
    }
    return s;
}

// ---- builders ----

AeroSu aeroFillSu() { return finishSu(makeSu(kFill)); }

AeroSu aeroLogonSu(uint32_t aesId, int gesId, bool logon) {
    AeroSu s = makeSu(logon ? kLogOnConfirm : kLogOffReq);
    putAesGes(s, aesId, gesId);
    return finishSu(s);
}

AeroSu aeroSatelliteIdSu(int satId, double lonDegEast, double psmc1MHz, double psmc2MHz, int seq) {
    AeroSu s = makeSu(kSysSat);
    double lon = std::fmod(lonDegEast, 360.0);
    if (lon < 0) lon += 360.0;
    s.bytes[2] = uint8_t(((seq & 0x3F) << 2) | ((satId >> 4) & 0x03));
    s.bytes[3] = uint8_t((satId & 0x0F) << 4);
    s.bytes[5] = uint8_t(std::lround(lon / 1.5) & 0xFF);
    const int c1 = mhzToChan(psmc1MHz);
    s.bytes[6] = uint8_t((c1 >> 8) & 0x7F);
    s.bytes[7] = uint8_t(c1 & 0xFF);
    if (psmc2MHz > 0) {
        const int c2 = mhzToChan(psmc2MHz);
        s.bytes[8] = uint8_t((c2 >> 8) & 0x7F);
        s.bytes[9] = uint8_t(c2 & 0xFF);
    }
    return finishSu(s);
}

AeroSu aeroChannelControlSu(int gesId, int bitRate, double pChannelMHz, bool spotBeam) {
    AeroSu s = makeSu(kPRControl);
    s.bytes[4] = uint8_t(gesId);
    s.bytes[7] = uint8_t((codeOfBitRate(bitRate) & 0x0F) << 4);
    const int c = mhzToChan(pChannelMHz);
    s.bytes[8] = uint8_t(((c >> 8) & 0x7F) | (spotBeam ? 0x80 : 0));
    s.bytes[9] = uint8_t(c & 0xFF);
    return finishSu(s);
}

// part 0..3: GES channel SU with lsu = part; 4: satellite id; 5: P/R channel control. Frequencies are synthetic
// but inside the real bands (P channels near 1545 MHz, R channels near 1630 MHz).
AeroSu aeroSystemTableSu(int gesId, int part) {
    if (part == 4) return aeroSatelliteIdSu(gesId & 0x3F, 64.5, 1545.0 + 0.0125 * (gesId & 0x0F), 0.0);
    if (part == 5) return aeroChannelControlSu(gesId, 10500, 1545.0 + 0.0125 * (gesId & 0x0F));
    const int lsu = part & 3;
    const int seq = (part >> 2) & 0x3F;
    AeroSu s = makeSu(kSysGesChan);
    s.bytes[2] = uint8_t((seq << 2) | lsu);
    s.bytes[3] = uint8_t(gesId);
    const int p = mhzToChan(1545.0 + 0.0125 * (gesId & 0x0F));
    int ch[3];
    if (lsu <= 1) {
        // Psmc as received, Rsmc as the offset from the 1611.5 MHz base JAERO adds
        ch[0] = p;
        ch[1] = int(std::lround((1626.5 + 0.05 * lsu - 1611.5) / 0.0025));
        ch[2] = int(std::lround((1627.0 + 0.05 * lsu - 1611.5) / 0.0025));
    } else {
        for (int i = 0; i < 3; i++) ch[i] = int(std::lround((1628.0 + 0.5 * lsu + 0.05 * i - 1611.5) / 0.0025));
    }
    for (int i = 0; i < 3; i++) {
        s.bytes[4 + 2 * i] = uint8_t(ch[i] >> 8);
        s.bytes[5 + 2 * i] = uint8_t(ch[i]);
    }
    return finishSu(s);
}

std::vector<AeroSu> aeroAcarsToSus(const AeroAcars& m, int qno, int refno) {
    std::vector<AeroSu> out;
    const size_t prefix = m.uplink ? 0 : 10;
    const size_t chunk = 220 - prefix;
    const std::string& all = m.text;
    uint8_t bi = m.blockId.empty() ? uint8_t(m.uplink ? 'A' : '0') : uint8_t(m.blockId[0]);
    size_t pos = 0;
    int blockNo = 0;
    do {
        const size_t n = std::min(chunk, all.size() - pos);
        AeroAcarsBlock b;
        b.mode = m.mode.empty() ? "2" : m.mode.substr(0, 1);
        b.registration = m.registration;
        b.label = m.label.empty() ? "H1" : m.label;
        b.blockId = bi;
        b.moreToCome = pos + n < all.size();
        std::string text = all.substr(pos, n);
        if (!m.uplink && blockNo == 0) {
            std::string mn = m.msgNo.empty() ? "M01A" : m.msgNo.substr(0, 4);
            mn.resize(4, ' ');
            std::string fl = m.flight.substr(0, 6);
            fl.resize(6, ' ');
            text = mn + fl + text;
        }
        b.hasText = !text.empty();
        b.text = text;
        const std::vector<uint8_t> ud = aeroBuildAcarsBlock(b);

        // ISU: one 0x71 SU with the first two bytes, then SSUs of 8 bytes; the last SSU has 1..8 used bytes
        const int ref = (refno + blockNo) & 0x0F;
        const size_t rest = ud.size() - 2;
        const size_t nSsu = (rest + 7) / 8;
        const size_t lastN = rest - 8 * (nSsu - 1);
        AeroSu head = makeSu(kUserIsu);
        putAesGes(head, m.aesId, m.gesId);
        head.bytes[5] = uint8_t(((qno & 0x0F) << 4) | ref);
        head.bytes[6] = uint8_t(nSsu & 0x3F);
        head.bytes[7] = uint8_t(lastN << 4);
        head.bytes[8] = ud[0];
        head.bytes[9] = ud[1];
        out.push_back(finishSu(head));
        for (size_t j = 0; j < nSsu; j++) {
            AeroSu s = makeSu(uint8_t(0xC0 | (nSsu - 1 - j)));
            s.bytes[1] = uint8_t(((qno & 0x0F) << 4) | ref);
            const size_t take = (j + 1 == nSsu) ? lastN : 8;
            for (size_t k = 0; k < take; k++) s.bytes[2 + k] = ud[2 + 8 * j + k];
            out.push_back(finishSu(s));
        }
        pos += n;
        bi = nextBlockId(bi);
        blockNo++;
    } while (pos < all.size());
    return out;
}

std::vector<AeroSu> aeroAcarsToSus(const AeroAcars& m) {
    // a reference number that depends on the message only, so the same input gives the same SUs
    uint32_t h = 2166136261u;
    auto mix = [&](uint8_t c) { h = (h ^ c) * 16777619u; };
    mix(uint8_t(m.aesId)); mix(uint8_t(m.aesId >> 8)); mix(uint8_t(m.aesId >> 16));
    for (char c : m.label) mix(uint8_t(c));
    for (char c : m.text) mix(uint8_t(c));
    return aeroAcarsToSus(m, 0, int(h >> 24) & 0x0F);
}

std::vector<uint8_t> aeroBuildBlock(const std::vector<AeroSu>& sus, int bitRate) {
    const size_t bytes = aeroBlockBytes(bitRate);
    std::vector<uint8_t> out;
    if (!bytes) return out;
    const size_t n = bytes / 12;
    const AeroSu fill = aeroFillSu();
    out.reserve(bytes);
    for (size_t i = 0; i < n; i++) {
        const AeroSu& s = i < sus.size() ? sus[i] : fill;
        out.insert(out.end(), s.bytes, s.bytes + 12);
    }
    return out;
}

// ---- decoder ----

struct AeroSuDecoder::State {
    struct Isu {
        uint32_t aes = 0;
        int ges = 0;
        int qno = 0, refno = 0;
        int remaining = 0;              // SSUs still to come
        int lastOctets = 0;
        std::vector<uint8_t> data;
        double t = 0;
    };
    struct Frag {
        AeroAcars msg;
        uint8_t tak = 0;
        uint8_t lastBi = 0;
        bool crcOk = true;
        double t = 0;
    };
    std::vector<Isu> isus;
    std::vector<Frag> frags;
};

AeroSuDecoder::AeroSuDecoder() : s_(std::make_unique<State>()) {}
AeroSuDecoder::~AeroSuDecoder() = default;
void AeroSuDecoder::reset() {
    s_->isus.clear();
    s_->frags.clear();
}

namespace {
std::string hexOf(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (uint8_t c : v) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}
std::string stripDots(const std::string& r) {
    size_t i = 0;
    while (i < r.size() && r[i] == '.') i++;
    std::string s = r.substr(i);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
} // namespace

void AeroSuDecoder::feed(const AeroSu& su, double t, std::vector<AeroAcars>& acars, std::vector<AeroLogon>& logons) {
    State& st = *s_;
    if (!su.crcOk) return;
    // forget what has waited too long
    st.isus.erase(std::remove_if(st.isus.begin(), st.isus.end(), [&](const State::Isu& i) { return t - i.t > 60.0; }),
                  st.isus.end());
    st.frags.erase(std::remove_if(st.frags.begin(), st.frags.end(), [&](const State::Frag& f) { return t - f.t > 120.0; }),
                   st.frags.end());

    const uint8_t type = su.bytes[0];
    if (type == kLogOnConfirm || type == kLogOffReq) {
        AeroLogon l;
        l.aesId = su.aesId;
        l.gesId = su.gesId;
        l.logon = type == kLogOnConfirm;
        logons.push_back(l);
        return;
    }

    std::vector<uint8_t> done;
    State::Isu doneInfo;
    bool complete = false;
    if (type == kUserIsu) {
        State::Isu i;
        i.aes = su.aesId;
        i.ges = su.gesId;
        i.qno = (su.bytes[5] >> 4) & 0x0F;
        i.refno = su.bytes[5] & 0x0F;
        i.remaining = su.bytes[6] & 0x3F;
        i.lastOctets = (su.bytes[7] >> 4) & 0x0F;
        i.data.assign(su.bytes + 8, su.bytes + 10);
        i.t = t;
        if (i.lastOctets > 8 || i.remaining == 0) return;
        for (size_t k = 0; k < st.isus.size(); k++)
            if (st.isus[k].aes == i.aes && st.isus[k].ges == i.ges && st.isus[k].qno == i.qno && st.isus[k].refno == i.refno) {
                st.isus.erase(st.isus.begin() + long(k));
                break;
            }
        if (st.isus.size() >= 16) st.isus.erase(st.isus.begin());
        st.isus.push_back(i);
        return;
    }
    if ((type & 0xC0) == 0xC0) {
        const int seq = type & 0x3F;
        const int qno = (su.bytes[1] >> 4) & 0x0F, ref = su.bytes[1] & 0x0F;
        // most recent ISU that expects exactly this SSU
        for (size_t k = st.isus.size(); k-- > 0;) {
            State::Isu& i = st.isus[k];
            if (i.qno != qno || i.refno != ref || i.remaining != seq + 1) continue;
            i.remaining--;
            i.t = t;
            if (i.remaining == 0) {
                i.data.insert(i.data.end(), su.bytes + 2, su.bytes + 2 + i.lastOctets);
                doneInfo = i;
                complete = true;
                st.isus.erase(st.isus.begin() + long(k));
            } else {
                i.data.insert(i.data.end(), su.bytes + 2, su.bytes + 10);
            }
            break;
        }
    }
    if (!complete) return;

    AeroAcarsBlock blk;
    if (!aeroParseAcarsBlock(doneInfo.data, blk)) {
        AeroAcars m;
        m.aesId = doneInfo.aes;
        m.gesId = doneInfo.ges;
        m.labelText = "Non-ACARS user data";
        m.text = hexOf(doneInfo.data);
        m.crcOk = true;                     // the SU CRCs held; there is no block check to test
        acars.push_back(m);
        return;
    }

    const bool uplink = !(blk.blockId >= '0' && blk.blockId <= '9');
    // continuation of a message that ended its last block with ETB
    for (size_t k = 0; k < st.frags.size(); k++) {
        State::Frag& f = st.frags[k];
        if (f.msg.aesId != doneInfo.aes || f.msg.gesId != doneInfo.ges || f.msg.label != blk.label ||
            f.msg.mode != blk.mode || f.tak != blk.tak || stripDots(blk.registration) != f.msg.registration)
            continue;
        if (nextBlockId(f.lastBi) != blk.blockId) continue;
        f.msg.text += blk.text;
        f.msg.crcOk = f.msg.crcOk && blk.crcOk && blk.parityOk;
        f.lastBi = blk.blockId;
        f.t = t;
        if (!blk.moreToCome) {
            f.msg.blockId.assign(1, char(blk.blockId));
            acars.push_back(f.msg);
            st.frags.erase(st.frags.begin() + long(k));
        }
        return;
    }

    AeroAcars m;
    m.aesId = doneInfo.aes;
    m.gesId = doneInfo.ges;
    m.uplink = uplink;
    m.mode = blk.mode;
    m.registration = stripDots(blk.registration);
    m.label = blk.label;
    m.labelText = aeroAcarsLabelText(blk.label);
    m.blockId.assign(1, char(blk.blockId));
    m.text = blk.text;
    m.crcOk = blk.crcOk && blk.parityOk;
    if (!uplink && blk.hasText && m.text.size() >= 10) {
        // downlink text starts with the message number (4) and the flight id (6)
        m.msgNo = m.text.substr(0, 4);
        m.flight = m.text.substr(4, 6);
        while (!m.flight.empty() && m.flight.back() == ' ') m.flight.pop_back();
        m.text.erase(0, 10);
    }
    if (blk.moreToCome) {
        if (st.frags.size() >= 16) st.frags.erase(st.frags.begin());
        State::Frag f;
        f.msg = m;
        f.tak = blk.tak;
        f.lastBi = blk.blockId;
        f.crcOk = m.crcOk;
        f.t = t;
        st.frags.push_back(f);
        return;
    }
    acars.push_back(m);
}

} // namespace dect2
