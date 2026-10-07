// DRM AAC (clause 5.4.1): the ER AAC frame of DRM (960 transform, side information first, Huffman codeword reordering, virtual codebooks) is read here and
// written again as an ordinary AAC-LC raw data block, which libavcodec then decodes. SBR and PS are not decoded: libavcodec has no SBR for 960 frames, so the
// core signal is what plays.
//
// Layout of a mono frame (found by comparing real streams with the CRC of the audio super frame, which covers the first two parts):
//   side information: ics_info (reserved bit, window_sequence 2, window_shape 1, max_sfb 6, or 4 and scale_factor_grouping 7 for short windows),
//     tns_data_present, pulse_data_present, global_gain 8, section data (5 bit codebooks), scale factor data,
//     length_of_reordered_spectral_data 14, length_of_longest_codeword 6
//   TNS data (when present), then the spectral data of that length in the order of the codeword reordering.
// Codeword reordering: the sorted list of codewords (codebook priority, then spectral position) is cut into segments of the widths of their longest possible
// code words; the first code word of the list sits at the start of its segment (a priority code word), the others fill the segments from the end and from the
// start alternately, set by set, and continue in the next segment when one does not fit.
#include "dect2/drm_aac.h"
#include "dect2/drm_fec.h"
#include "drm_aac_internal.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cstring>
#include <mutex>

