// MPE, time slicing and MPE-FEC (ETSI EN 301 192 clauses 7 and 9): see mpe_fec.h.
#include "dect2/tv/mpe_fec.h"
#include "dect2/tv/ts.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace mpe {

// ============================================================================ RS(255,191) over GF(256), p(x) = 0x11D, roots alpha^0..63
namespace {
constexpr int kN = 255, kK = 191, kR = 64;
struct Gf {
    uint8_t exp[512], log[256];
    uint8_t gen[kR + 1];   // g(x) = sum gen[i] x^i, monic
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
        std::memset(gen, 0, sizeof gen);
        gen[0] = 1;
        for (int j = 0; j < kR; j++) {   // multiply by (x + alpha^j)
            for (int i = j + 1; i > 0; i--) gen[i] = (uint8_t)(gen[i - 1] ^ mul(gen[i], exp[j]));
            gen[0] = mul(gen[0], exp[j]);
        }
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 255 - log[b]) % 255] : 0; }
    uint8_t pw(int e) const { e %= 255; if (e < 0) e += 255; return exp[e]; }
};
const Gf& gf() { static const Gf g; return g; }
}

void rsEncode255(const uint8_t* in, uint8_t* parity) {
    const Gf& G = gf();
    uint8_t p[kR] = {0};   // p[i]: coefficient of x^i of the remainder
    for (int n = 0; n < kK; n++) {
        const uint8_t fb = in[n] ^ p[kR - 1];
        for (int i = kR - 1; i > 0; i--) p[i] = (uint8_t)(p[i - 1] ^ G.mul(fb, G.gen[i]));
        p[0] = G.mul(fb, G.gen[0]);
    }
    for (int k = 0; k < kR; k++) parity[k] = p[kR - 1 - k];
}

int rsDecode255(uint8_t* c, const int* eras, int nEras) {
    const Gf& G = gf();
    if (nEras > kR) return -1;
    // syndromes S_j = c(alpha^j), c(x) = sum c[i] x^(254 - i)
    uint8_t S[kR];
    bool zero = true;
    for (int j = 0; j < kR; j++) {
        uint8_t s = 0;
        const uint8_t a = G.exp[j];
        for (int i = 0; i < kN; i++) s = (uint8_t)(G.mul(s, a) ^ c[i]);
        S[j] = s; zero &= s == 0;
    }
    if (zero) return 0;
    // erasure locator Gamma(x) = prod (1 + X x), X = alpha^(254 - pos); Berlekamp-Massey started from it
    uint8_t lam[kR + 1] = {0}, b[kR + 1] = {0}, t[kR + 1];
    lam[0] = 1;
    for (int e = 0; e < nEras; e++) {
        const uint8_t X = G.pw(kN - 1 - eras[e]);
        for (int i = e + 1; i > 0; i--) lam[i] ^= G.mul(lam[i - 1], X);
    }
    std::memcpy(b, lam, sizeof lam);
    int L = nEras;
    for (int r = nEras + 1; r <= kR; r++) {
        uint8_t d = 0;
        for (int i = 0; i < r; i++) if (i <= kR) d ^= G.mul(lam[i], S[r - i - 1]);
        if (d == 0) { std::memmove(b + 1, b, kR); b[0] = 0; continue; }
        t[0] = lam[0];
        for (int i = 1; i <= kR; i++) t[i] = (uint8_t)(lam[i] ^ G.mul(d, b[i - 1]));
        if (2 * L <= r + nEras - 1) {
            L = r + nEras - L;
            for (int i = 0; i <= kR; i++) b[i] = G.div(lam[i], d);
        } else { std::memmove(b + 1, b, kR); b[0] = 0; }
        std::memcpy(lam, t, sizeof t);
    }
    int deg = 0;
    for (int i = 0; i <= kR; i++) if (lam[i]) deg = i;
    if (deg > kR) return -1;
    // Omega(x) = S(x) Lambda(x) mod x^64
    uint8_t om[kR] = {0};
    for (int i = 0; i < kR; i++) for (int j = 0; j <= i; j++) om[i] ^= G.mul(S[j], lam[i - j]);
    // Chien search and Forney (first root alpha^0: Y = X Omega(1/X) / Lambda'(1/X))
    int found = 0, changed = 0;
    for (int pos = 0; pos < kN; pos++) {
        const int e = kN - 1 - pos;
        const uint8_t xi = G.pw(-e);   // 1 / X
        uint8_t v = 0, xp = 1;
        for (int i = 0; i <= deg; i++) { v ^= G.mul(lam[i], xp); xp = G.mul(xp, xi); }
        if (v) continue;
        found++;
        uint8_t num = 0, den = 0;
        xp = 1;
        for (int i = 0; i < kR; i++) { num ^= G.mul(om[i], xp); xp = G.mul(xp, xi); }
        xp = 1;
        for (int i = 1; i <= deg; i += 2) { den ^= G.mul(lam[i], xp); xp = G.mul(xp, G.mul(xi, xi)); }
        if (!den) return -1;
        const uint8_t y = G.mul(G.pw(e), G.div(num, den));
        if (y) { c[pos] ^= y; changed++; }
    }
    if (found != deg) return -1;
    return changed;
}

