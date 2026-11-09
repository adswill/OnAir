// File broadcasts on DVB-S2 (filecast.h). Formats from SatDump (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir; see THIRD_PARTY.md.
#include "dect2/wxsat/filecast.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/goes/goes_proto.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <thread>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../../third_party/stb/stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace dect2 {
namespace wxsat {

// ------------------------------------------------------------ bzip2
namespace {
const std::array<uint32_t, 256>& bzCrcTable() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i << 24; for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1; a[i] = c; }
        return a;
    }();
    return t;
}
inline void bzCrc(uint32_t& c, uint8_t b) { c = (c << 8) ^ bzCrcTable()[(c >> 24) ^ b]; }

struct BitIn {
    const uint8_t* p; size_t n; size_t pos = 0;   // bits
    bool over = false;
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            if ((pos >> 3) >= n) { over = true; return 0; }
            v = v << 1 | (uint32_t)((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    uint64_t get48() { uint64_t a = get(24); return a << 24 | get(24); }
};
constexpr uint64_t kBzBlock = 0x314159265359ull, kBzEnd = 0x177245385090ull;

// one stream starting at the "BZh" of b; false when damaged
bool bzStream(BitIn& b, std::vector<uint8_t>& out, size_t maxOut) {
    if (b.get(8) != 'B' || b.get(8) != 'Z' || b.get(8) != 'h') return false;
    const uint32_t lv = b.get(8);
    if (lv < '1' || lv > '9') return false;
    const size_t blockMax = (size_t)(lv - '0') * 100000;
    std::vector<uint32_t> tt(blockMax);
    uint32_t combined = 0;
    for (;;) {
        const uint64_t magic = b.get48();
        if (b.over) return false;
        if (magic == kBzEnd) {
            const uint32_t want = b.get(32);
            if (b.over || want != combined) return false;
            b.pos = (b.pos + 7) & ~(size_t)7;
            return true;
        }
        if (magic != kBzBlock) return false;
        const uint32_t blockCrc = b.get(32);
        if (b.get(1)) return false;                       // the randomised format of 0.9.0, not made since 2000
        const uint32_t origPtr = b.get(24);
        const uint32_t used16 = b.get(16);
        uint8_t seq[256]; int nUsed = 0;
        for (int i = 0; i < 16; i++) if (used16 & (0x8000u >> i)) {
            const uint32_t m = b.get(16);
            for (int k = 0; k < 16; k++) if (m & (0x8000u >> k)) seq[nUsed++] = (uint8_t)(i * 16 + k);
        }
        if (b.over || nUsed == 0) return false;
        const int alpha = nUsed + 2, nGroups = (int)b.get(3), nSel = (int)b.get(15);
        if (nGroups < 2 || nGroups > 6 || nSel < 1 || b.over) return false;
        // selectors, move-to-front
        std::vector<uint8_t> sel((size_t)nSel);
        uint8_t mtf[6]; for (int i = 0; i < 6; i++) mtf[i] = (uint8_t)i;
        for (int i = 0; i < nSel; i++) {
            int j = 0;
            while (b.get(1)) { if (++j >= nGroups || b.over) return false; }
            const uint8_t v = mtf[j];
            std::memmove(mtf + 1, mtf, (size_t)j);
            mtf[0] = v;
            sel[(size_t)i] = v;
        }
        // code tables
        struct Tab { int minLen = 21, maxLen = 0; int limit[23]; int base[23]; std::vector<int> perm; } tab[6];
        for (int t = 0; t < nGroups; t++) {
            int len = (int)b.get(5);
            std::vector<int> lens((size_t)alpha);
            for (int i = 0; i < alpha; i++) {
                for (;;) {
                    if (len < 1 || len > 20 || b.over) return false;
                    if (!b.get(1)) break;
                    len += b.get(1) ? -1 : 1;
                }
                lens[(size_t)i] = len;
            }
            Tab& T = tab[t];
            for (int l : lens) { T.minLen = std::min(T.minLen, l); T.maxLen = std::max(T.maxLen, l); }
            T.perm.clear();
            for (int l = T.minLen; l <= T.maxLen; l++) for (int i = 0; i < alpha; i++) if (lens[(size_t)i] == l) T.perm.push_back(i);
            int count[23] = {};
            for (int l : lens) count[l]++;
            int code = 0, idx = 0;
            for (int l = T.minLen; l <= T.maxLen; l++) {
                T.base[l] = idx - code;
                code += count[l]; idx += count[l];
                T.limit[l] = code - 1;
                code <<= 1;
            }
        }
        // the symbols
        uint8_t order[256]; for (int i = 0; i < 256; i++) order[i] = (uint8_t)i;
        int counts[256] = {};
        size_t nblock = 0;
        int runLen = 0, runBit = 0, groupLeft = 0, gi = -1; const Tab* T = nullptr;
        const int eob = alpha - 1;
        for (;;) {
            if (groupLeft == 0) { if (++gi >= nSel) return false; groupLeft = 50; if (sel[(size_t)gi] >= nGroups) return false; T = &tab[sel[(size_t)gi]]; }
            groupLeft--;
            int l = T->minLen; int code = (int)b.get(l), sym = -1;
            for (;;) {
                if (b.over || l > T->maxLen) return false;
                if (code <= T->limit[l]) {
                    const int k = code + T->base[l];
                    if (k < 0 || k >= (int)T->perm.size()) return false;
                    sym = T->perm[(size_t)k]; break;
                }
                code = code << 1 | (int)b.get(1); l++;
            }
            if (sym <= 1) {                                // RUNA / RUNB: a run of the front symbol
                if (runBit > 24) return false;
                runLen += (sym + 1) << runBit++;
                continue;
            }
            if (runLen) {
                if (nblock + (size_t)runLen > blockMax) return false;
                const uint8_t v = seq[order[0]];
                for (int i = 0; i < runLen; i++) tt[nblock++] = v;
                counts[v] += runLen;
                runLen = 0; runBit = 0;
            }
            if (sym == eob) break;
            if (nblock >= blockMax || sym - 1 >= nUsed) return false;
            const uint8_t o = order[sym - 1];
            std::memmove(order + 1, order, (size_t)(sym - 1));
            order[0] = o;
            const uint8_t v = seq[o];
            tt[nblock++] = v; counts[v]++;
        }
        if (origPtr >= nblock && nblock) return false;
        // undo the Burrows-Wheeler transform
        int cum[257]; cum[0] = 0;
        for (int i = 0; i < 256; i++) cum[i + 1] = cum[i] + counts[i];
        for (size_t i = 0; i < nblock; i++) { const int c = (int)(tt[i] & 255); tt[(size_t)cum[c]++] |= (uint32_t)i << 8; }
        // and the first run-length step
        uint32_t crc = 0xFFFFFFFFu;
        if (nblock) {
            uint32_t pos = tt[origPtr] >> 8;
            int prev = -1, same = 0;
            for (size_t i = 0; i < nblock; i++) {
                const uint32_t e = tt[pos];
                const uint8_t ch = (uint8_t)e; pos = e >> 8;
                if (same == 4) {
                    if (out.size() + ch > maxOut) return false;
                    for (int k = 0; k < ch; k++) { out.push_back((uint8_t)prev); bzCrc(crc, (uint8_t)prev); }
                    same = 0; prev = -1;
                    continue;
                }
                if (ch == prev) same++; else { prev = ch; same = 1; }
                if (out.size() >= maxOut) return false;
                out.push_back(ch); bzCrc(crc, ch);
            }
        }
        crc = ~crc;
        if (crc != blockCrc) return false;
        combined = ((combined << 1) | (combined >> 31)) ^ crc;
    }
}
}

bool bunzip2(const uint8_t* p, size_t n, std::vector<uint8_t>& out, size_t maxOut) {
    out.clear();
    size_t i = 0; bool any = false;
    while (i + 14 <= n) {
        if (p[i] != 'B' || p[i + 1] != 'Z' || p[i + 2] != 'h') { i++; continue; }
        BitIn b{p + i, n - i};
        if (!bzStream(b, out, maxOut)) return false;
        any = true;
        i += b.pos / 8;
    }
    return any;
}

namespace {
struct BitOut {
    std::vector<uint8_t> o; uint32_t acc = 0; int have = 0;
    void put(uint32_t v, int k) { for (int i = k - 1; i >= 0; i--) { acc = acc << 1 | ((v >> i) & 1); if (++have == 8) { o.push_back((uint8_t)acc); acc = 0; have = 0; } } }
    void put48(uint64_t v) { put((uint32_t)(v >> 24), 24); put((uint32_t)v & 0xFFFFFF, 24); }
    void flush() { while (have) put(0, 1); }
};
}

std::vector<uint8_t> bzip2Simple(const uint8_t* p, size_t n) {
    BitOut w;
    w.put('B', 8); w.put('Z', 8); w.put('h', 8); w.put('9', 8);
    uint32_t combined = 0;
    size_t at = 0;
    while (at < n) {
        // run-length step, block up to 800 kB
        std::vector<uint8_t> rle; rle.reserve(std::min<size_t>(n - at, 800000) + 16);
        uint32_t crc = 0xFFFFFFFFu;
        while (at < n && rle.size() < 800000) {
            const uint8_t c = p[at];
            size_t run = 1;
            while (at + run < n && p[at + run] == c && run < 255 + 4) run++;
            for (size_t i = 0; i < run; i++) bzCrc(crc, c);
            at += run;
            if (run >= 4) { rle.insert(rle.end(), 4, c); rle.push_back((uint8_t)(run - 4)); }
            else rle.insert(rle.end(), run, c);
        }
        crc = ~crc;
        combined = ((combined << 1) | (combined >> 31)) ^ crc;
        // Burrows-Wheeler
        const size_t m = rle.size();
        std::vector<uint32_t> ix(m); std::iota(ix.begin(), ix.end(), 0u);
        std::vector<uint8_t> two(rle); two.insert(two.end(), rle.begin(), rle.end());
        std::sort(ix.begin(), ix.end(), [&](uint32_t a, uint32_t b) { return std::memcmp(&two[a], &two[b], m) < 0; });
        std::vector<uint8_t> last(m);
        uint32_t orig = 0;
        for (size_t i = 0; i < m; i++) { last[i] = rle[(ix[i] + m - 1) % m]; if (ix[i] == 0) orig = (uint32_t)i; }
        bool used[256] = {};
        for (uint8_t c : last) used[c] = true;
        uint8_t order[256]; int nUsed = 0; int rank[256];
        for (int i = 0; i < 256; i++) if (used[i]) { rank[i] = nUsed; order[nUsed++] = (uint8_t)i; }
        for (int i = 0; i < nUsed; i++) order[i] = (uint8_t)i;
        std::vector<int> syms; int run = 0;
        const auto flushRun = [&] { while (run > 0) { run--; syms.push_back(run & 1); run >>= 1; } };   // bijective base 2: RUNA 0, RUNB 1
        for (uint8_t c : last) {
            const uint8_t r = (uint8_t)rank[c];
            int j = 0; while (order[j] != r) j++;
            if (j == 0) { run++; continue; }
            flushRun();
            std::memmove(order + 1, order, (size_t)j);
            order[0] = r;
            syms.push_back(j + 1);
        }
        flushRun();
        const int alpha = nUsed + 2;
        syms.push_back(alpha - 1);
        w.put48(kBzBlock); w.put(crc, 32); w.put(0, 1); w.put(orig, 24);
        uint32_t u16 = 0; for (int i = 0; i < 16; i++) { bool any = false; for (int k = 0; k < 16; k++) any = any || used[i * 16 + k]; if (any) u16 |= 0x8000u >> i; }
        w.put(u16, 16);
        for (int i = 0; i < 16; i++) if (u16 & (0x8000u >> i)) { uint32_t mm = 0; for (int k = 0; k < 16; k++) if (used[i * 16 + k]) mm |= 0x8000u >> k; w.put(mm, 16); }
        int L = 1; while ((1 << L) < alpha) L++;
        const int nSel = (int)(syms.size() + 49) / 50;
        w.put(2, 3); w.put((uint32_t)nSel, 15);
        for (int i = 0; i < nSel; i++) w.put(0, 1);
        for (int t = 0; t < 2; t++) { w.put((uint32_t)L, 5); for (int i = 0; i < alpha; i++) w.put(0, 1); }
        for (int s : syms) w.put((uint32_t)s, L);
    }
    w.put48(kBzEnd); w.put(combined, 32); w.flush();
    return w.o;
}

// ------------------------------------------------------------ Fazzt
void FazztAssembler::clear() { open_.clear(); done_.clear(); openBytes_ = 0; }

void FazztAssembler::finish(uint32_t id, double now, std::vector<FazztFile>& out) {
    auto it = open_.find(id);
    if (it == open_.end()) return;
    Open& o = it->second;
    openBytes_ -= o.f.data.size();
    o.f.complete = o.f.have == o.f.parts;
    o.f.data.resize(o.f.size);
    out.push_back(std::move(o.f));
    open_.erase(it);
    done_[id] = now;
}

void FazztAssembler::expire(double now, std::vector<FazztFile>& out) {
    for (auto it = done_.begin(); it != done_.end();) { if (now - it->second > 900) it = done_.erase(it); else ++it; }
    std::vector<uint32_t> old;
    for (const auto& kv : open_) if (now - kv.second.last > maxSilence) old.push_back(kv.first);
    for (uint32_t id : old) finish(id, now, out);
}

void FazztAssembler::push(const uint8_t* f, size_t n, double now, std::vector<FazztFile>& out) {
    stats.frames++;
    if ((stats.frames & 1023) == 0) expire(now, out);
    if (n < 8) { stats.bad++; return; }
    const int type = f[1];
    const size_t len = (size_t)f[2] | (size_t)f[3] << 8;
    const uint32_t id = (uint32_t)f[4] | (uint32_t)f[5] << 8 | (uint32_t)f[6] << 16 | (uint32_t)f[7] << 24;
    if (len > n) { stats.bad++; return; }
    if (type == 0x03) {
        stats.heads++;
        if (n < 85) { stats.bad++; return; }
        const uint8_t* nameEnd = (const uint8_t*)std::memchr(f + 84, 0, n - 84);
        if (!nameEnd) { stats.bad++; return; }
        const size_t nl = (size_t)(nameEnd - (f + 84));
        if (nl <= 4 || 84 + nl + 60 > n) { stats.bad++; return; }
        const size_t parts = (size_t)f[72] | (size_t)f[73] << 8;
        const uint8_t* s = f + 84 + nl + 56;
        const uint32_t size = (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24;
        const size_t cap = parts * (size_t)payload_;
        if (parts == 0 || cap < size || size == 0 || cap > maxFileBytes) { stats.bad++; return; }
        if (done_.count(id)) { stats.repeats++; return; }
        auto it = open_.find(id);
        if (it != open_.end()) {
            it->second.last = now;
            stats.repeats++;
            return;
        }
        // room for it: the oldest files go first, marked incomplete
        while (openBytes_ + cap > maxOpenBytes && !open_.empty()) {
            auto oldest = std::min_element(open_.begin(), open_.end(), [](const auto& a, const auto& b) { return a.second.last < b.second.last; });
            stats.evicted++;
            finish(oldest->first, now, out);
        }
        Open o;
        o.f.name.assign((const char*)f + 84, nl);
        o.f.id = id; o.f.size = size; o.f.parts = (int)parts;
        o.f.data.assign(cap, 0);
        o.got.assign(parts, false);
        o.last = now;
        openBytes_ += cap;
        open_.emplace(id, std::move(o));
    } else if (type == 0x01) {
        auto it = open_.find(id);
        if (it == open_.end()) { if (done_.count(id)) stats.repeats++; else stats.orphan++; return; }
        if (n < 16) { stats.bad++; return; }
        stats.bodies++;
        Open& o = it->second;
        o.last = now;
        const size_t part = (size_t)f[8] | (size_t)f[9] << 8;
        if (part >= o.got.size()) { stats.bad++; return; }
        std::memcpy(&o.f.data[part * (size_t)payload_], f + 16, std::min<size_t>((size_t)payload_, n - 16));
        if (!o.got[part]) { o.got[part] = true; o.f.have++; }
        if (o.f.have == o.f.parts) finish(id, now, out);
    } else if (type == 0xFF) {
        stats.tails++;
        if (open_.count(id)) finish(id, now, out);
    }
}

std::string cleanFileName(const std::string& name) {
    std::string r;
    for (unsigned char c : name) {
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') r.push_back('_');
        else r.push_back((char)c);
    }
    size_t lead = 0;
    while (lead < r.size() && (r[lead] == '.' || r[lead] == ' ')) lead++;
    r.erase(0, lead);
    while (!r.empty() && (r.back() == '.' || r.back() == ' ')) r.pop_back();
    if (r.size() > 200) r.erase(0, r.size() - 200);
    return r;
}

// ------------------------------------------------------------ HRIT pictures
HritSegment parseHritName(const std::string& name) {
    HritSegment s;
    // IMG_DK01IR1_201703080230_001
    const size_t a = name.find('_'), b = a == std::string::npos ? a : name.find('_', a + 1), c = b == std::string::npos ? b : name.find('_', b + 1);
    if (c == std::string::npos || name.compare(0, a, "IMG") != 0 || b - a <= 5 || name.compare(a + 1, 4, "DK01") != 0) return s;
    s.channel = name.substr(a + 5, b - a - 5);
    s.stamp = name.substr(b + 1, c - b - 1);
    const std::string segs = name.substr(c + 1, 3);
    if (s.stamp.size() != 12 || segs.size() != 3) return s;
    for (char ch : s.stamp) if (ch < '0' || ch > '9') return s;
    for (char ch : segs) if (ch < '0' || ch > '9') return s;
    s.seg = std::atoi(segs.c_str());
    s.ok = s.seg >= 1;
    return s;
}

void HritMosaic::emit(Pic& p) {
    if (onImage && p.have > 0) onImage(p.channel, p.stamp, p.w, p.segH * p.total, p.px.data(), p.have, p.total);
}

bool HritMosaic::push(const std::string& name, const std::vector<uint8_t>& d) {
    HritSegment s = parseHritName(name);
    if (!s.ok) return false;
    goes::Headers h;
    if (!goes::parseHeaders(d.data(), d.size(), h, 1) || !h.hasImage || h.compression != 0 || h.columns < 1 || h.lines < 1 || h.columns > 22000 || h.lines > 22000 || (h.bpp != 8 && h.bpp != 16)) { bad++; return true; }
    const int total = h.hasSeg ? h.maxSeg : 10;
    const int segNo = h.hasSeg ? h.segSeq : s.seg;
    const size_t bytes = (size_t)h.columns * (size_t)h.lines * (h.bpp == 16 ? 2 : 1);
    if (total < 1 || total > 64 || segNo < 1 || segNo > total || h.headerLength + bytes > d.size()) { bad++; return true; }
    auto it = open_.find(s.channel);
    if (it != open_.end() && (it->second.stamp != s.stamp || it->second.w != h.columns || it->second.segH != h.lines || it->second.total != total)) { emit(it->second); open_.erase(it); it = open_.end(); }
    if (it == open_.end()) {
        Pic p;
        p.channel = s.channel; p.stamp = s.stamp; p.w = h.columns; p.segH = h.lines; p.total = total;
        p.got.assign((size_t)total, false);
        p.px.assign((size_t)h.columns * (size_t)h.lines * (size_t)total, 0);
        it = open_.emplace(s.channel, std::move(p)).first;
    }
    Pic& p = it->second;
    const uint8_t* src = d.data() + h.headerLength;
    uint8_t* dst = p.px.data() + (size_t)(segNo - 1) * (size_t)h.columns * (size_t)h.lines;
    const size_t npx = (size_t)h.columns * (size_t)h.lines;
    if (h.bpp == 16) for (size_t i = 0; i < npx; i++) {
        const int v = src[i * 2] << 8 | src[i * 2 + 1];
        dst[i] = v >= 1023 ? 0 : (uint8_t)(v >> 2);            // the top count marks space and bad pixels
    } else std::memcpy(dst, src, npx);
    if (!p.got[(size_t)(segNo - 1)]) { p.got[(size_t)(segNo - 1)] = true; p.have++; }
    if (onProgress) onProgress(p.channel, p.stamp, p.w, p.segH * p.total, p.px.data(), p.have, p.total);
    if (p.have == p.total) { emit(p); open_.erase(it); }
    return true;
}

void HritMosaic::flush() {
    for (auto& kv : open_) emit(kv.second);
    open_.clear();
}

// ------------------------------------------------------------ the layer
struct FilecastLayer::Impl {
    int profile;
    bool himawari;
    int pidDef;
    int payload;
    std::atomic<int> pid;
    // receiver side
    std::mutex rxMu;
    dvbsip::MpeExtractor mpe;
    FazztAssembler fazzt;
    double now = 0;
    // worker side
    std::mutex mu;
    std::condition_variable cv, idleCv;
    std::deque<FazztFile> q;
    size_t qBytes = 0;
    bool stop = false, busy = false, flushPics = false;
    std::thread worker;
    HritMosaic mosaic;
    std::string dir;
    FilecastTelemetry tel;       // under mu
    uint64_t picId = 0;
    static constexpr size_t kMaxQueue = 512u << 20;
    static constexpr int kPreview = 1400;

    explicit Impl(int p) : profile(p), himawari(p == kWxHimawariCast), pidDef(himawari ? 1001 : 4201), payload(himawari ? 1411 : 1408), pid(pidDef), fazzt(payload) {
        tel.pid = pidDef;
        mosaic.onProgress = [this](const std::string& c, const std::string& s, int w, int h, const uint8_t* px, int have, int total) { picture(c, s, w, h, px, have, total, false); };
        mosaic.onImage = [this](const std::string& c, const std::string& s, int w, int h, const uint8_t* px, int have, int total) { picture(c, s, w, h, px, have, total, true); };
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        worker.join();
    }

    void note(const std::string& e) { std::lock_guard<std::mutex> lk(mu); tel.lastError = e; }

    std::string saveBytes(const char* sub, const std::string& name, const uint8_t* p, size_t n) {
        std::string d; { std::lock_guard<std::mutex> lk(mu); d = dir; }
        if (d.empty()) return "";
        const std::string clean = cleanFileName(name);
        if (clean.empty()) return "";
        std::error_code ec;
        const std::filesystem::path base(reinterpret_cast<const char8_t*>((d + "/" + sub).c_str()));
        std::filesystem::create_directories(base, ec);
        const std::filesystem::path f = base / std::filesystem::path(reinterpret_cast<const char8_t*>(clean.c_str()));
        std::ofstream o(f, std::ios::binary);
        if (!o) { note("could not write " + d + "/" + sub + "/" + clean); return ""; }
        o.write(reinterpret_cast<const char*>(p), (std::streamsize)n);
        return o ? d + "/" + sub + "/" + clean : "";
    }

    void picture(const std::string& c, const std::string& s, int w, int h, const uint8_t* px, int have, int total, bool finished) {
        std::string path;
        if (finished) {
            std::string d; { std::lock_guard<std::mutex> lk(mu); d = dir; }
            const std::string clean = cleanFileName(c + "_" + s + (have == total ? "" : "_partial") + ".png");
            if (!d.empty() && !clean.empty()) {
                std::error_code ec;
                const std::filesystem::path base(reinterpret_cast<const char8_t*>((d + "/IMAGES").c_str()));
                std::filesystem::create_directories(base, ec);
                path = d + "/IMAGES/" + clean;
                if (!stbi_write_png(path.c_str(), w, h, 1, px, w)) { note("could not write " + path); path.clear(); }
            }
        }
        // a reduced copy for the screen
        const int step = std::max(1, (std::max(w, h) + kPreview - 1) / kPreview);
        const int ow = w / step, oh = h / step;
        auto small = std::make_shared<std::vector<uint8_t>>((size_t)ow * (size_t)oh);
        for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) (*small)[(size_t)y * (size_t)ow + (size_t)x] = px[(size_t)(y * step) * (size_t)w + (size_t)(x * step)];
        std::lock_guard<std::mutex> lk(mu);
        const auto find = [&]() -> FilecastPicture* { for (auto& p : tel.pictures) if (p.channel == c) return &p; return nullptr; };
        if (!find()) {
            tel.pictures.emplace_back();
            tel.pictures.back().channel = c;
            std::sort(tel.pictures.begin(), tel.pictures.end(), [](const FilecastPicture& x, const FilecastPicture& y) { return x.channel < y.channel; });
        }
        FilecastPicture* slot = find();
        slot->channel = c; slot->stamp = s; slot->w = ow; slot->h = oh; slot->have = have; slot->total = total; slot->px = small; slot->id = ++picId;
        if (!path.empty()) slot->path = path;
        if (finished) tel.picturesDone++;
    }

    void handle(FazztFile& f) {
        std::string name = f.name;
        std::vector<uint8_t> plain;
        bool failed = false;
        const char* sub = f.complete ? "COMPLETE" : "INCOMPLETE";
        if (!himawari) {
            if (name.size() > 45) name.resize(name.size() - 45);   // GeoNetCast names end in a 45-character tag
            plain = std::move(f.data);
        } else if (f.complete && name.size() > 4 && name.compare(name.size() - 4, 4, ".bz2") == 0) {
            if (bunzip2(f.data.data(), f.data.size(), plain)) name.resize(name.size() - 4);
            else { failed = true; plain = std::move(f.data); sub = "BADBZ2"; note("damaged bzip2 data in " + cleanFileName(name)); }
        } else plain = std::move(f.data);
        bool isSeg = false;
        if (himawari && f.complete && !failed) {
            isSeg = mosaic.push(name, plain);
            if (isSeg && name.find('.') == std::string::npos) name += ".hrit";
        }
        const std::string path = saveBytes(himawari && f.complete && !failed ? (isSeg ? "HRIT" : "ADD") : sub, name, plain.data(), plain.size());
        std::lock_guard<std::mutex> lk(mu);
        if (failed) tel.filesFailed++; else if (f.complete) tel.filesComplete++; else tel.filesPartial++;
        tel.bytes += plain.size();
        tel.recent.insert(tel.recent.begin(), FilecastFileRow{cleanFileName(name), path, plain.size(), f.parts, f.have, f.complete && !failed});
        if (tel.recent.size() > 60) tel.recent.pop_back();
    }

    void run() {
        std::unique_lock<std::mutex> lk(mu);
        for (;;) {
            cv.wait(lk, [this] { return stop || !q.empty() || flushPics; });
            if (stop) return;
            if (!q.empty()) {
                FazztFile f = std::move(q.front());
                q.pop_front();
                qBytes -= f.data.size();
                busy = true;
                lk.unlock();
                handle(f);
                lk.lock();
                busy = false;
            } else if (flushPics) {
                flushPics = false;
                busy = true;
                lk.unlock();
                mosaic.flush();
                lk.lock();
                busy = false;
            }
            idleCv.notify_all();
        }
    }

    void enqueue(FazztFile f) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (qBytes + f.data.size() > kMaxQueue) { tel.filesDropped++; return; }
            qBytes += f.data.size();
            q.push_back(std::move(f));
        }
        cv.notify_one();
    }

    void frame(const uint8_t* p, size_t n) {
        std::vector<FazztFile> done;
        fazzt.push(p, n, now, done);
        for (auto& f : done) enqueue(std::move(f));
    }

    void datagram(const std::vector<uint8_t>& d) {
        // IPv4 + UDP around the Fazzt packet
        if (d.size() < 28 || (d[0] >> 4) != 4 || d[9] != 17) { std::lock_guard<std::mutex> lk(mu); tel.notUdp++; return; }
        const size_t ihl = (size_t)(d[0] & 15) * 4;
        if (ihl < 20 || d.size() < ihl + 8) { std::lock_guard<std::mutex> lk(mu); tel.notUdp++; return; }
        { std::lock_guard<std::mutex> lk(mu); tel.datagrams++; }
        frame(d.data() + ihl + 8, d.size() - ihl - 8);
    }

    void telFill(FilecastTelemetry& out) {
        {
            std::lock_guard<std::mutex> lk(rxMu);
            std::lock_guard<std::mutex> lk2(mu);
            tel.frames = fazzt.stats.frames;
            tel.badFrames = fazzt.stats.bad;
            tel.open = fazzt.filesOpen();
            tel.pid = pid.load();
            tel.saveDir = dir;
            out = tel;
        }
    }
};

