#include "dect2/acars_proto.h"
#include <cstdio>
#include <cstring>

namespace dect2 {

namespace {

struct CrcTable {
    uint16_t t[256];
    CrcTable() {
        for (int i = 0; i < 256; i++) {
            uint16_t c = (uint16_t)i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
            t[i] = c;
        }
    }
};
const CrcTable& crcTab() { static const CrcTable t; return t; }

// syn[e][bit]: residue of one flipped bit in the byte e places before the end of (text, check bytes). e = 0: the last check byte.
// A zero byte after the error advances the residue one step, so the table is built by repeating that step.
constexpr int kMaxSyn = 270;
struct SynTable {
    uint16_t s[kMaxSyn][8];
    SynTable() {
        const auto& t = crcTab().t;
        for (int b = 0; b < 8; b++) s[0][b] = t[1 << b];
        for (int e = 1; e < kMaxSyn; e++)
            for (int b = 0; b < 8; b++) s[e][b] = (uint16_t)((s[e - 1][b] >> 8) ^ t[s[e - 1][b] & 0xff]);
    }
};
const SynTable& synTab() { static const SynTable t; return t; }

bool repairWithParity(uint8_t* txt, int len, uint16_t crc, const int* pr, int pn, int idx, int& flips) {
    if (idx == pn) {
        if (crc == 0) return true;
        for (int e = 0; e < 2; e++)                      // what is left may be an error in the check bytes themselves
            for (int b = 0; b < 8; b++)
                if (synTab().s[e][b] == crc) return true;
        return false;
    }
    const int e = len - pr[idx] + 1;
    for (int b = 0; b < 8; b++) {
        if (repairWithParity(txt, len, (uint16_t)(crc ^ synTab().s[e][b]), pr, pn, idx + 1, flips)) {
            txt[pr[idx]] ^= (uint8_t)(1 << b);
            flips++;
            return true;
        }
    }
    return false;
}

bool repairDouble(uint8_t* txt, int len, uint16_t crc, int& flips) {
    for (int e = 0; e < 2; e++)
        for (int b = 0; b < 8; b++)
            if (synTab().s[e][b] == crc) return true;
    for (int k = 0; k < len; k++) {
        const int e = len - k + 1;
        for (int i = 0; i < 8; i++)
            for (int j = i + 1; j < 8; j++)
                if ((uint16_t)(crc ^ synTab().s[e][i] ^ synTab().s[e][j]) == 0) {
                    txt[k] ^= (uint8_t)((1 << i) | (1 << j));
                    flips += 2;
                    return true;
                }
    }
    return false;
}

struct LabelName { char a, b; const char* text; };
// Names that the decoders' own message formats support (acarsdec label.c reads fields from these labels), plus the service labels
// of ARINC 618. The full ARINC 620 list is not public and is not reproduced; unknown labels show with no name.
const LabelName kLabels[] = {
    {'_', 'd', "Command or response"},
    {'Q', '0', "Link test"},
    {'S', 'Q', "Ground station squitter"},
    {'S', 'A', "Media advisory"},
    {'H', '1', "Terminal message"},
    {'5', 'Z', "Airline defined downlink"},
    {'Q', '1', "OOOI report (out, off, on, in)"},
    {'Q', '2', "ETA report"},
    {'Q', 'A', "Gate out"},
    {'Q', 'B', "Wheels off"},
    {'Q', 'C', "Wheels on"},
    {'Q', 'D', "Gate in"},
    {'Q', 'E', "Gate out, destination"},
    {'Q', 'F', "Wheels off, destination"},
    {'Q', 'G', "Gate out and in"},
    {'Q', 'H', "Gate out"},
    {'Q', 'K', "Wheels on, destination"},
    {'Q', 'L', "Gate in, origin and destination"},
    {'Q', 'M', "Origin and destination"},
    {'Q', 'N', "Destination and ETA"},
    {'Q', 'P', "Gate out, origin and destination"},
    {'Q', 'Q', "Wheels off, origin and destination"},
    {'Q', 'R', "Wheels on, origin and destination"},
    {'Q', 'S', "Gate in, origin and destination"},
    {'Q', 'T', "Gate out and in, origin and destination"},
    {'1', '0', "Arrival report"},
    {'1', '5', "Flight status report"},
    {'1', '7', "ETA report"},
    {'2', 'N', "Takeoff report"},
    {'4', '4', "Position or ETA report"},
};

// Field layout of the Q labels (acarsdec label.c): which 4 characters of the text hold what.
struct QField { char kind; int off; };      // 'o' origin, 'd' destination, 'O' out, 'F' off, 'N' on, 'I' in, 'E' ETA
struct QLayout { char b; QField f[6]; };
const QLayout kQ[] = {
    {'1', {{'o', 0}, {'O', 4}, {'F', 8}, {'N', 12}, {'I', 16}, {'d', 24}}},
    {'2', {{'o', 0}, {'E', 4}}},
    {'A', {{'o', 0}, {'O', 4}}},
    {'B', {{'o', 0}, {'F', 4}}},
    {'C', {{'o', 0}, {'N', 4}}},
    {'D', {{'o', 0}, {'I', 4}}},
    {'E', {{'o', 0}, {'O', 4}, {'d', 8}}},
    {'F', {{'o', 0}, {'F', 4}, {'d', 8}}},
    {'G', {{'o', 0}, {'O', 4}, {'I', 8}}},
    {'H', {{'o', 0}, {'O', 4}}},
    {'K', {{'o', 0}, {'N', 4}, {'d', 8}}},
    {'L', {{'d', 0}, {'I', 8}, {'o', 13}}},
    {'M', {{'d', 0}, {'o', 8}}},
    {'N', {{'d', 4}, {'E', 8}}},
    {'P', {{'o', 0}, {'d', 4}, {'O', 8}}},
    {'Q', {{'o', 0}, {'d', 4}, {'F', 8}}},
    {'R', {{'o', 0}, {'d', 4}, {'N', 8}}},
    {'S', {{'o', 0}, {'d', 4}, {'I', 8}}},
    {'T', {{'o', 0}, {'d', 4}, {'O', 8}, {'I', 12}}},
};

bool isTime(const std::string& s, size_t o) {
    if (o + 4 > s.size()) return false;
    for (size_t i = 0; i < 4; i++) if (s[o + i] < '0' || s[o + i] > '9') return false;
    return (s[o] - '0') * 10 + (s[o + 1] - '0') < 24 && (s[o + 2] - '0') < 6;
}
bool isAirport(const std::string& s, size_t o) {
    if (o + 4 > s.size()) return false;
    for (size_t i = 0; i < 4; i++) if (s[o + i] < 'A' || s[o + i] > 'Z') return false;
    return true;
}

std::string printable(const uint8_t* p, int n, int cap) {
    std::string r;
    for (int i = 0; i < n && (int)r.size() < cap; i++) {
        const uint8_t c = p[i];
        if (c == '\n') r += '\n';
        else if (c == '\r') { if (i + 1 >= n || p[i + 1] != '\n') r += '\n'; }
        else if (c < 0x20 || c == 0x7f) r += '.';
        else r += (char)c;
    }
    return r;
}

} // namespace

uint16_t acarsCrcUpdate(uint16_t crc, uint8_t b) { return (uint16_t)((crc >> 8) ^ crcTab().t[(crc ^ b) & 0xff]); }
uint16_t acarsCrc(const uint8_t* p, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; i++) crc = acarsCrcUpdate(crc, p[i]);
    return crc;
}