// ============================================================================ demultiplexer
namespace {
RtParams rtOf(const uint8_t* p) {
    RtParams r;
    r.deltaT = (p[0] << 4) | (p[1] >> 4);
    r.tableBoundary = (p[1] >> 3) & 1;
    r.frameBoundary = (p[1] >> 2) & 1;
    r.address = ((p[1] & 3) << 16) | (p[2] << 8) | p[3];
    return r;
}
void putRt(uint8_t* p, const RtParams& r) {
    p[0] = (uint8_t)(r.deltaT >> 4);
    p[1] = (uint8_t)(((r.deltaT & 15) << 4) | (r.tableBoundary ? 8 : 0) | (r.frameBoundary ? 4 : 0) | ((r.address >> 16) & 3));
    p[2] = (uint8_t)(r.address >> 8); p[3] = (uint8_t)r.address;
}
// length of the IP datagram starting at d (0: not one)
size_t ipLength(const uint8_t* d, size_t avail) {
    if (avail < 20) return 0;
    const int v = d[0] >> 4;
    if (v == 4) { const size_t n = (size_t)(d[2] << 8 | d[3]); return n >= 20 && n <= avail ? n : 0; }
    if (v == 6 && avail >= 40) { const size_t n = 40 + (size_t)(d[4] << 8 | d[5]); return n <= avail ? n : 0; }
    return 0;
}
}

struct MpeDemux::Stream {
    int pid = 0, cc = -1;
    std::vector<uint8_t> sec;     // the section being collected
    bool collecting = false;
    bool fecSeen = false;
    bool noData = false;           // a frame begun by its RS data (its application data went out before the stream was recognised)
    bool rowsGuess = false;        // rows not yet known from an MPE-FEC section (the largest frame assumed)
    // the MPE-FEC frame being collected
    int rows = 0;
    bool open = false, fecPart = false;
    int padding = 0;
    std::vector<uint8_t> adt, adtOk, rst, rstOk;
    size_t adtEnd = 0;             // one past the last application byte received (the table boundary)
    bool tableEnd = false;
    int lastDelta = -1;
};

MpeDemux::MpeDemux() = default;
MpeDemux::~MpeDemux() = default;

void MpeDemux::reset() { streams_.clear(); st_ = Stats(); }