namespace dect2 { namespace drm {

using namespace aac;

namespace aac {

const CbInfo& cbInfo(int cb) {
    static const CbInfo t[12] = {
        {0, false, 0, 0, nullptr, nullptr},
        {4, true, 1, 81, kSpecCode1, kSpecBits1}, {4, true, 1, 81, kSpecCode2, kSpecBits2}, {4, false, 2, 81, kSpecCode3, kSpecBits3}, {4, false, 2, 81, kSpecCode4, kSpecBits4},
        {2, true, 4, 81, kSpecCode5, kSpecBits5}, {2, true, 4, 81, kSpecCode6, kSpecBits6}, {2, false, 7, 64, kSpecCode7, kSpecBits7}, {2, false, 7, 64, kSpecCode8, kSpecBits8},
        {2, false, 12, 169, kSpecCode9, kSpecBits9}, {2, false, 12, 169, kSpecCode10, kSpecBits10}, {2, false, 16, 289, kSpecCode11, kSpecBits11},
    };
    return t[cb < 0 || cb > 11 ? 0 : cb];
}

void cbValues(int cb, int idx, int* v) {
    const CbInfo& c = cbInfo(cb);
    const int base = c.isSigned ? 2 * c.lav + 1 : c.lav + 1;
    for (int i = c.dim - 1; i >= 0; i--) { v[i] = idx % base; idx /= base; if (c.isSigned) v[i] -= c.lav; }
}
int cbIndex(int cb, const int* v) {
    const CbInfo& c = cbInfo(cb);
    const int base = c.isSigned ? 2 * c.lav + 1 : c.lav + 1;
    int idx = 0;
    for (int i = 0; i < c.dim; i++) idx = idx * base + (c.isSigned ? v[i] + c.lav : v[i]);
    return idx;
}

void PrefixTable::build(const uint32_t* code, const uint8_t* bits, int n) {
    nodes_.assign(1, Node());
    for (int s = 0; s < n; s++) {
        int cur = 0;
        for (int b = bits[s] - 1; b >= 0; b--) {
            const int bit = (code[s] >> b) & 1;
            if (nodes_[(size_t)cur].child[bit] < 0) { nodes_[(size_t)cur].child[bit] = (int)nodes_.size(); nodes_.push_back(Node()); }
            cur = nodes_[(size_t)cur].child[bit];
        }
        nodes_[(size_t)cur].sym = s;
    }
}
void PrefixTable::build16(const uint16_t* code, const uint8_t* bits, int n) {
    std::vector<uint32_t> c(code, code + n);
    build(c.data(), bits, n);
}
int PrefixTable::find(const uint8_t* b, int n, int* used) const {
    int cur = 0;
    for (int i = 0; i < n; i++) {
        cur = nodes_[(size_t)cur].child[b[i] & 1];
        if (cur < 0) return -2;
        if (nodes_[(size_t)cur].sym >= 0) { *used = i + 1; return nodes_[(size_t)cur].sym; }
    }
    return -1;
}
const PrefixTable& scaleTable() {
    static const PrefixTable t = [] { PrefixTable p; p.build(kScaleCode, kScaleBits, 121); return p; }();
    return t;
}
const PrefixTable& specTable(int cb) {
    static const std::vector<PrefixTable> t = [] {
        std::vector<PrefixTable> v(12);
        for (int c = 1; c <= 11; c++) v[(size_t)c].build16(cbInfo(c).code, cbInfo(c).bits, cbInfo(c).size);
        return v;
    }();
    return t[(size_t)std::max(1, std::min(11, cb))];
}
// longest code word (with its sign bits and escape sequence) of each codebook, with the virtual codebooks 16 to 31 of the VCB11 tool: index = codebook
const int kMaxCwLen[32] = {0, 11, 9, 20, 16, 13, 11, 14, 12, 17, 14, 49, 0, 0, 0, 0, 14, 17, 21, 21, 25, 25, 29, 29, 29, 29, 33, 33, 33, 37, 37, 41};

namespace {
bool goodCbHcr(int thisCb, int secCb) {
    if (!((secCb > 0 && secCb <= 11) || (secCb >= 16 && secCb <= 31))) return false;
    if (thisCb < 11) return secCb == thisCb || secCb == thisCb + 1;
    return secCb == thisCb;
}
} // namespace

// The sorted code word list of the codeword reordering (ISO/IEC 14496-3, HCR): by codebook priority, then by band, then by position.
void hcrSortedList(int numGroups, const int* groupLen, int maxSfb, const int (*sfbCb)[64], const uint16_t* swb, int frameLines, std::vector<HcrCw>& cws) {
    static const int kPre[22] = {11, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 9, 7, 5, 3, 1};
    cws.clear();
    int spOff[8] = {};
    for (int g = 1; g < numGroups; g++) spOff[g] = spOff[g - 1] + frameLines * groupLen[g - 1];
    for (int pi = 0; pi < 22; pi++) {
        const int thisCb = kPre[pi];
        for (int sfb = 0; sfb < maxSfb; sfb++) {
            const int width0 = swb[sfb + 1] - swb[sfb];
            for (int w = 0; 4 * w < width0; w++) {
                for (int g = 0; g < numGroups; g++) {
                    const int cb = sfbCb[g][sfb];
                    if (!goodCbHcr(thisCb, cb)) continue;
                    const int inc = cb < 5 ? 4 : 2;
                    const int perStep = 4 * groupLen[g] / inc;
                    const int sfbSize = groupLen[g] * width0;
                    for (int k = 0; k < perStep; k++) {
                        if (k + w * perStep >= sfbSize) break;
                        cws.push_back({cb, spOff[g] + groupLen[g] * swb[sfb] + inc * (k + w * perStep)});
                    }
                }
            }
        }
    }
}

const uint16_t* swbLong(int rateHz, int& numSfb) {
    if (rateHz == 12000) { numSfb = 42; return kSwbOffset96016; }
    numSfb = 46; return kSwbOffset96024;
}
const uint16_t* swbShort(int rateHz, int& numSfb) {
    numSfb = 15;
    return rateHz == 12000 ? kSwbOffset12016 : kSwbOffset12024;
}

} // namespace aac

namespace {

struct BitIn {
    const uint8_t* p; size_t n; size_t pos = 0; bool bad = false;
    BitIn(const uint8_t* d, size_t bits) : p(d), n(bits) {}
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            if (pos >= n) { bad = true; v <<= 1; continue; }
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1u);
            pos++;
        }
        return v;
    }
    size_t left() const { return n > pos ? n - pos : 0; }
};