FilecastLayer::FilecastLayer(int profile) : p_(std::make_unique<Impl>(profile)) {}
FilecastLayer::~FilecastLayer() = default;
void FilecastLayer::setSaveDir(const std::string& d) { std::lock_guard<std::mutex> lk(p_->mu); p_->dir = d; }
void FilecastLayer::setPid(int pid) { p_->pid = pid > 0 && pid < 8191 ? pid : p_->pidDef; }
int FilecastLayer::pid() const { return p_->pid.load(); }

void FilecastLayer::pushTs(const uint8_t* packets, size_t n, double secs) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.rxMu);
    m.now += secs;
    const int want = m.pid.load();
    std::vector<std::vector<uint8_t>> out;
    uint64_t seen = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = packets + i * 188;
        if (p[0] != 0x47) continue;
        if ((((p[1] & 0x1F) << 8) | p[2]) != want) continue;
        seen++;
        out.clear();
        m.mpe.packet(p, out);
        for (const auto& d : out) m.datagram(d);
    }
    if (seen) { std::lock_guard<std::mutex> lk2(m.mu); m.tel.tsPackets += seen; }
    std::vector<FazztFile> done;
    m.fazzt.expire(m.now, done);
    for (auto& f : done) m.enqueue(std::move(f));
}

void FilecastLayer::pushFile(FazztFile f) { p_->enqueue(std::move(f)); }