void MpeDemux::feed(const uint8_t* pk, size_t count) {
    for (size_t n = 0; n < count; n++) {
        const uint8_t* p = pk + n * 188;
        if (p[0] != 0x47) continue;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid < 0x20 || pid == 0x1FFF) continue;
        const bool tei = p[1] & 0x80, pusi = p[1] & 0x40;
        const int afc = (p[3] >> 4) & 3, cc = p[3] & 15;
        int off = 4;
        if (afc == 2 || afc == 0) continue;
        if (afc == 3) off += 1 + p[4];
        if (off >= 188) continue;
        auto it = streams_.find(pid);
        if (it == streams_.end()) {
            // a new stream is taken when a section starts with an MPE or MPE-FEC table id
            if (tei || !pusi) continue;
            const int ptr = p[off];
            if (off + 1 + ptr >= 188) continue;
            const uint8_t tid = p[off + 1 + ptr];
            if (tid != 0x3E && tid != 0x78) continue;
            auto s = std::make_unique<Stream>();
            s->pid = pid;
            it = streams_.emplace(pid, std::move(s)).first;
        }
        Stream& s = *it->second;
        if (tei) { s.collecting = false; s.sec.clear(); s.cc = -1; continue; }
        if (s.cc >= 0 && cc != ((s.cc + 1) & 15)) { s.collecting = false; s.sec.clear(); }
        s.cc = cc;
        auto take = [&](const uint8_t* d, size_t len) {
            // append to the section being collected; complete sections go to section()
            while (len > 0) {
                if (!s.collecting) return;
                const size_t want = s.sec.size() < 3 ? 3 - s.sec.size() : (size_t)(((s.sec[1] & 15) << 8) | s.sec[2]) + 3 - s.sec.size();
                const size_t k = std::min(want, len);
                s.sec.insert(s.sec.end(), d, d + k);
                d += k; len -= k;
                if (s.sec.size() >= 3) {
                    const size_t total = (size_t)(((s.sec[1] & 15) << 8) | s.sec[2]) + 3;
                    if (s.sec.size() == total) {
                        section(s, s.sec.data(), total);
                        s.sec.clear();
                        s.collecting = len > 0 && d[0] != 0xFF;   // another section in the same packet, or stuffing
                    }
                }
            }
        };
        if (pusi) {
            const int ptr = p[off];
            const uint8_t* d = p + off + 1;
            const size_t avail = (size_t)(188 - off - 1);
            if ((size_t)ptr > avail) { s.collecting = false; s.sec.clear(); continue; }
            take(d, (size_t)ptr);           // the end of the previous section
            s.sec.clear(); s.collecting = d[ptr] != 0xFF;
            take(d + ptr, avail - (size_t)ptr);
        } else take(p + off, (size_t)(188 - off));
    }
}