uint16_t acarsSyndrome(int e, int bit) {
    if (e < 0 || e >= kMaxSyn || bit < 0 || bit > 7) return 0;
    return synTab().s[e][bit];
}

bool acarsRepairBlock(uint8_t* txt, int len, const uint8_t crc[2], int& fixedBits, int& parityErrors, int maxParityErrors) {
    fixedBits = 0; parityErrors = 0;
    if (len < 13 || len + 2 >= kMaxSyn) return false;
    int pr[8]; int pn = 0;
    for (int i = 0; i < len; i++)
        if (!acarsParityOk(txt[i])) { if (pn < 8) pr[pn] = i; pn++; }
    parityErrors = pn;
    if (pn > maxParityErrors) return false;
    uint16_t c = acarsCrc(txt, (size_t)len);
    c = acarsCrcUpdate(c, crc[0]);
    c = acarsCrcUpdate(c, crc[1]);
    if (pn) {
        if (!repairWithParity(txt, len, c, pr, pn, 0, fixedBits)) return false;
    } else if (c) {
        if (!repairDouble(txt, len, c, fixedBits)) return false;
    }
    for (int i = 0; i < len; i++)
        if (!acarsParityOk(txt[i])) return false;
    return true;
}

const char* acarsLabelText(char a, char b) {
    for (const auto& l : kLabels) if (l.a == a && l.b == b) return l.text;
    return "";
}

