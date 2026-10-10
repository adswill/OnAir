// The 77-bit messages of FT8 / FT4 / FT2 (see hfdig_ftx_int.h): packing and unpacking of every message type of WSJT-X's packjt77.f90
// (free text, DXpedition, Field Day, telemetry, standard with /R /P, RTTY Roundup, nonstandard calls, EU VHF), re-implemented from its
// description and cross-checked against ft8_lib's ft8/message.c (constants NTOKENS, MAX22, MAXGRID4, the alphabets, the hash).
#include "hfdig_ftx_int.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace dect2 {
namespace ftx {

namespace {

constexpr uint32_t kNtokens = 2063592, kMax22 = 4194304, kMaxGrid4 = 32400;
const char* kA1 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char* kA2 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char* kA3 = "0123456789";
const char* kA4 = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char* kA38 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
const char* kA42 = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+-./?";

int idx(const char* alpha, char c) {
    if (c == 0) return -1;
    const char* p = std::strchr(alpha, c);
    return p ? (int)(p - alpha) : -1;
}

uint64_t getBits(const uint8_t* b, int pos, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[pos + i] & 1);
    return v;
}
void setBits(uint8_t* b, int pos, int n, uint64_t v) {
    for (int i = 0; i < n; i++) b[pos + i] = (uint8_t)((v >> (n - 1 - i)) & 1);
}

std::string trim(const std::string& s) {
    size_t a = 0, e = s.size();
    while (a < e && s[a] == ' ') a++;
    while (e > a && s[e - 1] == ' ') e--;
    return s.substr(a, e - a);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string w;
    while (is >> w) out.push_back(w);
    return out;
}

bool allIn(const std::string& s, const char* alpha) {
    for (char c : s) if (idx(alpha, c) < 0) return false;
    return true;
}
bool isLetters(const std::string& s) { for (char c : s) if (c < 'A' || c > 'Z') return false; return !s.empty(); }
bool isDigits(const std::string& s) { for (char c : s) if (c < '0' || c > '9') return false; return !s.empty(); }

bool isGrid4(const std::string& g) {
    return g.size() == 4 && g[0] >= 'A' && g[0] <= 'R' && g[1] >= 'A' && g[1] <= 'R' && isdigit((unsigned char)g[2]) && isdigit((unsigned char)g[3]);
}

// a standard callsign (base call, no suffix) -> its 28-bit number without the offsets, -1 if not standard
int64_t packBasecall(const std::string& call) {
    const size_t len = call.size();
    if (len < 3) return -1;
    std::string c6;
    if (call.compare(0, 4, "3DA0") == 0 && len > 4 && len <= 7) c6 = "3D0" + call.substr(4);
    else if (call.compare(0, 2, "3X") == 0 && len > 2 && isalpha((unsigned char)call[2]) && len <= 7) c6 = "Q" + call.substr(2);
    else if (isdigit((unsigned char)call[2]) && len <= 6) c6 = call;
    else if (isdigit((unsigned char)call[1]) && len <= 5) c6 = " " + call;
    else return -1;
    while (c6.size() < 6) c6 += ' ';
    if (c6.size() > 6) return -1;
    const int i0 = idx(kA1, c6[0]), i1 = idx(kA2, c6[1]), i2 = idx(kA3, c6[2]), i3 = idx(kA4, c6[3]), i4 = idx(kA4, c6[4]), i5 = idx(kA4, c6[5]);
    if (i0 < 0 || i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0 || i5 < 0) return -1;
    // the letters after the digit must be a run without spaces in between
    for (int k = 3; k < 5; k++) if (c6[k] == ' ' && c6[k + 1] != ' ') return -1;
    return ((((int64_t)i0 * 36 + i1) * 10 + i2) * 27 + i3) * 27 * 27 + (int64_t)i4 * 27 + i5;
}

// a call, a <hashed call> or a token -> 28 bits; -1 if it does not fit
int64_t pack28(const std::string& t) {
    if (t == "DE") return 0;
    if (t == "QRZ") return 1;
    if (t == "CQ") return 2;
    if (t.compare(0, 3, "CQ ") == 0) {
        const std::string m = t.substr(3);
        if (m.size() == 3 && isDigits(m)) return 3 + std::stoi(m);
        if (m.size() >= 1 && m.size() <= 4 && isLetters(m)) {
            std::string r = std::string(4 - m.size(), ' ') + m;
            int64_t v = 0;
            for (char c : r) v = v * 27 + idx(kA4, c);
            return 1003 + v;
        }
        return -1;
    }
    if (t.size() > 2 && t.front() == '<' && t.back() == '>') {
        const std::string c = t.substr(1, t.size() - 2);
        if (c.empty() || c.size() > 11 || !allIn(c, kA38)) return -1;
        return kNtokens + CallHash::hash22(c);
    }
    const int64_t n = packBasecall(t);
    if (n < 0) return -1;
    return kNtokens + kMax22 + n;
}

std::string unpackHashed(uint32_t h, int bits, const CallHash* hash) {
    std::string c;
    if (hash) c = bits == 22 ? hash->find22(h) : bits == 12 ? hash->find12(h) : hash->find10(h);
    return c.empty() ? "<...>" : "<" + c + ">";
}

// 28 bits -> text; empty when invalid
std::string unpack28(uint32_t n, const CallHash* hash) {
    if (n < kNtokens) {
        if (n == 0) return "DE";
        if (n == 1) return "QRZ";
        if (n == 2) return "CQ";
        if (n <= 1002) { char b[16]; snprintf(b, sizeof b, "CQ %03u", n - 3); return b; }
        if (n <= 532443) {
            uint32_t m = n - 1003;
            char c[5] = {};
            for (int k = 3; k >= 0; k--) { c[k] = kA4[m % 27]; m /= 27; }
            const std::string s = trim(c);
            if (s.empty()) return "";
            return "CQ " + s;
        }
        return "";
    }
    n -= kNtokens;
    if (n < kMax22) return unpackHashed(n, 22, hash);
    n -= kMax22;
    char c[7] = {};
    c[5] = kA4[n % 27]; n /= 27;
    c[4] = kA4[n % 27]; n /= 27;
    c[3] = kA4[n % 27]; n /= 27;
    c[2] = kA3[n % 10]; n /= 10;
    c[1] = kA2[n % 36]; n /= 36;
    if (n >= 37) return "";
    c[0] = kA1[n];
    std::string s = trim(c);
    if (s.size() >= 4 && s.compare(0, 3, "3D0") == 0) s = "3DA0" + s.substr(3);
    else if (s.size() >= 2 && s[0] == 'Q' && isalpha((unsigned char)s[1])) s = "3X" + s.substr(1);
    if (s.empty() || s.find(' ') != std::string::npos) return "";
    return s;
}

bool isToken(const std::string& s) { return s == "DE" || s == "QRZ" || s.compare(0, 2, "CQ") == 0; }

std::string report(int isnr) {
    char b[8];
    snprintf(b, sizeof b, "%c%02d", isnr >= 0 ? '+' : '-', std::abs(isnr));
    return b;
}

// 71-bit numbers (free text, telemetry, c58 uses 64): three 32-bit limbs, most significant first
struct Big {
    uint32_t w[3] = {0, 0, 0};
    void mulAdd(uint32_t m, uint32_t a) {
        uint64_t carry = a;
        for (int i = 2; i >= 0; i--) { const uint64_t v = (uint64_t)w[i] * m + carry; w[i] = (uint32_t)v; carry = v >> 32; }
    }
    uint32_t divMod(uint32_t d) {
        uint64_t r = 0;
        for (int i = 0; i < 3; i++) { const uint64_t v = (r << 32) | w[i]; w[i] = (uint32_t)(v / d); r = v % d; }
        return (uint32_t)r;
    }
    bool bit(int i) const { return (w[2 - i / 32] >> (i % 32)) & 1; }   // bit i from the least significant
    void setBit(int i) { w[2 - i / 32] |= 1u << (i % 32); }
};
void bigToBits(const Big& v, uint8_t* b71) { for (int i = 0; i < 71; i++) b71[i] = v.bit(70 - i) ? 1 : 0; }
Big bitsToBig(const uint8_t* b71) { Big v; for (int i = 0; i < 71; i++) if (b71[i]) v.setBit(70 - i); return v; }

std::string stripCall(const std::string& c, int& suffix) {   // K1ABC/R -> K1ABC (1), K1ABC/P -> K1ABC (2)
    suffix = 0;
    if (c.size() > 2 && c.compare(c.size() - 2, 2, "/R") == 0) { suffix = 1; return c.substr(0, c.size() - 2); }
    if (c.size() > 2 && c.compare(c.size() - 2, 2, "/P") == 0) { suffix = 2; return c.substr(0, c.size() - 2); }
    return c;
}

// the grid / report field of a standard message: g15 + the R bit; false if it is not one
bool packGrid(const std::vector<std::string>& t, size_t from, uint32_t& g, int& ir) {
    ir = 0;
    const size_t n = t.size() - from;
    if (n == 0) { g = kMaxGrid4 + 1; return true; }
    std::string a = t[from];
    if (n == 2) {
        if (a != "R" || !isGrid4(t[from + 1])) return false;
        ir = 1; a = t[from + 1];
    } else if (n != 1) return false;
    if (a == "RRR") { g = kMaxGrid4 + 2; return true; }
    if (a == "RR73") { g = kMaxGrid4 + 3; return true; }
    if (a == "73") { g = kMaxGrid4 + 4; return true; }
    if (isGrid4(a)) { g = (uint32_t)((((a[0] - 'A') * 18 + (a[1] - 'A')) * 10 + (a[2] - '0')) * 10 + (a[3] - '0')); return true; }
    std::string r = a;
    if (r.size() >= 2 && r[0] == 'R' && (r[1] == '+' || r[1] == '-')) { ir = 1; r = r.substr(1); }
    if (r.size() >= 2 && r.size() <= 3 && (r[0] == '+' || r[0] == '-') && isDigits(r.substr(1))) {
        int dd = std::stoi(r.substr(1));
        if (r[0] == '-') dd = -dd;
        if (dd < -50 || dd > 49) return false;
        int irpt = 35 + dd;
        if (irpt < 5) irpt += 101;
        g = kMaxGrid4 + (uint32_t)irpt;
        return true;
    }
    return false;
}

bool packStandard(const std::vector<std::string>& in, uint8_t* b) {
    std::vector<std::string> t = in;
    // "CQ DX ..." / "CQ 123 ...": the modifier belongs to the first field
    if (t.size() >= 3 && t[0] == "CQ" && ((t[1].size() <= 4 && isLetters(t[1]) && pack28(t[2]) >= 0 && t[2] != "DE") ||
                                          (t[1].size() == 3 && isDigits(t[1])))) {
        t[0] = "CQ " + t[1];
        t.erase(t.begin() + 1);
    }
    if (t.size() < 2) return false;
    int sa = 0, sb = 0;
    const std::string ca = isToken(t[0]) ? t[0] : stripCall(t[0], sa);
    const std::string cb = stripCall(t[1], sb);
    if ((sa == 1 && sb == 2) || (sa == 2 && sb == 1)) return false;
    const int64_t na = pack28(ca), nb = pack28(cb);
    if (na < 0 || nb < 0 || isToken(cb)) return false;
    uint32_t g;
    int ir;
    if (!packGrid(t, 2, g, ir)) return false;
    if (na <= 2 && na >= 0 && ir) return false;
    const int i3 = (sa == 2 || sb == 2) ? 2 : 1;
    setBits(b, 0, 28, (uint64_t)na); b[28] = sa ? 1 : 0;
    setBits(b, 29, 28, (uint64_t)nb); b[57] = sb ? 1 : 0;
    b[58] = (uint8_t)ir;
    setBits(b, 59, 15, g);
    setBits(b, 74, 3, (uint64_t)i3);
    return true;
}

bool isNonstandard(const std::string& c) {
    if (c.empty() || c.size() > 11 || !allIn(c, kA38)) return false;
    int s;
    return packBasecall(stripCall(c, s)) < 0;
}

bool packType4(const std::vector<std::string>& t, uint8_t* b) {
    if (t.size() < 2 || t.size() > 3) return false;
    int icq = 0, iflip = 0, nrpt = 0;
    std::string plain, hashed;
    if (t[0] == "CQ") {
        if (t.size() != 2 || !isNonstandard(t[1])) return false;
        icq = 1; plain = t[1];
    } else {
        auto bare = [](const std::string& s) { return s.size() > 2 && s.front() == '<' && s.back() == '>' ? s.substr(1, s.size() - 2) : s; };
        const bool n0 = isNonstandard(bare(t[0])), n1 = isNonstandard(bare(t[1]));
        const bool h0 = t[0].front() == '<', h1 = t[1].front() == '<';
        if (h0 && !h1 && n1) { hashed = bare(t[0]); plain = t[1]; iflip = 0; }
        else if (h1 && !h0 && n0) { hashed = bare(t[1]); plain = t[0]; iflip = 1; }
        else if (!h0 && !h1 && n1 && !n0) { hashed = t[0]; plain = t[1]; iflip = 0; }
        else if (!h0 && !h1 && n0 && !n1) { hashed = t[1]; plain = t[0]; iflip = 1; }
        else return false;
        if (hashed.empty() || !allIn(hashed, kA38) || hashed.size() > 11) return false;
        if (t.size() == 3) {
            if (t[2] == "RRR") nrpt = 1;
            else if (t[2] == "RR73") nrpt = 2;
            else if (t[2] == "73") nrpt = 3;
            else return false;
        }
    }
    uint64_t n58 = 0;
    const std::string p = std::string(11 - plain.size(), ' ') + plain;
    for (char c : p) n58 = n58 * 38 + (uint64_t)idx(kA38, c);
    const uint32_t h12 = hashed.empty() ? 0 : CallHash::hash22(hashed) >> 10;
    setBits(b, 0, 12, h12);
    setBits(b, 12, 58, n58);
    b[70] = (uint8_t)iflip;
    setBits(b, 71, 2, (uint64_t)nrpt);
    b[73] = (uint8_t)icq;
    setBits(b, 74, 3, 4);
    return true;
}

bool packFreeText(const std::string& s, uint8_t* b) {
    if (s.size() > 13 || !allIn(s, kA42)) return false;
    const std::string p = s + std::string(13 - s.size(), ' ');
    Big v;
    for (char c : p) v.mulAdd(42, (uint32_t)idx(kA42, c));
    bigToBits(v, b);
    setBits(b, 71, 3, 0);
    setBits(b, 74, 3, 0);
    return true;
}

bool packTelemetry(const std::string& s, uint8_t* b) {
    if (s.size() != 18 || s[0] > '7') return false;
    Big v;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v.mulAdd(16, (uint32_t)d);
    }
    bigToBits(v, b);
    setBits(b, 71, 3, 5);
    setBits(b, 74, 3, 0);
    return true;
}

} // namespace