void MpeDemux::section(Stream& s, const uint8_t* sec, size_t len) {
    st_.sections++;
    st_.pid = s.pid;
    if (len < 16) { st_.crcErrors++; return; }
    const uint32_t crc = (uint32_t)sec[len - 4] << 24 | (uint32_t)sec[len - 3] << 16 | (uint32_t)sec[len - 2] << 8 | sec[len - 1];
    if (mpegCrc32(sec, (int)len - 4) != crc) { st_.crcErrors++; return; }
    const uint8_t tid = sec[0];
    if (tid == 0x78) {
        st_.fecSections++;
        s.fecSeen = true; st_.mpeFec = true;
        const int rows = (int)len - 16;
        if (rows != 256 && rows != 512 && rows != 768 && rows != 1024) return;
        const RtParams rt = rtOf(sec + 8);
        if (s.open && s.rowsGuess && !s.fecPart && s.rows != rows) {
            // the first frame of the stream: its application data went in before the frame's row count was known; the linear addresses stay
            s.rows = rows;
            s.adt.resize((size_t)kK * rows); s.adtOk.resize((size_t)kK * rows);
            s.rst.assign((size_t)kR * rows, 0); s.rstOk.assign((size_t)kR * rows, 0);
        }
        s.rowsGuess = false;
        if (!s.open || s.rows != rows) {   // RS data of a frame whose application data was all lost: nothing to rebuild
            if (s.open) finishFrame(s);
            s.open = true; s.rows = rows;
            s.adt.assign((size_t)kK * rows, 0); s.adtOk.assign((size_t)kK * rows, 0);
            s.rst.assign((size_t)kR * rows, 0); s.rstOk.assign((size_t)kR * rows, 0);
            s.adtEnd = 0; s.tableEnd = false; s.noData = true;
        }
        s.fecPart = true;
        s.padding = sec[3];
        st_.rows = rows;
        if (rt.address + rows <= kR * rows) {
            std::memcpy(&s.rst[(size_t)rt.address], sec + 12, (size_t)rows);
            std::memset(&s.rstOk[(size_t)rt.address], 1, (size_t)rows);
        }
        if (rt.frameBoundary) finishFrame(s);
        return;
    }
    if (tid != 0x3E) return;
    if (!(sec[1] & 0x80)) return;   // checksum form (not used by DVB-H)
    const bool llc = sec[5] & 2;
    const uint8_t* pay = sec + 12;
    const size_t plen = len - 16;
    const bool rt = rtHint_ || s.fecSeen;
    if (!rt) {   // plain MPE: the datagram as it comes
        if (!llc && cb_) { st_.datagrams++; cb_(pay, plen, true, s.pid); }
        return;
    }
    const RtParams r = rtOf(sec + 8);
    if (r.deltaT != s.lastDelta) { s.lastDelta = r.deltaT; }
    if (rtHint_) { st_.timeSlicing = true; st_.lastDeltaTms = r.deltaT * 10; }
    if (!s.fecSeen) {
        if (!llc && cb_) { st_.datagrams++; cb_(pay, plen, true, s.pid); }
        if (r.frameBoundary) st_.bursts++;
        return;
    }
    // an MPE-FEC stream: into the application data table of the frame
    if (s.open && (r.address == 0 || s.fecPart)) finishFrame(s);
    if (!s.open) {
        const int rows = s.rows ? s.rows : 1024;
        s.rowsGuess = s.rows == 0;
        s.open = true; s.rows = rows; s.fecPart = false;
        s.adt.assign((size_t)kK * rows, 0); s.adtOk.assign((size_t)kK * rows, 0);
        s.rst.assign((size_t)kR * rows, 0); s.rstOk.assign((size_t)kR * rows, 0);
        s.adtEnd = 0; s.tableEnd = false; s.noData = false;
    }
    if ((size_t)r.address + plen <= s.adt.size()) {
        std::memcpy(&s.adt[(size_t)r.address], pay, plen);
        std::memset(&s.adtOk[(size_t)r.address], 1, plen);
        s.adtEnd = std::max(s.adtEnd, (size_t)r.address + plen);
    }
    if (r.tableBoundary) s.tableEnd = true;
    if (r.frameBoundary) finishFrame(s);
}

void MpeDemux::finishFrame(Stream& s) {
    if (!s.open) return;
    s.open = false;
    if (s.noData) return;
    st_.frames++;
    if (st_.timeSlicing) st_.bursts++;
    const int R = s.rows;
    // the padding columns are zeros the receiver knows; behind the table boundary everything up to them is padding as well
    const size_t padFrom = (size_t)(kK - std::min(s.padding, kK)) * R;
    for (size_t a = padFrom; a < s.adt.size(); a++) { s.adt[a] = 0; s.adtOk[a] = 1; }
    if (s.tableEnd) for (size_t a = s.adtEnd; a < padFrom; a++) if (!s.adtOk[a]) { s.adt[a] = 0; s.adtOk[a] = 1; }
    bool anyRs = false;
    for (uint8_t v : s.rstOk) anyRs |= v != 0;
    if (anyRs && R > 0) {
        uint8_t w[kN];
        int er[kN];
        for (int row = 0; row < R; row++) {
            int ne = 0;
            for (int c = 0; c < kK; c++) { const size_t a = (size_t)c * R + row; w[c] = s.adt[a]; if (!s.adtOk[a]) er[ne++] = c; }
            for (int c = 0; c < kR; c++) { const size_t a = (size_t)c * R + row; w[kK + c] = s.rst[a]; if (!s.rstOk[a]) er[ne++] = kK + c; }
            bool adtBad = false;
            for (int i = 0; i < ne; i++) adtBad |= er[i] < kK;
            if (!adtBad) { st_.rowsClean++; continue; }
            const int k = ne <= kR ? rsDecode255(w, er, ne) : -1;
            if (k < 0) { st_.rowsFailed++; continue; }
            st_.rowsCorrected++; st_.bytesCorrected += (uint64_t)k;
            for (int c = 0; c < kK; c++) { const size_t a = (size_t)c * R + row; s.adt[a] = w[c]; s.adtOk[a] = 1; }
        }
    }
    emitTable(s, s.adt, s.adtOk, padFrom);
}