// Media advisory (label SA), libacars media-adv.c: version 0, state E or L, current link, hhmmss, available links, optional "/text".
static const char* linkName(char c) {
    switch (c) {
    case 'V': return "VHF ACARS"; case 'S': return "default SATCOM"; case 'H': return "HF"; case 'G': return "Globalstar";
    case 'C': return "ICO SATCOM"; case '2': return "VDL2"; case 'X': return "Inmarsat Aero"; case 'I': return "Iridium";
    }
    return nullptr;
}

static std::string decodeMediaAdvisory(const std::string& t) {
    if (t.size() < 10 || t[0] != '0' || (t[1] != 'E' && t[1] != 'L') || !linkName(t[2])) return "";
    for (int i = 3; i < 9; i++) if (t[(size_t)i] < '0' || t[(size_t)i] > '9') return "";
    const int h = (t[3] - '0') * 10 + (t[4] - '0'), m = (t[5] - '0') * 10 + (t[6] - '0'), s = (t[7] - '0') * 10 + (t[8] - '0');
    if (h > 23 || m > 59 || s > 59) return "";
    std::string avail;
    size_t i = 9;
    for (; i < t.size() && t[i] != '/'; i++) {
        const char* n = linkName(t[i]);
        if (!n) return "";
        if (!avail.empty()) avail += ", ";
        avail += n;
    }
    char b[32];
    snprintf(b, sizeof b, "%02d:%02d:%02d", h, m, s);
    return std::string(t[1] == 'E' ? "link established: " : "link lost: ") + linkName(t[2]) + " at " + b + ", available: " + avail;
}

std::string acarsDecodeText(const std::string& label, const std::string& text) {
    if (label == "SA") return decodeMediaAdvisory(text);
    if (label.size() != 2 || label[0] != 'Q') return "";
    for (const auto& q : kQ) {
        if (q.b != label[1]) continue;
        std::string o;
        for (const auto& f : q.f) {
            if (!f.kind) break;
            const bool air = f.kind == 'o' || f.kind == 'd';
            if (air ? !isAirport(text, (size_t)f.off) : !isTime(text, (size_t)f.off)) return "";
            const char* name = f.kind == 'o' ? "from" : f.kind == 'd' ? "to" : f.kind == 'O' ? "out" : f.kind == 'F' ? "off" : f.kind == 'N' ? "on" : f.kind == 'I' ? "in" : "eta";
            if (!o.empty()) o += ' ';
            o += name; o += ' ';
            o += text.substr((size_t)f.off, 4);
        }
        return o;
    }
    return "";
}

