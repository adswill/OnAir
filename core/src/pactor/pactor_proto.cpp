// PACTOR-I packet layer (see pactor_proto.h).
#include "dect2/pactor/pactor_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace dect2 {
namespace pactor {

uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

uint8_t statusByte(const Status& s) { return (uint8_t)((s.count & 3) | (s.huffman ? 4 : 0) | (s.breakIn ? 8 : 0) | (s.qrt ? 16 : 0)); }
Status statusParse(uint8_t b) { Status s; s.count = b & 3; s.huffman = b & 4; s.breakIn = b & 8; s.qrt = b & 16; return s; }

bool isFast(const Packet& p) { return (int)p.data.size() == kDataFast; }

std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& b) {
    std::vector<uint8_t> v;
    v.reserve(b.size() * 8);
    for (uint8_t x : b) for (int k = 0; k < 8; k++) v.push_back((uint8_t)((x >> k) & 1));
    return v;
}
std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> b(bits.size() / 8, 0);
    for (size_t i = 0; i < b.size() * 8; i++) if (bits[i]) b[i / 8] = (uint8_t)(b[i / 8] | (1 << (i % 8)));
    return b;
}

std::vector<uint8_t> packetBits(const Packet& p) {
    std::vector<uint8_t> body = p.data;
    body.push_back(p.status);
    const uint16_t c = crc16(body.data(), body.size());
    std::vector<uint8_t> all;
    all.push_back(p.header);
    all.insert(all.end(), body.begin(), body.end());
    all.push_back((uint8_t)(c >> 8));
    all.push_back((uint8_t)(c & 0xFF));
    return bytesToBits(all);
}

bool packetParse(const std::vector<uint8_t>& bits, Packet& out, int* headerErr, int* crcMode) {
    if (bits.size() != (size_t)kBitsSlow && bits.size() != (size_t)kBitsFast) return false;
    const std::vector<uint8_t> b = bitsToBytes(bits);
    const int e1 = __builtin_popcount(b[0] ^ kHeaderA), e2 = __builtin_popcount(b[0] ^ kHeaderB);
    if (headerErr) *headerErr = std::min(e1, e2);
    const size_t n = b.size();
    const uint16_t rx = (uint16_t)((b[n - 2] << 8) | b[n - 1]);
    const uint16_t c = crc16(&b[1], n - 3);
    int mode = -1;
    if (rx == c) mode = 0; else if (rx == (uint16_t)~c) mode = 1;
    if (mode < 0) return false;
    if (crcMode) *crcMode = mode;
    out.header = b[0];
    out.data.assign(b.begin() + 1, b.begin() + (long)n - 3);
    out.status = b[n - 3];
    return true;
}

std::vector<uint8_t> csBits(int n) {
    std::vector<uint8_t> v;
    if (n < 1 || n > 4) return v;
    for (int k = 11; k >= 0; k--) v.push_back((uint8_t)((kCs[n - 1] >> k) & 1));
    return v;
}
int csMatch(const uint8_t* bits12, int* dist) {
    int best = 0, bd = 99;
    for (int n = 1; n <= 4; n++) {
        int d = 0;
        for (int k = 0; k < 12; k++) d += (bits12[k] & 1) != ((kCs[n - 1] >> (11 - k)) & 1);
        if (d < bd) { bd = d; best = n; }
    }
    if (dist) *dist = bd;
    return bd <= 2 ? best : 0;
}

std::vector<uint8_t> callData(const std::string& call, bool reversed) {
    std::string c = call.substr(0, 8);
    if (reversed) std::reverse(c.begin(), c.end());
    std::vector<uint8_t> d(c.begin(), c.end());
    while (d.size() < 8) d.push_back(kCallPad);
    return d;
}
bool callParse(const std::vector<uint8_t>& d, std::string& call, bool* reversed) {
    if (d.size() < 8) return false;
    auto ok = [](uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '/'; };
    size_t n = 0;
    while (n < 8 && ok(d[n])) n++;
    if (n < 3 || n >= 8) return false;
    for (size_t i = n; i < 8; i++) if (d[i] != kCallPad) return false;
    call.assign(d.begin(), d.begin() + (long)n);
    if (reversed) *reversed = false;
    return true;
}

// ------------------------------------------------------------ Huffman
namespace {
struct Table {
    std::map<std::string, int> dec;      // code -> character
    std::map<int, std::string> enc;
    size_t maxLen = 0;
    void add(int ch, const std::string& code) { dec[code] = ch; enc[ch] = code; maxLen = std::max(maxLen, code.size()); }
    void builtin() {
        dec.clear(); enc.clear(); maxLen = 0;
        add(' ', "10"); add('e', "011"); add('t', "00000"); add('s', "00100"); add('a', "01000"); add(13, "001100"); add(10, "001101");
    }
};
Table& table() { static Table t = [] { Table x; x.builtin(); return x; }(); return t; }
std::mutex gMu;
}

void huffReset() { std::lock_guard<std::mutex> lk(gMu); table().builtin(); }
size_t huffEntries() { std::lock_guard<std::mutex> lk(gMu); return table().enc.size(); }
bool huffComplete() {
    std::lock_guard<std::mutex> lk(gMu);
    int n = 0;
    for (int c = 32; c < 127; c++) n += table().enc.count(c) ? 1 : 0;
    return n >= 95;
}
bool huffLoad(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    Table t;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        std::string code;
        for (char* q = tab + 1; *q; q++) {
            if (*q == '0' || *q == '1') code += *q;
            else if (*q == '\n' || *q == '\r') break;
            else if (*q != ' ') { code.clear(); break; }
        }
        if (code.empty() || code.size() > 15) continue;
        int ch;
        if (line[0] && !line[1]) ch = (unsigned char)line[0]; else ch = atoi(line);
        if (ch <= 0 || ch > 255) continue;
        t.add(ch, code);
    }
    fclose(f);
    if (t.enc.size() < 3) return false;
    std::lock_guard<std::mutex> lk(gMu);
    table() = std::move(t);
    return true;
}
bool huffEncode(const std::string& text, std::vector<uint8_t>& bits) {
    std::lock_guard<std::mutex> lk(gMu);
    bits.clear();
    for (unsigned char c : text) {
        auto it = table().enc.find(c);
        if (it == table().enc.end()) return false;
        for (char b : it->second) bits.push_back(b == '1');
    }
    return true;
}
std::string huffDecode(const std::vector<uint8_t>& bits, bool* unknown) {
    std::lock_guard<std::mutex> lk(gMu);
    std::string out, cur;
    if (unknown) *unknown = false;
    for (size_t i = 0; i < bits.size(); i++) {
        cur += bits[i] ? '1' : '0';
        auto it = table().dec.find(cur);
        if (it != table().dec.end()) { out += (char)it->second; cur.clear(); }
        else if (cur.size() >= table().maxLen) { out += '~'; if (unknown) *unknown = true; return out; }   // not in the table: the rest cannot be framed
    }
    return out;
}

std::string asciiText(const std::vector<uint8_t>& data) {
    std::string s;
    for (uint8_t c : data) s += (c == '\n' || c == '\r') ? '\n' : (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    return s;
}

} // namespace pactor
} // namespace dect2