void FilecastLayer::pushFazzt(const uint8_t* f, size_t n, double now) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.rxMu);
    m.now = now;
    m.frame(f, n);
}

void FilecastLayer::flush() {
    Impl& m = *p_;
    std::unique_lock<std::mutex> lk(m.mu);
    m.flushPics = true;
    m.cv.notify_all();
    m.idleCv.wait(lk, [&m] { return m.q.empty() && !m.flushPics && !m.busy; });
}

void FilecastLayer::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.rxMu);
    m.fazzt.clear();
    m.mpe = dvbsip::MpeExtractor();
    std::lock_guard<std::mutex> lk2(m.mu);
    const std::vector<FilecastPicture> keep = m.tel.pictures;
    m.tel = FilecastTelemetry();
    m.tel.pictures = keep;
    m.tel.pid = m.pid.load();
}

FilecastTelemetry FilecastLayer::telemetry() const { FilecastTelemetry o; p_->telFill(o); return o; }

// ------------------------------------------------------------ the test carousel
int filecastTestPixel(int x, int y) { return ((x * 3 + y * 5) % 1000) + ((y / 16) & 1 ? 11 : 0); }

std::function<void(uint8_t*)> filecastTestTs(int profile) {
    struct State {
        int profile, payload, pid;
        uint8_t cc = 0;
        std::deque<std::array<uint8_t, 188>> ts;
        int file = 0;
        uint32_t ids = 0x1000;
        uint8_t null[188];
    };
    auto st = std::make_shared<State>();
    st->profile = profile;
    st->payload = profile == kWxHimawariCast ? 1411 : 1408;
    st->pid = profile == kWxHimawariCast ? 1001 : 4201;
    std::memset(st->null, 0xFF, 188);
    st->null[0] = 0x47; st->null[1] = 0x1F; st->null[2] = 0xFF; st->null[3] = 0x10;
    const auto send = [](State& s, const std::vector<uint8_t>& frame) {
        static const uint8_t src[4] = {10, 0, 0, 1}, dst[4] = {239, 0, 0, 1}, mac[6] = {1, 0, 0x5E, 0, 0, 1};
        const std::vector<uint8_t> ip = dvbsip::buildUdpV4(src, dst, 5000, 8001, frame);
        for (const auto& pkt : dvbsip::packetizeSection((uint16_t)s.pid, s.cc, dvbsip::buildMpeSection(mac, ip))) {
            std::array<uint8_t, 188> a; std::memcpy(a.data(), pkt.data(), 188); s.ts.push_back(a);
        }
    };
    const auto nextFile = [send](State& s) {
        std::string name;
        std::vector<uint8_t> body;
        const int cycle = s.file / (kFcTestSegments + 1);        // each round of the carousel is a new scan, ten minutes later, as on the air
        const int n = s.file++ % (kFcTestSegments + 1);
        const int minutes = (2 * 60 + 30 + cycle * 10) % (24 * 60);
        const uint32_t id = s.ids++;
        if (s.profile == kWxHimawariCast) {
            if (n < kFcTestSegments) {
                char nm[64]; snprintf(nm, sizeof nm, "IMG_DK01IR1_20261109%02d%02d_%03d", minutes / 60, minutes % 60, n + 1);
                goes::Headers h;
                h.fileType = 0; h.hasImage = true; h.bpp = 16; h.columns = kFcTestColumns; h.lines = kFcTestSegLines; h.compression = 0;
                h.annotation = nm; h.hasSeg = true; h.segSeq = n + 1; h.maxSeg = kFcTestSegments; h.startLine = n * kFcTestSegLines + 1;
                std::vector<uint8_t> px((size_t)kFcTestColumns * kFcTestSegLines * 2);
                for (int y = 0; y < kFcTestSegLines; y++) for (int x = 0; x < kFcTestColumns; x++) {
                    const int v = filecastTestPixel(x, y + n * kFcTestSegLines); const size_t i = (size_t)(y * kFcTestColumns + x) * 2;
                    px[i] = (uint8_t)(v >> 8); px[i + 1] = (uint8_t)v;
                }
                const std::vector<uint8_t> f = goes::makeFile(h, px, 1);
                body = bzip2Simple(f.data(), f.size());
                name = std::string(nm) + ".bz2";
            } else { const char t[] = "OnAir test notice\n"; body = bzip2Simple(reinterpret_cast<const uint8_t*>(t), sizeof t - 1); name = "NOTICE_202611090230.txt.bz2"; }
        } else {
            body.resize(5000 + (size_t)n * 700);
            for (size_t i = 0; i < body.size(); i++) body[i] = (uint8_t)(i * 7 + (size_t)n);
            char nm[64]; snprintf(nm, sizeof nm, "OR_TEST-%02d_G16_s2026313.nc", n);
            name = std::string(nm) + std::string(45, 'X');
        }
        const int parts = (int)((body.size() + (size_t)s.payload - 1) / (size_t)s.payload);
        std::vector<uint8_t> head(242, 0);
        head[1] = 3; head[2] = 242; head[3] = 0;
        for (int i = 0; i < 4; i++) head[(size_t)(4 + i)] = (uint8_t)(id >> (8 * i));
        head[72] = (uint8_t)parts; head[73] = (uint8_t)(parts >> 8);
        std::memcpy(&head[84], name.c_str(), std::min<size_t>(name.size(), 100));
        const size_t sz = 84 + name.size() + 56;
        if (sz + 4 < head.size()) for (int i = 0; i < 4; i++) head[sz + (size_t)i] = (uint8_t)(body.size() >> (8 * i));
        send(s, head);
        for (int p = 0; p < parts; p++) {
            std::vector<uint8_t> fr(16 + (size_t)s.payload, 0);
            fr[1] = 1; fr[2] = (uint8_t)fr.size(); fr[3] = (uint8_t)(fr.size() >> 8);
            for (int i = 0; i < 4; i++) fr[(size_t)(4 + i)] = (uint8_t)(id >> (8 * i));
            fr[8] = (uint8_t)p; fr[9] = (uint8_t)(p >> 8);
            const size_t off = (size_t)p * (size_t)s.payload;
            std::memcpy(&fr[16], body.data() + off, std::min<size_t>((size_t)s.payload, body.size() - off));
            send(s, fr);
        }
        send(s, head);
        std::vector<uint8_t> tail(16, 0);
        tail[1] = 0xFF; tail[2] = 16;
        for (int i = 0; i < 4; i++) tail[(size_t)(4 + i)] = (uint8_t)(id >> (8 * i));
        send(s, tail);
    };
    return [st, nextFile](uint8_t* out) {
        if (st->ts.empty()) nextFile(*st);
        if (st->ts.empty()) { std::memcpy(out, st->null, 188); return; }
        std::memcpy(out, st->ts.front().data(), 188);
        st->ts.pop_front();
    };
}

} // namespace wxsat
} // namespace dect2