bool acarsParseBlock(const uint8_t* t, int len, AcarsMessage& m) {
    // mode, 7 address, ack, 2 label, block id, then STX and text, then the suffix: 13 bytes at least
    if (len < 13) return false;
    const uint8_t suffix = t[len - 1];
    if (suffix != 0x03 && suffix != 0x17) return false;
    m.finalBlock = suffix == 0x03;
    int n = len - 1;
    int i = 0;
    m.mode = (char)t[i++];
    std::string reg((const char*)t + i, 7); i += 7;
    size_t d = 0;
    while (d < reg.size() && reg[d] == '.') d++;
    m.reg = reg.substr(d);
    for (auto& c : m.reg) if ((uint8_t)c < 0x20) c = '.';
    m.ack = (char)t[i++];
    if (m.ack == 0x15) m.ack = '!'; else if (m.ack == 0x06) m.ack = '^';
    m.label.assign(2, ' ');
    m.label[0] = (char)t[i++]; m.label[1] = (char)t[i++];
    if (m.label[1] == 0x7f) m.label[1] = 'd';
    for (auto& c : m.label) if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7e) c = '.';
    m.labelText = acarsLabelText(m.label[0], m.label[1]);
    m.blockId = (char)t[i++];
    if (m.blockId == 0) m.blockId = ' ';
    m.downlink = m.blockId >= '0' && m.blockId <= '9';
    if (i >= n) { m.text.clear(); return true; }         // an acknowledgement without text
    if (t[i] != kAcarsStx) return false;
    i++;
    const uint8_t* p = t + i;
    int rem = n - i;
    if (m.downlink && rem >= 10) {
        m.msgNum.assign((const char*)p, 3);
        m.msgSeq = (char)p[3];
        m.flightId.assign((const char*)p + 4, 6);
        for (auto& c : m.msgNum) if ((uint8_t)c < 0x20) c = '.';
        for (auto& c : m.flightId) if ((uint8_t)c < 0x20) c = '.';
        p += 10; rem -= 10;
    }
    m.text = printable(p, rem, 220);
    if (m.label == "H1") {                                // sublabel and MFI, libacars acars.c: la_acars_extract_sublabel_and_mfi
        const char* s = m.text.c_str();
        int r = (int)m.text.size();
        int off = 0;
        if (m.downlink) {
            if (r >= 4 && s[0] == '#' && s[3] == 'B') { m.sublabel = m.text.substr(1, 2); off = 4; }
        } else if (r >= 5 && strncmp(s, "- #", 3) == 0) { m.sublabel = m.text.substr(3, 2); off = 5; }
        if (off && r - off >= 4 && s[off] == '/' && s[off + 3] == ' ') m.mfi = m.text.substr((size_t)off + 1, 2);
    }
    m.decoded = acarsDecodeText(m.label, m.text);
    return true;
}

std::vector<uint8_t> acarsBuildFrame(const AcarsBlockSpec& s, int preKeyChars) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> blk;                              // what the check sequence covers
    blk.push_back(acarsWithParity((uint8_t)s.mode));
    std::string reg = s.reg.size() > 7 ? s.reg.substr(0, 7) : s.reg;
    reg.insert(0, 7 - reg.size(), '.');
    for (char c : reg) blk.push_back(acarsWithParity((uint8_t)c));
    blk.push_back(acarsWithParity(s.ack));
    blk.push_back(acarsWithParity((uint8_t)s.label[0]));
    blk.push_back(acarsWithParity(s.label[1] == 'd' && s.label[0] == '_' ? kAcarsDel : (uint8_t)s.label[1]));
    blk.push_back(acarsWithParity((uint8_t)s.blockId));
    if (s.hasText) {
        blk.push_back(kAcarsStx);
        for (size_t i = 0; i < s.text.size() && i < 220; i++) blk.push_back(acarsWithParity((uint8_t)s.text[i]));
    }
    blk.push_back(s.lastBlock ? kAcarsEtx : kAcarsEtb);
    const uint16_t crc = acarsCrc(blk.data(), blk.size());
    if (preKeyChars > 0) {
        for (int i = 0; i < preKeyChars; i++) out.push_back(0xff);
        out.push_back(acarsWithParity('+'));
        out.push_back(acarsWithParity('*'));
    }
    out.push_back(kAcarsSyn); out.push_back(kAcarsSyn); out.push_back(kAcarsSoh);
    out.insert(out.end(), blk.begin(), blk.end());
    out.push_back((uint8_t)(crc & 0xff)); out.push_back((uint8_t)(crc >> 8));
    out.push_back(kAcarsDel);
    return out;
}

} // namespace dect2