// ---------------------------------------------------------------- hashes

uint32_t CallHash::hash22(const std::string& call) {
    std::string c = call.substr(0, 11);
    while (c.size() < 11) c += ' ';
    uint64_t n = 0;
    for (char ch : c) { int j = idx(kA38, ch); if (j < 0) j = 0; n = n * 38 + (uint64_t)j; }
    return (uint32_t)((47055833459ull * n) >> 42) & 0x3FFFFF;
}
void CallHash::add(const std::string& call) {
    if (call.empty() || call.size() > 11 || call.front() == '<' || !allIn(call, kA38)) return;
    if (m_.size() > 5000) m_.clear();
    m_[hash22(call)] = call;
}
std::string CallHash::find22(uint32_t h) const { auto i = m_.find(h); return i == m_.end() ? "" : i->second; }
std::string CallHash::find12(uint32_t h) const { for (const auto& e : m_) if ((e.first >> 10) == h) return e.second; return ""; }
std::string CallHash::find10(uint32_t h) const { for (const auto& e : m_) if ((e.first >> 12) == h) return e.second; return ""; }

// ---------------------------------------------------------------- pack / unpack

bool pack77(const std::string& text, uint8_t* b) {
    std::string up;
    for (char c : text) up += (char)toupper((unsigned char)c);
    const std::vector<std::string> t = split(up);
    std::string joined;
    for (size_t i = 0; i < t.size(); i++) joined += (i ? " " : "") + t[i];
    std::memset(b, 0, 77);
    if (t.empty()) return false;
    if (t.size() >= 2 && packStandard(t, b)) return true;
    std::memset(b, 0, 77);
    if (packType4(t, b)) return true;
    std::memset(b, 0, 77);
    if (t.size() == 1 && packTelemetry(joined, b)) return true;
    std::memset(b, 0, 77);
    return packFreeText(joined, b);
}