void MpeDemux::emitTable(Stream& s, const std::vector<uint8_t>& adt, const std::vector<uint8_t>& ok, size_t used) {
    size_t pos = 0;
    while (pos < used) {
        const size_t n = ipLength(&adt[pos], used - pos);
        if (!n) break;
        bool rel = true;
        for (size_t i = 0; i < n && rel; i++) rel = ok[pos + i] != 0;
        st_.datagrams++;
        if (!rel) st_.datagramsBad++;
        if (cb_) cb_(&adt[pos], n, rel, s.pid);
        pos += n;
    }
}

void MpeDemux::flush() { for (auto& kv : streams_) finishFrame(*kv.second); }

// ============================================================================ encoder
void MpeEncoder::packetise(const std::vector<uint8_t>& sec, std::vector<uint8_t>& out) {
    size_t i = 0;
    bool first = true;
    while (i < sec.size()) {
        uint8_t p[188];
        std::memset(p, 0xFF, 188);
        p[0] = 0x47; p[1] = (uint8_t)((first ? 0x40 : 0) | ((c_.pid >> 8) & 0x1F)); p[2] = (uint8_t)c_.pid; p[3] = (uint8_t)(0x10 | (cc_++ & 15));
        int off = 4;
        if (first) p[off++] = 0;
        const size_t k = std::min(sec.size() - i, (size_t)(188 - off));
        std::memcpy(p + off, &sec[i], k);
        i += k; first = false;
        out.insert(out.end(), p, p + 188);
    }
}