struct BitOut {
    std::vector<uint8_t> b; size_t n = 0;
    void put(uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; i--) {
            if ((n & 7) == 0) b.push_back(0);
            if ((v >> i) & 1u) b.back() |= (uint8_t)(0x80 >> (n & 7));
            n++;
        }
    }
    void bitsFrom(const uint8_t* src, size_t from, size_t to) { for (size_t i = from; i < to; i++) put((src[i >> 3] >> (7 - (i & 7))) & 1u, 1); }
};

struct Sect { int cb = 0, start = 0, end = 0; };

struct Ics {
    int ws = 0, shape = 0, maxSfb = 0, grouping = 0, gg = 0;
    bool tns = false, pulse = false;
    int numGroups = 1, groupLen[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    std::vector<Sect> sect[8];
    std::vector<int> sfSym;           // symbols of the scale factor code in the order of coding
    int L = 0, Lc = 0;
    size_t tnsBegin = 0, tnsEnd = 0;  // bit positions in the frame
    size_t dataBegin = 0;
    bool shortWin() const { return ws == 2; }
    int q[1024] = {};
};

enum Result { kOk = 0, kDamaged, kUnsupported };

// the TNS data of one channel: only its length is needed, the bits are copied
bool skipTns(BitIn& in, bool shortWin) {
    const int nWin = shortWin ? 8 : 1;
    for (int w = 0; w < nWin; w++) {
        const int nFilt = (int)in.get(shortWin ? 1 : 2);
        if (!nFilt) continue;
        const int res = (int)in.get(1);
        for (int f = 0; f < nFilt; f++) {
            in.get(shortWin ? 4 : 6);
            const int order = (int)in.get(shortWin ? 3 : 5);
            if (order) {
                in.get(1);
                const int comp = (int)in.get(1);
                const int cb = res + 3 - comp;
                for (int i = 0; i < order; i++) in.get(cb);
            }
        }
        if (in.bad) return false;
    }
    return !in.bad;
}

// side information of a mono frame
Result parseSi(const uint8_t* d, size_t bits, int rateHz, Ics& s) {
    BitIn in(d, bits);
    if (in.get(1) != 0) return kDamaged;                      // ics_reserved_bit
    s.ws = (int)in.get(2);
    s.shape = (int)in.get(1);
    int nLong = 0, nShort = 0;
    const uint16_t* swbL = swbLong(rateHz, nLong);
    const uint16_t* swbS = swbShort(rateHz, nShort);
    (void)swbL; (void)swbS;
    if (s.shortWin()) {
        s.maxSfb = (int)in.get(4);
        s.grouping = (int)in.get(7);
        s.numGroups = 1; s.groupLen[0] = 1;
        for (int i = 0; i < 7; i++) {
            if ((s.grouping >> (6 - i)) & 1) s.groupLen[s.numGroups - 1]++;
            else s.groupLen[s.numGroups++] = 1;
        }
        if (s.maxSfb > nShort) return kDamaged;
    } else {
        s.maxSfb = (int)in.get(6);
        s.numGroups = 1; s.groupLen[0] = 1;
        if (s.maxSfb > nLong) return kDamaged;
    }
    s.tns = in.get(1) != 0;
    s.pulse = in.get(1) != 0;
    s.gg = (int)in.get(8);
    // section data
    const int secBits = s.shortWin() ? 3 : 5, secEsc = (1 << secBits) - 1;
    for (int g = 0; g < s.numGroups; g++) {
        int k = 0;
        while (k < s.maxSfb) {
            Sect sc;
            sc.cb = (int)in.get(5);
            int len = 0, inc;
            do { inc = (int)in.get(secBits); len += inc; } while (inc == secEsc && !in.bad);
            sc.start = k; sc.end = k + len;
            if (in.bad || len == 0 || sc.end > s.maxSfb) return kDamaged;
            s.sect[g].push_back(sc);
            k = sc.end;
        }
    }
    // scale factors: one symbol for every band whose codebook is not 0
    for (int g = 0; g < s.numGroups; g++)
        for (const Sect& sc : s.sect[g]) {
            if (sc.cb == 0) continue;
            for (int b = sc.start; b < sc.end; b++) {
                // the codes are at most 19 bits long
                uint8_t tmp[19];
                const size_t avail = std::min<size_t>(19, in.left());
                for (size_t i = 0; i < avail; i++) tmp[i] = (uint8_t)((d[(in.pos + i) >> 3] >> (7 - ((in.pos + i) & 7))) & 1);
                int used = 0;
                const int sym = scaleTable().find(tmp, (int)avail, &used);
                if (sym < 0) return kDamaged;
                in.pos += (size_t)used;
                s.sfSym.push_back(sym);
            }
        }
    s.L = (int)in.get(14);
    s.Lc = (int)in.get(6);
    if (in.bad) return kDamaged;
    s.tnsBegin = in.pos;
    if (s.tns && !skipTns(in, s.shortWin())) return kDamaged;
    s.tnsEnd = in.pos;
    s.dataBegin = in.pos;
    if (s.pulse) return kUnsupported;
    if (s.L < 0 || s.dataBegin + (size_t)s.L > bits) return kDamaged;
    if (s.L == 0) { bool any = false; for (int g = 0; g < s.numGroups; g++) for (const Sect& sc : s.sect[g]) if (sc.cb) any = true; if (any) return kDamaged; }
    else if (s.Lc <= 0) return kDamaged;
    return kOk;
}

// one coded unit (a pair or a quad with its sign bits and escape sequences) from a bit sequence: values and the number of bits used; -1 not enough bits, -2 invalid
int decodeUnit(int cb, const uint8_t* b, int n, int* v, int* used) {
    const int c = cb >= 16 ? 11 : cb;
    const CbInfo& ci = cbInfo(c);
    int pos = 0;
    const int sym = specTable(c).find(b, std::min(n, 19), &pos);
    if (sym == -1) return -1;
    if (sym < 0) return -2;
    cbValues(c, sym, v);
    if (!ci.isSigned) {
        for (int i = 0; i < ci.dim; i++) {
            if (!v[i]) continue;
            if (pos >= n) return -1;
            if (b[pos++]) v[i] = -v[i];
        }
    }
    if (c == 11) {
        for (int i = 0; i < 2; i++) {
            if (std::abs(v[i]) != 16) continue;
            int nb = 4;
            for (;;) {
                if (pos >= n) return -1;
                if (!b[pos++]) break;
                if (++nb > 12) return -2;
            }
            if (pos + nb > n) return -1;
            int w = 0;
            for (int k = 0; k < nb; k++) w = (w << 1) | b[pos++];
            const int val = (1 << nb) + w;
            v[i] = v[i] < 0 ? -val : val;
        }
    }
    *used = pos;
    return 0;
}

using Cw = HcrCw;

// Codeword reordering. bits: the L bits of the data region (one per byte). Fills s.q. Returns false when the data do not decode (damaged frame).
bool hcrDecode(Ics& s, int rateHz, const uint8_t* bits) {
    int nLong = 0, nShort = 0;
    const uint16_t* swb = s.shortWin() ? swbShort(rateHz, nShort) : swbLong(rateHz, nLong);
    const int nshort = s.shortWin() ? 120 : kFrame;
    // sorted list of code words
    std::vector<Cw> cws;
    int sfbCb[8][64] = {};
    for (int g = 0; g < s.numGroups; g++)
        for (const Sect& sc : s.sect[g]) for (int b = sc.start; b < sc.end; b++) sfbCb[g][b] = sc.cb;
    hcrSortedList(s.numGroups, s.groupLen, s.maxSfb, sfbCb, swb, nshort, cws);
    if (cws.empty()) return true;
    // segments: each starts with a priority code word
    std::vector<std::vector<uint8_t>> seg;
    int bitsRead = 0;
    size_t i = 0;
    for (; i < cws.size(); i++) {
        const int w = std::min(kMaxCwLen[cws[i].cb], s.Lc);
        if (bitsRead + w > s.L) break;
        std::vector<uint8_t> sb(bits + bitsRead, bits + bitsRead + w);
        bitsRead += w;
        int v[4], used = 0;
        if (decodeUnit(cws[i].cb, sb.data(), w, v, &used) != 0) return false;
        for (int k = 0; k < cbInfo(cws[i].cb >= 16 ? 11 : cws[i].cb).dim; k++) s.q[cws[i].sp + k] = v[k];
        std::vector<uint8_t> rest(sb.begin() + used, sb.end());
        std::reverse(rest.begin(), rest.end());
        seg.push_back(std::move(rest));
    }
    if (seg.empty()) return false;
    if (bitsRead < s.L) {                                   // the bits left over belong to the last segment, behind its end
        std::vector<uint8_t> tail(bits + bitsRead, bits + s.L);
        std::reverse(tail.begin(), tail.end());
        tail.insert(tail.end(), seg.back().begin(), seg.back().end());
        seg.back() = std::move(tail);
    }
    const int nSeg = (int)seg.size();
    const size_t nRest = cws.size() - i;
    std::vector<std::vector<uint8_t>> stored(nRest);
    std::vector<uint8_t> done(nRest, 0);
    std::vector<uint8_t> comb;
    const int nSets = (int)(cws.size() / (size_t)nSeg);
    for (int set = 1; set <= nSets; set++) {
        for (int trial = 0; trial < nSeg; trial++) {
            for (int base = 0; base < nSeg; base++) {
                const int sIdx = (trial + base) % nSeg;
                const size_t ci = (size_t)base + (size_t)(set - 1) * (size_t)nSeg;
                if (ci >= nRest) break;
                if (done[ci] || seg[(size_t)sIdx].empty()) continue;
                comb = stored[ci];
                comb.insert(comb.end(), seg[(size_t)sIdx].begin(), seg[(size_t)sIdx].end());
                const Cw& cw = cws[i + ci];
                int v[4], used = 0;
                const int r = decodeUnit(cw.cb, comb.data(), (int)comb.size(), v, &used);
                if (r == 0) {
                    for (int k = 0; k < cbInfo(cw.cb >= 16 ? 11 : cw.cb).dim; k++) s.q[cw.sp + k] = v[k];
                    done[ci] = 1;
                    seg[(size_t)sIdx].assign(comb.begin() + used, comb.end());
                    stored[ci].clear();
                } else if (r == -1) {
                    stored[ci] = comb;
                    seg[(size_t)sIdx].clear();
                } else {
                    return false;
                }
            }
        }
        for (auto& sg : seg) std::reverse(sg.begin(), sg.end());
    }
    for (uint8_t d : done) if (!d) return false;
    return true;
}

// ---------------------------------------------------------------- the ordinary AAC-LC raw data block

void putScale(BitOut& o, int sym) { o.put(kScaleCode[sym], kScaleBits[sym]); }

void putUnit(BitOut& o, int cb, const int* v) {
    const CbInfo& ci = cbInfo(cb);
    int t[4];
    for (int i = 0; i < ci.dim; i++) t[i] = ci.isSigned ? v[i] : std::abs(v[i]);
    int escMark[2] = {0, 0};
    if (cb == 11) for (int i = 0; i < 2; i++) if (t[i] > 15) { escMark[i] = t[i]; t[i] = 16; }
    const int idx = cbIndex(cb, t);
    o.put(ci.code[idx], ci.bits[idx]);
    if (!ci.isSigned) for (int i = 0; i < ci.dim; i++) if (v[i]) o.put(v[i] < 0 ? 1 : 0, 1);
    if (cb == 11) {
        for (int i = 0; i < 2; i++) {
            if (!escMark[i]) continue;
            int nb = 4;
            while ((escMark[i] >> (nb + 1)) != 0) nb++;       // value = 2^nb + word, word < 2^nb
            for (int k = 4; k < nb; k++) o.put(1, 1);
            o.put(0, 1);
            o.put((uint32_t)(escMark[i] - (1 << nb)), nb);
        }
    }
}

std::vector<uint8_t> writeRaw(const Ics& s, const uint8_t* frame, int rateHz) {
    BitOut o;
    o.put(0, 3);                                            // ID_SCE
    o.put(0, 4);                                            // element_instance_tag
    o.put((uint32_t)s.gg, 8);
    o.put(0, 1); o.put((uint32_t)s.ws, 2); o.put((uint32_t)s.shape, 1);
    if (s.shortWin()) { o.put((uint32_t)s.maxSfb, 4); o.put((uint32_t)s.grouping, 7); }
    else { o.put((uint32_t)s.maxSfb, 6); o.put(0, 1); }     // predictor_data_present
    const int secBits = s.shortWin() ? 3 : 5, secEsc = (1 << secBits) - 1;
    for (int g = 0; g < s.numGroups; g++)
        for (const Sect& sc : s.sect[g]) {
            o.put((uint32_t)(sc.cb >= 16 ? 11 : sc.cb), 4);
            int len = sc.end - sc.start;
            while (len >= secEsc) { o.put((uint32_t)secEsc, secBits); len -= secEsc; }
            o.put((uint32_t)len, secBits);
        }
    for (int sym : s.sfSym) putScale(o, sym);
    o.put(0, 1);                                            // pulse_data_present
    o.put(s.tns ? 1 : 0, 1);
    if (s.tns) o.bitsFrom(frame, s.tnsBegin, s.tnsEnd);
    o.put(0, 1);                                            // gain_control_data_present
    // spectral data in the order of the standard: the tuples of every section in increasing position (the groups of short windows one after the other)
    int nL = 0, nS = 0;
    const uint16_t* swb = s.shortWin() ? swbShort(rateHz, nS) : swbLong(rateHz, nL);
    const int nshort = s.shortWin() ? 120 : kFrame;
    int spBase = 0;
    for (int g = 0; g < s.numGroups; g++) {
        for (const Sect& sc : s.sect[g]) {
            if (sc.cb == 0) continue;
            const int cb = sc.cb >= 16 ? 11 : sc.cb;
            const int dim = cbInfo(cb).dim;
            for (int sp = spBase + s.groupLen[g] * swb[sc.start]; sp < spBase + s.groupLen[g] * swb[sc.end]; sp += dim) putUnit(o, cb, s.q + sp);
        }
        spBase += nshort * s.groupLen[g];
    }
    o.put(7, 3);                                            // ID_END
    return o.b;
}

} // namespace