std::string unpack77(const uint8_t* b, CallHash* hash) {
    const int i3 = (int)getBits(b, 74, 3);
    const int n3 = (int)getBits(b, 71, 3);
    bool any = false;
    for (int i = 0; i < 77; i++) any |= b[i] != 0;
    if (!any) return "";
    auto learn = [&](const std::string& c) {
        if (!hash || c.empty() || c[0] == '<' || isToken(c)) return;
        int s;
        hash->add(stripCall(c, s));
        hash->add(c);
    };
    if (i3 == 1 || i3 == 2) {
        const uint32_t na = (uint32_t)getBits(b, 0, 28), nb = (uint32_t)getBits(b, 29, 28);
        const int ipa = b[28], ipb = b[57], ir = b[58];
        const uint32_t g = (uint32_t)getBits(b, 59, 15);
        std::string ca = unpack28(na, hash), cb = unpack28(nb, hash);
        if (ca.empty() || cb.empty() || isToken(cb)) return "";
        const char* suf = i3 == 1 ? "/R" : "/P";
        if (ipa && !isToken(ca) && ca[0] != '<') ca += suf;
        if (ipb && cb[0] != '<') cb += suf;
        std::string out = ca + " " + cb;
        if (g <= kMaxGrid4) {
            char gr[5] = {(char)('A' + g / 1800), (char)('A' + (g / 100) % 18), (char)('0' + (g / 10) % 10), (char)('0' + g % 10), 0};
            out += ir ? std::string(" R ") + gr : std::string(" ") + gr;
        } else {
            const int irpt = (int)(g - kMaxGrid4);
            if (isToken(ca) && ca.compare(0, 2, "CQ") == 0 && irpt >= 2) return "";
            if (irpt == 2) out += " RRR";
            else if (irpt == 3) out += " RR73";
            else if (irpt == 4) out += " 73";
            else if (irpt >= 5) {
                int isnr = irpt - 35;
                if (isnr > 50) isnr -= 101;
                out += std::string(" ") + (ir ? "R" : "") + report(isnr);
            }
        }
        learn(ca); learn(cb);
        return out;
    }
    if (i3 == 0 && n3 == 0) {
        Big v = bitsToBig(b);
        char c[14] = {};
        for (int k = 12; k >= 0; k--) c[k] = kA42[v.divMod(42)];
        const std::string s = trim(c);
        return s;
    }
    if (i3 == 0 && n3 == 1) {
        const std::string c1 = unpack28((uint32_t)getBits(b, 0, 28), hash), c2 = unpack28((uint32_t)getBits(b, 28, 28), hash);
        if (c1.empty() || c2.empty() || isToken(c1) || isToken(c2)) return "";
        const std::string h = unpackHashed((uint32_t)getBits(b, 56, 10), 10, hash);
        const int rpt = 2 * (int)getBits(b, 66, 5) - 30;
        learn(c1); learn(c2);
        return c1 + " RR73; " + c2 + " " + h + " " + report(rpt);
    }
    if (i3 == 0 && (n3 == 3 || n3 == 4)) {
        const std::string c1 = unpack28((uint32_t)getBits(b, 0, 28), hash), c2 = unpack28((uint32_t)getBits(b, 28, 28), hash);
        if (c1.empty() || c2.empty() || isToken(c2)) return "";
        const int ir = b[56], intx = (int)getBits(b, 57, 4), ncl = (int)getBits(b, 61, 3), isec = (int)getBits(b, 64, 7);
        if (isec < 1 || isec > 86) return "";
        const int ntx = intx + (n3 == 3 ? 1 : 17);
        char x[16];
        snprintf(x, sizeof x, "%d%c", ntx, 'A' + ncl);
        learn(c1); learn(c2);
        return c1 + " " + c2 + (ir ? " R " : " ") + x + " " + kArrlSections[isec - 1];
    }
    if (i3 == 0 && n3 == 5) {
        Big v = bitsToBig(b);
        char hx[19] = {};
        for (int k = 17; k >= 0; k--) hx[k] = "0123456789ABCDEF"[v.divMod(16)];
        std::string s = hx;
        const size_t nz = s.find_first_not_of('0');
        return nz == std::string::npos ? "0" : s.substr(nz);
    }
    if (i3 == 3) {
        const int itu = b[0];
        const std::string c1 = unpack28((uint32_t)getBits(b, 1, 28), hash), c2 = unpack28((uint32_t)getBits(b, 29, 28), hash);
        if (c1.empty() || c2.empty() || isToken(c2)) return "";
        const int ir = b[57], irpt = (int)getBits(b, 58, 3), nexch = (int)getBits(b, 61, 13);
        std::string ex;
        if (nexch > 8000) {
            if (nexch - 8000 > 171) return "";
            ex = rttyMult(nexch - 8001);
        } else {
            if (nexch < 1) return "";
            char s[8]; snprintf(s, sizeof s, "%04d", nexch); ex = s;
        }
        char r[4] = {'5', (char)('0' + irpt + 2), '9', 0};
        learn(c1); learn(c2);
        return std::string(itu ? "TU; " : "") + c1 + " " + c2 + (ir ? " R " : " ") + r + " " + ex;
    }
    if (i3 == 4) {
        const uint32_t h12 = (uint32_t)getBits(b, 0, 12);
        uint64_t n58 = getBits(b, 12, 58);
        const int iflip = b[70], nrpt = (int)getBits(b, 71, 2), icq = b[73];
        char c[12] = {};
        for (int k = 10; k >= 0; k--) { c[k] = kA38[n58 % 38]; n58 /= 38; }
        const std::string plain = trim(c);
        if (plain.empty()) return "";
        if (hash) hash->add(plain);
        if (icq) return "CQ " + plain;
        const std::string h = unpackHashed(h12, 12, hash);
        std::string out = iflip ? plain + " " + h : h + " " + plain;
        if (nrpt == 1) out += " RRR";
        else if (nrpt == 2) out += " RR73";
        else if (nrpt == 3) out += " 73";
        return out;
    }
    if (i3 == 5) {
        const std::string c1 = unpackHashed((uint32_t)getBits(b, 0, 12), 12, hash);
        const std::string c2 = unpackHashed((uint32_t)getBits(b, 12, 22), 22, hash);
        const int ir = b[34], irpt = (int)getBits(b, 35, 3), ser = (int)getBits(b, 38, 11);
        uint32_t g6 = (uint32_t)getBits(b, 49, 25);
        if (g6 > 18662399) return "";
        char gr[7];
        gr[5] = (char)('A' + g6 % 24); g6 /= 24;
        gr[4] = (char)('A' + g6 % 24); g6 /= 24;
        gr[3] = (char)('0' + g6 % 10); g6 /= 10;
        gr[2] = (char)('0' + g6 % 10); g6 /= 10;
        gr[1] = (char)('A' + g6 % 18); g6 /= 18;
        gr[0] = (char)('A' + g6);
        gr[6] = 0;
        char ex[16];
        snprintf(ex, sizeof ex, "%02d%04d", 52 + irpt, ser);
        return c1 + " " + c2 + (ir ? " R " : " ") + ex + " " + gr;
    }
    return "";
}