bool MpeEncoder::frame(const std::vector<std::vector<uint8_t>>& dg, std::vector<uint8_t>& out) {
    const int R = c_.rows;
    std::vector<uint8_t> adt((size_t)kK * R, 0);
    size_t addr = 0;
    std::vector<size_t> at;
    for (const auto& d : dg) { if (addr + d.size() > adt.size()) return false; at.push_back(addr); std::memcpy(&adt[addr], d.data(), d.size()); addr += d.size(); }
    const int padding = (int)((adt.size() - addr) / (size_t)R);
    const bool rt = c_.mpeFec || c_.timeSlicing;
    const int delta = c_.timeSlicing ? std::max(1, std::min(4095, c_.deltaTms / 10)) : (frameNo_ & 0xFFF);
    const int nRs = c_.mpeFec ? kR - std::max(0, std::min(kR, c_.punctured)) : 0;
    for (size_t i = 0; i < dg.size(); i++) {
        std::vector<uint8_t> s(12);
        s[0] = 0x3E;
        const size_t slen = 9 + dg[i].size() + 4;
        s[1] = (uint8_t)(0xB0 | (slen >> 8)); s[2] = (uint8_t)slen;
        s[3] = c_.mac[5]; s[4] = c_.mac[4];
        s[5] = 0xC1;   // reserved 11, not scrambled, no LLC/SNAP, current
        s[6] = 0; s[7] = 0;
        if (rt) {
            RtParams r;
            r.deltaT = delta; r.address = (int)at[i];
            r.tableBoundary = c_.mpeFec ? i + 1 == dg.size() : true;
            r.frameBoundary = i + 1 == dg.size() && nRs == 0;
            putRt(&s[8], r);
        } else { s[8] = c_.mac[3]; s[9] = c_.mac[2]; s[10] = c_.mac[1]; s[11] = c_.mac[0]; }
        s.insert(s.end(), dg[i].begin(), dg[i].end());
        const uint32_t crc = mpegCrc32(s.data(), (int)s.size());
        s.push_back((uint8_t)(crc >> 24)); s.push_back((uint8_t)(crc >> 16)); s.push_back((uint8_t)(crc >> 8)); s.push_back((uint8_t)crc);
        packetise(s, out);
    }
    if (nRs > 0) {
        std::vector<uint8_t> rst((size_t)kR * R);
        uint8_t row[kK], par[kR];
        for (int r = 0; r < R; r++) {
            for (int c = 0; c < kK; c++) row[c] = adt[(size_t)c * R + r];
            rsEncode255(row, par);
            for (int c = 0; c < kR; c++) rst[(size_t)c * R + r] = par[c];
        }
        for (int c = 0; c < nRs; c++) {
            std::vector<uint8_t> s(12);
            s[0] = 0x78;
            const size_t slen = 9 + (size_t)R + 4;
            s[1] = (uint8_t)(0xB0 | (slen >> 8)); s[2] = (uint8_t)slen;
            s[3] = (uint8_t)padding; s[4] = 0xFF; s[5] = 0xFF;
            s[6] = (uint8_t)c; s[7] = (uint8_t)(nRs - 1);
            RtParams r; r.deltaT = delta; r.address = c * R; r.tableBoundary = c + 1 == nRs; r.frameBoundary = c + 1 == nRs;
            putRt(&s[8], r);
            s.insert(s.end(), rst.begin() + (long)c * R, rst.begin() + (long)(c + 1) * R);
            const uint32_t crc = mpegCrc32(s.data(), (int)s.size());
            s.push_back((uint8_t)(crc >> 24)); s.push_back((uint8_t)(crc >> 16)); s.push_back((uint8_t)(crc >> 8)); s.push_back((uint8_t)crc);
            packetise(s, out);
        }
    }
    frameNo_++;
    return true;
}

std::vector<uint8_t> testDatagram(uint32_t seq, size_t len) {
    len = std::max<size_t>(len, 28);
    std::vector<uint8_t> d(len, 0);
    d[0] = 0x45; d[2] = (uint8_t)(len >> 8); d[3] = (uint8_t)len; d[8] = 64; d[9] = 17;   // IPv4, UDP
    d[12] = 10; d[15] = 1; d[16] = 224; d[19] = 1;
    unsigned sum = 0;
    for (int i = 0; i < 20; i += 2) sum += (unsigned)(d[i] << 8 | d[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    d[10] = (uint8_t)(~sum >> 8); d[11] = (uint8_t)~sum;
    d[20] = 0x13; d[21] = 0x88; d[22] = 0x13; d[23] = 0x88; d[24] = (uint8_t)((len - 20) >> 8); d[25] = (uint8_t)(len - 20);
    if (len >= 32) { d[28] = (uint8_t)(seq >> 24); d[29] = (uint8_t)(seq >> 16); d[30] = (uint8_t)(seq >> 8); d[31] = (uint8_t)seq; }
    for (size_t i = 32; i < len; i++) d[i] = (uint8_t)(seq * 13 + i * 7);
    return d;
}

bool checkTestDatagram(const uint8_t* d, size_t len, uint32_t* seq) {
    if (len < 32 || d[0] != 0x45 || (size_t)(d[2] << 8 | d[3]) != len || d[9] != 17) return false;
    const uint32_t s = (uint32_t)d[28] << 24 | (uint32_t)d[29] << 16 | (uint32_t)d[30] << 8 | d[31];
    for (size_t i = 32; i < len; i++) if (d[i] != (uint8_t)(s * 13 + i * 7)) return false;
    if (seq) *seq = s;
    return true;
}

} // namespace mpe
} // namespace dect2