// ---------------------------------------------------------------- the decoder

struct AacCoreDecoder::Impl {
    int rate = 0;
    bool stereo = false;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    uint64_t unsupported = 0, damaged = 0, decoded = 0;
    std::string lastError;
    std::vector<uint8_t> bits;
    std::vector<float> last;          // the last good frame, for the fade out when the next one is lost
    bool prevOk = false;

    ~Impl() { close(); }
    void close() {
        if (ctx) avcodec_free_context(&ctx);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
    }
    bool open() {
        close();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
        if (!codec) { lastError = "no AAC decoder in this build"; return false; }
        ctx = avcodec_alloc_context3(codec);
        ctx->log_level_offset = 24;
        // AudioSpecificConfig: AAC-LC, core rate, mono, 960 samples per frame
        BitOut a;
        a.put(2, 5);
        a.put(rate == 12000 ? 9u : 6u, 4);
        a.put(1, 4);
        a.put(1, 1); a.put(0, 1); a.put(0, 1);
        ctx->extradata = (uint8_t*)av_mallocz(a.b.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        std::memcpy(ctx->extradata, a.b.data(), a.b.size());
        ctx->extradata_size = (int)a.b.size();
        if (avcodec_open2(ctx, codec, nullptr) < 0) { avcodec_free_context(&ctx); lastError = "the AAC decoder would not open"; return false; }
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        return true;
    }
};

AacCoreDecoder::AacCoreDecoder() : p_(std::make_unique<Impl>()) {}
AacCoreDecoder::~AacCoreDecoder() = default;

bool AacCoreDecoder::configure(int rateHz, bool stereo, std::string* why) {
    Impl& d = *p_;
    d.rate = rateHz; d.stereo = stereo;
    d.unsupported = d.damaged = d.decoded = 0;
    if (rateHz != 12000 && rateHz != 24000) { if (why) *why = "AAC core rate not supported"; return false; }
    if (stereo) { if (why) *why = "AAC stereo (not verified against a real stream)"; return false; }
    if (!d.open()) { if (why) *why = d.lastError; return false; }
    return true;
}
bool AacCoreDecoder::open() const { return p_->ctx != nullptr; }
int AacCoreDecoder::rateHz() const { return p_->rate; }
uint64_t AacCoreDecoder::framesDamaged() const { return p_->damaged; }
uint64_t AacCoreDecoder::framesUnsupported() const { return p_->unsupported; }
uint64_t AacCoreDecoder::framesDecoded() const { return p_->decoded; }
void AacCoreDecoder::reset() { if (p_->ctx) avcodec_flush_buffers(p_->ctx); p_->prevOk = false; }

AacFrameStatus AacCoreDecoder::frame(const uint8_t* data, int len, uint8_t crc, std::vector<float>& out) {
    Impl& d = *p_;
    const size_t start = out.size();
    // a frame that is lost: the last good one fades out over the frame, later ones are silent; the first good frame after a loss fades in
    // (its overlap with the missing frame is wrong)
    auto lost = [&](AacFrameStatus st) {
        out.resize(start + (size_t)kFrame, 0.f);
        if (d.prevOk && d.last.size() == (size_t)kFrame)
            for (int i = 0; i < kFrame; i++) out[start + (size_t)i] = d.last[(size_t)i] * (1.f - (float)i / kFrame);
        d.prevOk = false;
        if (d.ctx) avcodec_flush_buffers(d.ctx);
        return st;
    };
    if (!d.ctx || len < 4) return lost(AacFrameStatus::kDamaged);
    Ics s;
    const Result r = parseSi(data, (size_t)len * 8, d.rate, s);
    if (r == kDamaged) { d.damaged++; return lost(AacFrameStatus::kDamaged); }
    // the CRC of the super frame covers the side information and the TNS data
    {
        std::vector<uint8_t> b(s.tnsEnd);
        for (size_t i = 0; i < s.tnsEnd; i++) b[i] = (uint8_t)((data[i >> 3] >> (7 - (i & 7))) & 1);
        if (crc8(b.data(), b.size()) != crc) { d.damaged++; return lost(AacFrameStatus::kDamaged); }
    }
    if (r == kUnsupported) { d.unsupported++; return lost(AacFrameStatus::kUnsupported); }
    d.bits.resize((size_t)s.L);
    for (int i = 0; i < s.L; i++) { const size_t p = s.dataBegin + (size_t)i; d.bits[(size_t)i] = (uint8_t)((data[p >> 3] >> (7 - (p & 7))) & 1); }
    if (!hcrDecode(s, d.rate, d.bits.data())) { d.damaged++; return lost(AacFrameStatus::kDamaged); }
    const std::vector<uint8_t> raw = writeRaw(s, data, d.rate);
    if (av_new_packet(d.pkt, (int)raw.size()) < 0) return lost(AacFrameStatus::kDamaged);
    std::memcpy(d.pkt->data, raw.data(), raw.size());
    const int sr = avcodec_send_packet(d.ctx, d.pkt);
    av_packet_unref(d.pkt);
    if (sr < 0) { d.damaged++; return lost(AacFrameStatus::kDamaged); }
    size_t got = 0;
    while (avcodec_receive_frame(d.ctx, d.frame) == 0) {
        const int n = d.frame->nb_samples;
        const float* ch = (const float*)d.frame->data[0];
        out.resize(out.size() + (size_t)n);
        if (d.frame->format == AV_SAMPLE_FMT_FLTP || d.frame->format == AV_SAMPLE_FMT_FLT) std::memcpy(out.data() + start + got, ch, (size_t)n * sizeof(float));
        got += (size_t)n;
        av_frame_unref(d.frame);
    }
    if (got != (size_t)kFrame) { out.resize(start); d.damaged++; return lost(AacFrameStatus::kDamaged); }
    if (!d.prevOk) for (int i = 0; i < kFrame; i++) out[start + (size_t)i] *= (float)i / kFrame;
    d.last.assign(out.begin() + (ptrdiff_t)start, out.end());
    d.prevOk = true;
    d.decoded++;
    return AacFrameStatus::kOk;
}

} } // namespace dect2::drm