void callAndGrid(const std::string& msg, std::string& call, std::string& grid) {
    call.clear(); grid.clear();
    std::vector<std::string> t = split(msg);
    if (t.empty() || msg.find(';') != std::string::npos) return;
    auto bare = [](std::string s) {
        if (s.size() > 2 && s.front() == '<' && s.back() == '>') s = s.substr(1, s.size() - 2);
        return s == "..." ? std::string() : s;
    };
    auto isGrid = [](const std::string& g) {
        if (g == "RR73") return false;
        if (isGrid4(g)) return true;
        return g.size() == 6 && isGrid4(g.substr(0, 4)) && g[4] >= 'A' && g[4] <= 'X' && g[5] >= 'A' && g[5] <= 'X';
    };
    size_t first = 0;
    if (t[0] == "CQ") {
        first = 1;
        if (t.size() >= 3 && (isLetters(t[1]) || isDigits(t[1])) && t[1].size() <= 4 && !isGrid(t[2]) && t[2] != "R") first = 2;
    }
    const std::string last = t.back();
    if (t.size() >= 2 && isGrid(last)) {
        grid = last;
        size_t i = t.size() - 2;
        if (t[i] == "R" && i > 0) i--;
        call = bare(t[i]);
        if (first == 0 && t.size() >= 3 && i == 0) call = bare(t[0]);   // "K1ABC FN42 37" (WSPR)
    } else if (first > 0 && t.size() > first) {
        call = bare(t[first]);
    } else if (t.size() >= 2) {
        call = bare(t[1]);
    } else {
        call.clear();
    }
    if (isDigits(call) || call == "R" || call == "RRR" || call == "73") call.clear();
}

bool gridToLatLon(const std::string& g, double& lat, double& lon) {
    if (g.size() < 4 || !isGrid4(g.substr(0, 4))) return false;
    lon = (g[0] - 'A') * 20.0 - 180 + (g[2] - '0') * 2.0;
    lat = (g[1] - 'A') * 10.0 - 90 + (g[3] - '0') * 1.0;
    if (g.size() >= 6 && g[4] >= 'A' && g[4] <= 'X' && g[5] >= 'A' && g[5] <= 'X') {
        lon += (g[4] - 'A') * (2.0 / 24) + 1.0 / 24;
        lat += (g[5] - 'A') * (1.0 / 24) + 0.5 / 24;
    } else {
        lon += 1.0; lat += 0.5;
    }
    return true;
}

} // namespace ftx
} // namespace dect2
