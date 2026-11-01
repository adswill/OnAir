// JPEG 2000 decoder (j2k.h): codestream syntax (Annex A), packets and tag trees (Annex B), the MQ coder and the bit-plane coding passes
// (Annexes C and D), dequantisation (E) and the inverse wavelet transforms (F), DC level shift (G).
#include "dect2/image/j2k.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace dect2::j2k {

namespace {

inline int ceilDiv(int a, int b) { return (a + b - 1) / b; }          // a >= 0
inline int ceilShift(int a, int s) { return (int)(((int64_t)a + ((int64_t)1 << s) - 1) >> s); }
inline int floorShift(int a, int s) { return a >> s; }

// ------------------------------------------------------------------------------------------------ MQ decoder
struct QeRow { uint16_t qe; uint8_t nmps, nlps, sw; };
const QeRow kQe[47] = {
    {0x5601,1,1,1},{0x3401,2,6,0},{0x1801,3,9,0},{0x0AC1,4,12,0},{0x0521,5,29,0},{0x0221,38,33,0},{0x5601,7,6,1},{0x5401,8,14,0},
    {0x4801,9,14,0},{0x3801,10,14,0},{0x3001,11,17,0},{0x2401,12,18,0},{0x1C01,13,20,0},{0x1601,29,21,0},{0x5601,15,14,1},{0x5401,16,14,0},
    {0x5101,17,15,0},{0x4801,18,16,0},{0x3801,19,17,0},{0x3401,20,18,0},{0x3001,21,19,0},{0x2801,22,19,0},{0x2401,23,20,0},{0x2201,24,21,0},
    {0x1C01,25,22,0},{0x1801,26,23,0},{0x1601,27,24,0},{0x1401,28,25,0},{0x1201,29,26,0},{0x1101,30,27,0},{0x0AC1,31,28,0},{0x09C1,32,29,0},
    {0x08A1,33,30,0},{0x0521,34,31,0},{0x0441,35,32,0},{0x02A1,36,33,0},{0x0221,37,34,0},{0x0141,38,35,0},{0x0111,39,36,0},{0x0085,40,37,0},
    {0x0049,41,38,0},{0x0025,42,39,0},{0x0015,43,40,0},{0x0009,44,41,0},{0x0005,45,42,0},{0x0001,45,43,0},{0x5601,46,46,0}};

struct Mq {
    const uint8_t* d = nullptr;
    size_t n = 0, bp = 0;
    uint32_t a = 0, c = 0;
    int ct = 0;
    uint8_t st[19], mps[19];
    void resetCtx() {
        memset(st, 0, sizeof st); memset(mps, 0, sizeof mps);
        st[0] = 4; st[17] = 3; st[18] = 46;
    }
    uint8_t byteAt(size_t i) const { return i < n ? d[i] : 0xFF; }
    void bytein() {
        if (bp < n) {
            const uint32_t c1 = byteAt(bp + 1);
            if (d[bp] == 0xFF) {
                if (c1 > 0x8F) { c += 0xFF00; ct = 8; }
                else { bp++; c += c1 << 9; ct = 7; }
            } else { bp++; c += c1 << 8; ct = 8; }
        } else { c += 0xFF00; ct = 8; }
    }
    void init(const uint8_t* data, size_t len) {
        d = data; n = len; bp = 0;
        c = (uint32_t)byteAt(0) << 16;
        bytein();
        c <<= 7; ct -= 7; a = 0x8000;
    }
    int code(int cx, int /*actual*/) { return decode(cx); }
    int decode(int cx) {
        const QeRow& q = kQe[st[cx]];
        int bit;
        a -= q.qe;
        if ((c >> 16) < q.qe) {
            if (a < q.qe) { bit = mps[cx]; st[cx] = q.nmps; }
            else { bit = 1 - mps[cx]; if (q.sw) mps[cx] ^= 1; st[cx] = q.nlps; }
            a = q.qe;
            renorm();
        } else {
            c -= q.qe << 16;
            if ((a & 0x8000) == 0) {
                if (a < q.qe) { bit = 1 - mps[cx]; if (q.sw) mps[cx] ^= 1; st[cx] = q.nlps; }
                else { bit = mps[cx]; st[cx] = q.nmps; }
                renorm();
            } else bit = mps[cx];
        }
        return bit;
    }
    void renorm() {
        do { if (ct == 0) bytein(); a <<= 1; c <<= 1; ct--; } while ((a & 0x8000) == 0);
    }
};

// ------------------------------------------------------------------------------------------------ tag tree
struct TagTree {
    struct Node { int value = 0x7fffffff, low = 0; int parent = -1; };
    std::vector<Node> nodes;
    int w = 0, h = 0;
    void init(int W, int H) {
        w = W; h = H;
        nodes.clear();
        if (W <= 0 || H <= 0) return;
        std::vector<int> lw{W}, lh{H}, off{0};
        while (lw.back() > 1 || lh.back() > 1) { lw.push_back((lw.back() + 1) / 2); lh.push_back((lh.back() + 1) / 2); }
        int tot = 0;
        for (size_t i = 0; i < lw.size(); i++) { off.resize(lw.size()); off[i] = tot; tot += lw[i] * lh[i]; }
        nodes.assign((size_t)tot, Node());
        for (size_t l = 0; l + 1 < lw.size(); l++)
            for (int y = 0; y < lh[l]; y++)
                for (int x = 0; x < lw[l]; x++) nodes[(size_t)(off[l] + y * lw[l] + x)].parent = off[l + 1] + (y / 2) * lw[l + 1] + x / 2;
    }
};

// ------------------------------------------------------------------------------------------------ bit reader of packet headers
struct HdrBits {
    const uint8_t* d; size_t n, pos = 0;
    int cur = 0, left = 0;
    bool prevFF = false, over = false;
    HdrBits(const uint8_t* p, size_t len) : d(p), n(len) {}
    int bit() {
        if (left == 0) {
            if (pos >= n) { over = true; return 0; }
            cur = d[pos++];
            left = prevFF ? 7 : 8;
            prevFF = cur == 0xFF;
        }
        left--;
        return (cur >> left) & 1;
    }
    uint32_t bits(int k) { uint32_t v = 0; while (k-- > 0) v = v << 1 | (uint32_t)bit(); return v; }
    void align() {
        left = 0;
        if (prevFF) { if (pos < n) pos++; prevFF = false; }
    }
};

int tagDecode(TagTree& t, int leaf, int threshold, HdrBits& br) {
    int stk[32], k = 0;
    for (int i = leaf; i >= 0 && k < 32; i = t.nodes[(size_t)i].parent) stk[k++] = i;
    int low = 0;
    while (k > 0) {
        TagTree::Node& nd = t.nodes[(size_t)stk[--k]];
        if (low > nd.low) nd.low = low; else low = nd.low;
        while (low < threshold && low < nd.value) { if (br.bit()) nd.value = low; else low++; }
        nd.low = low;
    }
    return t.nodes[(size_t)leaf].value < threshold;
}

// ------------------------------------------------------------------------------------------------ structures
struct CodeBlock {
    int x0, y0, x1, y1;                // in the subband
    bool included = false;
    int zbp = 0, lblock = 3, passes = 0;
    std::vector<uint8_t> data;
    std::vector<std::pair<int, int>> segs;   // (passes, bytes) of each codeword segment
};
struct PrecBand {                       // a subband inside one precinct
    int cbx0 = 0, cby0 = 0, ncw = 0, nch = 0;
    std::vector<CodeBlock> cbs;
    TagTree incl, zbpTree;
};
struct Band {
    int orient = 0;                    // 0 LL, 1 HL, 2 LH, 3 HH
    int x0, y0, x1, y1;
    int mb = 0;                        // magnitude bit planes
    float step = 1;
    int xoff = 0, yoff = 0;            // position in the resolution's array
};
struct Precinct { std::vector<PrecBand> pb; };
struct Resolution {
    int x0, y0, x1, y1;
    int ppx, ppy, npw, nph;
    std::vector<Band> bands;
    std::vector<Precinct> precs;
};
struct CompParams {
    int levels = 5, cbw = 6, cbh = 6, style = 0, transform = 1;   // cb exponents; transform 1 = 5/3
    std::vector<int> ppx, ppy;
    int qstyle = 0, guard = 2;
    std::vector<int> expn, mant;
    int prec = 8; bool sgnd = false;
    int xr = 1, yr = 1;
};
struct CodParams { int prog = 0, layers = 1, mct = 0; bool sop = false, eph = false; };

struct Ctx {
    std::string err;
    int xsiz = 0, ysiz = 0, xo = 0, yo = 0, xt = 0, yt = 0, xto = 0, yto = 0, ncomp = 0;
    std::vector<CompParams> cp;
    CodParams cod;
    bool codSeen = false;
};

// ------------------------------------------------------------------------------------------------ bit-plane decoder
template <class C>
struct T1 {
    C mq;
    const int32_t* src = nullptr;     // encoder: the signed coefficients, w x h (the passes then code what is there instead of reading it)
    int w = 0, h = 0, s = 0;
    std::vector<uint8_t> fl;   // bit0 significant, bit1 negative, bit2 refined, bit3 visited
    std::vector<int32_t> mag;
    int orient = 0; bool vcausal = false;

    uint8_t& F(int x, int y) { return fl[(size_t)((y + 1) * s + x + 1)]; }
    // neighbour significance counts (horizontal, vertical, diagonal) of (x, y)
    void nb(int x, int y, int& hh, int& vv, int& dd) {
        const uint8_t* p = fl.data() + (size_t)((y + 1) * s + x + 1);
        hh = (p[-1] & 1) + (p[1] & 1);
        vv = (p[-s] & 1);
        dd = (p[-s - 1] & 1) + (p[-s + 1] & 1);
        if (!(vcausal && (y & 3) == 3)) { vv += (p[s] & 1); dd += (p[s - 1] & 1) + (p[s + 1] & 1); }
    }
    int zc(int hh, int vv, int dd) const {
        if (orient == 1) std::swap(hh, vv);
        if (orient == 3) {
            const int hv = hh + vv;
            if (dd >= 3) return 8;
            if (dd == 2) return hv >= 1 ? 7 : 6;
            if (dd == 1) return hv >= 2 ? 5 : hv == 1 ? 4 : 3;
            return hv >= 2 ? 2 : hv;
        }
        if (hh == 2) return 8;
        if (hh == 1) return vv >= 1 ? 7 : dd >= 1 ? 6 : 5;
        if (vv == 2) return 4;
        if (vv == 1) return 3;
        return dd >= 2 ? 2 : dd;
    }
    // sign context and xor bit
    void signCtx(int x, int y, int& cx, int& xr) {
        const uint8_t* p = fl.data() + (size_t)((y + 1) * s + x + 1);
        auto contrib = [](uint8_t f) { return (f & 1) ? ((f & 2) ? -1 : 1) : 0; };
        int hc = contrib(p[-1]) + contrib(p[1]);
        int vc = contrib(p[-s]) + ((vcausal && (y & 3) == 3) ? 0 : contrib(p[s]));
        hc = std::max(-1, std::min(1, hc)); vc = std::max(-1, std::min(1, vc));
        static const int tcx[3][3] = {{13, 12, 11}, {10, 9, 10}, {11, 12, 13}};   // [h+1][v+1]
        static const int txr[3][3] = {{1, 1, 1}, {1, 0, 0}, {0, 0, 0}};
        // h = 1: v = 1 -> 13, v = 0 -> 12, v = -1 -> 11 (xor 0); h = 0: v = 1 -> 10, 0 -> 9, -1 -> 10 (xor 1); h = -1 mirrors with xor 1
        cx = tcx[hc + 1][vc + 1];
        xr = (hc < 0 || (hc == 0 && vc < 0)) ? 1 : 0;
        (void)txr;
    }
    int absAt(int x, int y) const { return std::abs(src[(size_t)(y * w + x)]); }
    int bitAt(int x, int y, int bp) const { return src ? (absAt(x, y) >> bp) & 1 : 0; }
    void setSig(int x, int y, int bp) {
        const int neg = decodeSign(x, y);
        F(x, y) |= (uint8_t)(1 | (neg ? 2 : 0));
        mag[(size_t)(y * w + x)] = 1 << bp;
    }
    int decodeSign(int x, int y) {
        int cx, xr;
        signCtx(x, y, cx, xr);
        const int act = src ? ((src[(size_t)(y * w + x)] < 0 ? 1 : 0) ^ xr) : 0;
        return mq.code(cx, act) ^ xr;
    }

    void sigProp(int bp) {
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x = 0; x < w; x++)
                for (int y = y0; y < std::min(h, y0 + 4); y++) {
                    uint8_t& f = F(x, y);
                    if (f & 1) continue;
                    int hh, vv, dd;
                    nb(x, y, hh, vv, dd);
                    if (hh + vv + dd == 0) continue;
                    f |= 8;
                    if (mq.code(zc(hh, vv, dd), bitAt(x, y, bp))) setSig(x, y, bp);
                }
    }
    void magRef(int bp) {
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x = 0; x < w; x++)
                for (int y = y0; y < std::min(h, y0 + 4); y++) {
                    uint8_t& f = F(x, y);
                    if ((f & 1) == 0 || (f & 8)) continue;
                    int cx = 16;
                    if (!(f & 4)) { int hh, vv, dd; nb(x, y, hh, vv, dd); cx = hh + vv + dd > 0 ? 15 : 14; }
                    const int b = mq.code(cx, bitAt(x, y, bp));
                    mag[(size_t)(y * w + x)] |= b << bp;
                    f |= 4;
                }
    }
    void cleanup(int bp, bool segsym) {
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x = 0; x < w; x++) {
                int y = y0;
                const int ye = std::min(h, y0 + 4);
                if (y0 + 4 <= h) {
                    bool allQuiet = true;
                    for (int k = 0; k < 4 && allQuiet; k++) {
                        const uint8_t f = F(x, y0 + k);
                        if (f & 9) { allQuiet = false; break; }
                        int hh, vv, dd; nb(x, y0 + k, hh, vv, dd);
                        if (hh + vv + dd) allQuiet = false;
                    }
                    if (allQuiet) {
                        int firstAct = -1;
                        if (src) for (int k = 0; k < 4; k++) if (bitAt(x, y0 + k, bp)) { firstAct = k; break; }
                        if (!mq.code(17, firstAct >= 0)) continue;
                        const int hi = mq.code(18, firstAct >> 1 & 1);
                        const int r = hi << 1 | mq.code(18, firstAct & 1);
                        y = y0 + r;
                        setSig(x, y, bp);
                        y++;
                    }
                }
                for (; y < ye; y++) {
                    uint8_t& f = F(x, y);
                    if (f & 9) continue;
                    int hh, vv, dd;
                    nb(x, y, hh, vv, dd);
                    if (mq.code(zc(hh, vv, dd), bitAt(x, y, bp))) setSig(x, y, bp);
                }
            }
        for (auto& f : fl) f &= (uint8_t)~8;
        if (segsym) { static const int sym[4] = {1, 0, 1, 0}; for (int i = 0; i < 4; i++) mq.code(18, sym[i]); }
    }
};

// ------------------------------------------------------------------------------------------------ the decoder
struct TileComp {
    int x0, y0, x1, y1;
    std::vector<Resolution> res;
    std::vector<float> samples;      // x1-x0 by y1-y0 once transformed
};
struct Tile {
    int idx = 0, x0, y0, x1, y1;
    std::vector<CompParams> cp;
    CodParams cod;
    std::vector<TileComp> tc;
    std::vector<uint8_t> data;       // packet data of all tile parts
    bool seen = false;
};

class Decoder {
public:
    Ctx c;
    Image img;
    std::vector<Tile> tiles;
    int ntx = 0, nty = 0;

    uint32_t u16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
    uint32_t u32(const uint8_t* p) { return u16(p) << 16 | u16(p + 2); }

    bool fail(const char* m) { if (img.error.empty()) img.error = m; return false; }

    bool parseCod(const uint8_t* p, size_t len, CodParams& cod, std::vector<CompParams>& cps, int comp /* -1 all */, bool isCoc) {
        size_t o = 0;
        if (!isCoc) {
            if (len < 10) return fail("short COD");
            const int scod = p[0];
            cod.sop = scod & 2; cod.eph = scod & 4;
            cod.prog = p[1]; cod.layers = (int)u16(p + 2); cod.mct = p[4];
            o = 5;
            return parseSp(p + o, len - o, scod & 1, cps, comp);
        }
        return fail("COC");
    }
    bool parseSp(const uint8_t* p, size_t len, bool precincts, std::vector<CompParams>& cps, int comp) {
        if (len < 5) return fail("short COD");
        CompParams q;
        q.levels = p[0]; q.cbw = (p[1] & 15) + 2; q.cbh = (p[2] & 15) + 2; q.style = p[3]; q.transform = p[4];
        if (q.levels > 32 || q.cbw > 10 || q.cbh > 10 || q.cbw + q.cbh > 12) return fail("bad code-block size");
        q.ppx.assign((size_t)q.levels + 1, 15); q.ppy.assign((size_t)q.levels + 1, 15);
        if (precincts) {
            if (len < 5 + (size_t)q.levels + 1) return fail("short COD");
            for (int i = 0; i <= q.levels; i++) { q.ppx[(size_t)i] = p[5 + i] & 15; q.ppy[(size_t)i] = p[5 + i] >> 4; }
        }
        for (int i = 0; i < (int)cps.size(); i++)
            if (comp < 0 || comp == i) {
                CompParams& d = cps[(size_t)i];
                d.levels = q.levels; d.cbw = q.cbw; d.cbh = q.cbh; d.style = q.style; d.transform = q.transform; d.ppx = q.ppx; d.ppy = q.ppy;
            }
        return true;
    }
    bool parseQcd(const uint8_t* p, size_t len, std::vector<CompParams>& cps, int comp) {
        if (len < 1) return fail("short QCD");
        const int sq = p[0], style = sq & 31, guard = sq >> 5;
        std::vector<int> e, m;
        if (style == 0) { for (size_t i = 1; i < len; i++) { e.push_back(p[i] >> 3); m.push_back(0); } }
        else if (style == 1 || style == 2) { for (size_t i = 1; i + 1 < len; i += 2) { const uint32_t v = u16(p + i); e.push_back((int)(v >> 11)); m.push_back((int)(v & 0x7FF)); } }
        else return fail("unknown quantisation style");
        if (e.empty()) return fail("short QCD");
        for (int i = 0; i < (int)cps.size(); i++)
            if (comp < 0 || comp == i) { CompParams& d = cps[(size_t)i]; d.qstyle = style; d.guard = guard; d.expn = e; d.mant = m; }
        return true;
    }

    bool mainHeader(const uint8_t* d, size_t n, size_t& pos) {
        if (n < 4 || d[0] != 0xFF || d[1] != 0x4F) return fail("no codestream");
        pos = 2;
        bool siz = false;
        while (pos + 4 <= n) {
            const int m = d[pos] << 8 | d[pos + 1];
            if (m == 0xFF90) return siz && c.codSeen ? true : fail("missing COD");
            const size_t L = u16(d + pos + 2);
            if (L < 2 || pos + 2 + L > n) return fail("truncated header");
            const uint8_t* p = d + pos + 4;
            const size_t len = L - 2;
            switch (m) {
            case 0xFF51: {
                if (len < 36) return fail("short SIZ");
                c.xsiz = (int)u32(p + 2); c.ysiz = (int)u32(p + 6); c.xo = (int)u32(p + 10); c.yo = (int)u32(p + 14);
                c.xt = (int)u32(p + 18); c.yt = (int)u32(p + 22); c.xto = (int)u32(p + 26); c.yto = (int)u32(p + 30);
                c.ncomp = (int)u16(p + 34);
                if (c.ncomp < 1 || c.ncomp > 16 || len < 36 + (size_t)c.ncomp * 3) return fail("bad SIZ");
                if (c.xsiz <= c.xo || c.ysiz <= c.yo || c.xt <= 0 || c.yt <= 0 || (int64_t)(c.xsiz - c.xo) * (c.ysiz - c.yo) > (int64_t)40000 * 40000) return fail("bad image size");
                c.cp.assign((size_t)c.ncomp, CompParams());
                for (int i = 0; i < c.ncomp; i++) {
                    const uint8_t s = p[36 + 3 * i];
                    c.cp[(size_t)i].prec = (s & 0x7F) + 1; c.cp[(size_t)i].sgnd = s >> 7;
                    c.cp[(size_t)i].xr = std::max(1, (int)p[37 + 3 * i]); c.cp[(size_t)i].yr = std::max(1, (int)p[38 + 3 * i]);
                }
                siz = true;
                break;
            }
            case 0xFF52: if (!siz) return fail("COD before SIZ"); if (!parseCod(p, len, c.cod, c.cp, -1, false)) return false; c.codSeen = true; break;
            case 0xFF53: {
                if (!siz) return fail("COC before SIZ");
                const size_t ci = c.ncomp < 257 ? 1 : 2;
                if (len < ci + 1 + 5) return fail("short COC");
                const int comp = ci == 1 ? p[0] : (int)u16(p);
                if (comp >= c.ncomp) return fail("bad COC");
                if (!parseSp(p + ci + 1, len - ci - 1, p[ci] & 1, c.cp, comp)) return false;
                break;
            }
            case 0xFF5C: if (!siz) return fail("QCD before SIZ"); if (!parseQcd(p, len, c.cp, -1)) return false; break;
            case 0xFF5D: {
                const size_t ci = c.ncomp < 257 ? 1 : 2;
                if (len < ci + 1) return fail("short QCC");
                const int comp = ci == 1 ? p[0] : (int)u16(p);
                if (comp >= c.ncomp) return fail("bad QCC");
                if (!parseQcd(p + ci, len - ci, c.cp, comp)) return false;
                break;
            }
            case 0xFF5E: return fail("region of interest not supported");
            case 0xFF60: case 0xFF61: return fail("packed packet headers not supported");
            default: break;
            }
            pos += 2 + L;
        }
        return fail("no tile data");
    }

    // ---- tile geometry
    void buildTile(Tile& t) {
        t.tc.assign((size_t)c.ncomp, TileComp());
        for (int ci = 0; ci < c.ncomp; ci++) {
            const CompParams& q = t.cp[(size_t)ci];
            TileComp& tc = t.tc[(size_t)ci];
            tc.x0 = ceilDiv(t.x0, q.xr); tc.y0 = ceilDiv(t.y0, q.yr); tc.x1 = ceilDiv(t.x1, q.xr); tc.y1 = ceilDiv(t.y1, q.yr);
            const int NL = q.levels;
            tc.res.assign((size_t)NL + 1, Resolution());
            for (int r = 0; r <= NL; r++) {
                Resolution& R = tc.res[(size_t)r];
                const int sc = NL - r;
                R.x0 = ceilShift(tc.x0, sc); R.y0 = ceilShift(tc.y0, sc); R.x1 = ceilShift(tc.x1, sc); R.y1 = ceilShift(tc.y1, sc);
                R.ppx = q.ppx[(size_t)r]; R.ppy = q.ppy[(size_t)r];
                R.npw = R.x1 > R.x0 ? ceilShift(R.x1, R.ppx) - floorShift(R.x0, R.ppx) : 0;
                R.nph = R.y1 > R.y0 ? ceilShift(R.y1, R.ppy) - floorShift(R.y0, R.ppy) : 0;
                // bands
                const int nbands = r == 0 ? 1 : 3;
                for (int b = 0; b < nbands; b++) {
                    Band B;
                    const int orient = r == 0 ? 0 : b + 1;
                    const int nb = r == 0 ? NL : NL - r + 1;
                    const int xob = (orient == 1 || orient == 3), yob = (orient == 2 || orient == 3);
                    B.orient = orient;
                    if (nb == 0) { B.x0 = tc.x0; B.y0 = tc.y0; B.x1 = tc.x1; B.y1 = tc.y1; }
                    else {
                        const int64_t sx = (int64_t)xob << (nb - 1), sy = (int64_t)yob << (nb - 1);
                        auto cs = [&](int64_t v) { return (int)((v + ((int64_t)1 << nb) - 1) >> nb); };
                        B.x0 = cs(tc.x0 - sx); B.y0 = cs(tc.y0 - sy); B.x1 = cs(tc.x1 - sx); B.y1 = cs(tc.y1 - sy);
                    }
                    const int idx = r == 0 ? 0 : 1 + 3 * (r - 1) + b;
                    int e, mant = 0;
                    if (q.qstyle == 1) { e = q.expn[0] - NL + (r == 0 ? NL : NL - r + 1); mant = q.mant[0]; if (r == 0) e = q.expn[0]; }
                    else { const size_t k = std::min<size_t>((size_t)idx, q.expn.size() - 1); e = q.expn[k]; mant = q.mant[k]; }
                    B.mb = q.guard + e - 1;
                    const int gain = orient == 0 ? 0 : orient == 3 ? 2 : 1;
                    if (q.transform == 1 && q.qstyle == 0) B.step = 1.f;
                    else B.step = (float)std::ldexp(1.0 + mant / 2048.0, q.prec + gain - e);
                    R.bands.push_back(B);
                }
                // precincts and code-blocks
                R.precs.assign((size_t)R.npw * R.nph, Precinct());
                const int pw = r == 0 ? R.ppx : R.ppx - 1, ph = r == 0 ? R.ppy : R.ppy - 1;
                const int xcb = std::min(q.cbw, pw), ycb = std::min(q.cbh, ph);
                for (int py = 0; py < R.nph; py++)
                    for (int px = 0; px < R.npw; px++) {
                        Precinct& P = R.precs[(size_t)(py * R.npw + px)];
                        P.pb.assign(R.bands.size(), PrecBand());
                        for (size_t bi = 0; bi < R.bands.size(); bi++) {
                            const Band& B = R.bands[bi];
                            PrecBand& PB = P.pb[bi];
                            // precinct bounds in the subband
                            int bx0, by0, bx1, by1;
                            const int rx0 = (floorShift(R.x0, R.ppx) + px) << R.ppx, ry0 = (floorShift(R.y0, R.ppy) + py) << R.ppy;
                            if (r == 0) { bx0 = rx0; by0 = ry0; bx1 = rx0 + (1 << R.ppx); by1 = ry0 + (1 << R.ppy); }
                            else { bx0 = rx0 >> 1; by0 = ry0 >> 1; bx1 = (rx0 + (1 << R.ppx)) >> 1; by1 = (ry0 + (1 << R.ppy)) >> 1; }
                            bx0 = std::max(bx0, B.x0); by0 = std::max(by0, B.y0); bx1 = std::min(bx1, B.x1); by1 = std::min(by1, B.y1);
                            if (bx1 <= bx0 || by1 <= by0) continue;
                            const int cx0 = floorShift(bx0, xcb), cy0 = floorShift(by0, ycb), cx1 = ceilShift(bx1, xcb), cy1 = ceilShift(by1, ycb);
                            PB.ncw = cx1 - cx0; PB.nch = cy1 - cy0; PB.cbx0 = cx0; PB.cby0 = cy0;
                            PB.cbs.assign((size_t)PB.ncw * PB.nch, CodeBlock());
                            for (int cy = 0; cy < PB.nch; cy++)
                                for (int cx = 0; cx < PB.ncw; cx++) {
                                    CodeBlock& cb = PB.cbs[(size_t)(cy * PB.ncw + cx)];
                                    cb.x0 = std::max(bx0, (cx0 + cx) << xcb); cb.x1 = std::min(bx1, (cx0 + cx + 1) << xcb);
                                    cb.y0 = std::max(by0, (cy0 + cy) << ycb); cb.y1 = std::min(by1, (cy0 + cy + 1) << ycb);
                                }
                            PB.incl.init(PB.ncw, PB.nch); PB.zbpTree.init(PB.ncw, PB.nch);
                        }
                    }
            }
        }
    }

    // ---- one packet; returns bytes consumed or -1
    long packet(Tile& t, int comp, int r, int prec, int layer, const uint8_t* d, size_t n) {
        const CompParams& q = t.cp[(size_t)comp];
        Resolution& R = t.tc[(size_t)comp].res[(size_t)r];
        if (prec >= (int)R.precs.size()) return 0;   // an empty resolution has no packet bodies, but still a packet: handled by caller
        Precinct& P = R.precs[(size_t)prec];
        size_t pos = 0;
        if (t.cod.sop) { if (n >= 6 && d[0] == 0xFF && d[1] == 0x91) pos = 6; }
        HdrBits br(d + pos, n - pos);
        struct Inc { CodeBlock* cb; int newPasses; std::vector<int> lens; };
        std::vector<Inc> incs;
        if (br.bit()) {
            for (auto& PB : P.pb) {
                for (size_t k = 0; k < PB.cbs.size(); k++) {
                    CodeBlock& cb = PB.cbs[k];
                    bool inc;
                    if (!cb.included) inc = tagDecode(PB.incl, (int)k, layer + 1, br) != 0;
                    else inc = br.bit() != 0;
                    if (!inc) continue;
                    if (!cb.included) {
                        cb.included = true;
                        int z = 1;
                        while (!tagDecode(PB.zbpTree, (int)k, z, br)) { z++; if (z > 80 || br.over) return -1; }
                        cb.zbp = z - 1;
                    }
                    int np;
                    if (!br.bit()) np = 1;
                    else if (!br.bit()) np = 2;
                    else {
                        const int v = (int)br.bits(2);
                        if (v < 3) np = 3 + v;
                        else { const int w5 = (int)br.bits(5); np = w5 < 31 ? 6 + w5 : 37 + (int)br.bits(7); }
                    }
                    while (br.bit()) { cb.lblock++; if (cb.lblock > 32 || br.over) return -1; }
                    Inc in{&cb, np, {}};
                    if (q.style & 4) {          // every pass ends its codeword segment
                        for (int i = 0; i < np; i++) in.lens.push_back((int)br.bits(cb.lblock));
                    } else {
                        int lg = 0; while ((2 << lg) <= np) lg++;
                        in.lens.push_back((int)br.bits(cb.lblock + lg));
                    }
                    if (br.over) return -1;
                    incs.push_back(std::move(in));
                }
            }
        }
        br.align();
        pos += br.pos;
        if (t.cod.eph && pos + 2 <= n && d[pos] == 0xFF && d[pos + 1] == 0x92) pos += 2;
        for (auto& in : incs) {
            size_t tot = 0;
            for (int l : in.lens) tot += (size_t)l;
            if (pos + tot > n) tot = n > pos ? n - pos : 0;   // damaged: use what there is
            size_t off = pos;
            (void)off;
            if (q.style & 4) for (int l : in.lens) in.cb->segs.emplace_back(1, l);
            else if (in.cb->segs.empty()) in.cb->segs.emplace_back(in.newPasses, in.lens[0]);
            else { in.cb->segs[0].first += in.newPasses; in.cb->segs[0].second += in.lens[0]; }   // one codeword segment runs through all layers
            in.cb->data.insert(in.cb->data.end(), d + pos, d + pos + tot);
            in.cb->passes += in.newPasses;
            pos += tot;
        }
        return (long)pos;
    }

    // ---- packet sequence of a tile
    bool readPackets(Tile& t) {
        const uint8_t* d = t.data.data();
        size_t n = t.data.size(), pos = 0;
        int maxNL = 0;
        for (auto& q : t.cp) maxNL = std::max(maxNL, q.levels);
        bool bad = false;
        auto one = [&](int comp, int r, int prec, int layer) {
            if (bad) return;
            if (r > t.cp[(size_t)comp].levels) return;
            if (pos >= n) { bad = true; return; }
            const long used = packet(t, comp, r, prec, layer, d + pos, n - pos);
            if (used < 0) { bad = true; return; }
            pos += (size_t)used;
        };
        const int L = t.cod.layers;
        auto precCount = [&](int comp, int r) { return (int)t.tc[(size_t)comp].res[(size_t)r].precs.size(); };
        switch (t.cod.prog) {
        case 0:
            for (int l = 0; l < L; l++) for (int r = 0; r <= maxNL; r++) for (int ci = 0; ci < c.ncomp; ci++) {
                if (r > t.cp[(size_t)ci].levels) continue;
                for (int p = 0; p < precCount(ci, r); p++) one(ci, r, p, l);
            }
            break;
        case 1:
            for (int r = 0; r <= maxNL; r++) for (int l = 0; l < L; l++) for (int ci = 0; ci < c.ncomp; ci++) {
                if (r > t.cp[(size_t)ci].levels) continue;
                for (int p = 0; p < precCount(ci, r); p++) one(ci, r, p, l);
            }
            break;
        default: {
            // position based orders: RPCL (2), PCRL (3), CPRL (4); the precinct a reference-grid point belongs to
            auto precAt = [&](int ci, int r, int x, int y, int& out) {
                const CompParams& q = t.cp[(size_t)ci];
                if (r > q.levels) return false;
                const Resolution& R = t.tc[(size_t)ci].res[(size_t)r];
                if (R.npw == 0 || R.nph == 0) return false;
                const int sc = q.levels - r;
                const int64_t stepx = (int64_t)q.xr << (R.ppx + sc), stepy = (int64_t)q.yr << (R.ppy + sc);
                const bool okx = (x % stepx == 0) || (x == t.x0 && ((int64_t)R.x0 << sc) % ((int64_t)1 << (R.ppx + sc)) != 0);
                const bool oky = (y % stepy == 0) || (y == t.y0 && ((int64_t)R.y0 << sc) % ((int64_t)1 << (R.ppy + sc)) != 0);
                if (!okx || !oky) return false;
                const int px = floorShift(ceilShift(ceilDiv(x, q.xr), sc), R.ppx) - floorShift(R.x0, R.ppx);
                const int py = floorShift(ceilShift(ceilDiv(y, q.yr), sc), R.ppy) - floorShift(R.y0, R.ppy);
                if (px < 0 || py < 0 || px >= R.npw || py >= R.nph) return false;
                out = py * R.npw + px;
                return true;
            };
            int stepX = 1 << 30, stepY = 1 << 30;
            for (int ci = 0; ci < c.ncomp; ci++) for (int r = 0; r <= t.cp[(size_t)ci].levels; r++) {
                const Resolution& R = t.tc[(size_t)ci].res[(size_t)r];
                const int sc = t.cp[(size_t)ci].levels - r;
                stepX = std::min<int64_t>(stepX, (int64_t)t.cp[(size_t)ci].xr << std::min(30, R.ppx + sc));
                stepY = std::min<int64_t>(stepY, (int64_t)t.cp[(size_t)ci].yr << std::min(30, R.ppy + sc));
            }
            auto gridLoop = [&](auto&& body) {
                for (int y = t.y0; y < t.y1; y += stepY - (y % stepY))
                    for (int x = t.x0; x < t.x1; x += stepX - (x % stepX)) body(x, y);
            };
            if (t.cod.prog == 2) {
                for (int r = 0; r <= maxNL; r++) gridLoop([&](int x, int y) {
                    for (int ci = 0; ci < c.ncomp; ci++) { int p; if (precAt(ci, r, x, y, p)) for (int l = 0; l < L; l++) one(ci, r, p, l); }
                });
            } else if (t.cod.prog == 3) {
                gridLoop([&](int x, int y) {
                    for (int ci = 0; ci < c.ncomp; ci++) for (int r = 0; r <= t.cp[(size_t)ci].levels; r++) { int p; if (precAt(ci, r, x, y, p)) for (int l = 0; l < L; l++) one(ci, r, p, l); }
                });
            } else {
                for (int ci = 0; ci < c.ncomp; ci++) gridLoop([&](int x, int y) {
                    for (int r = 0; r <= t.cp[(size_t)ci].levels; r++) { int p; if (precAt(ci, r, x, y, p)) for (int l = 0; l < L; l++) one(ci, r, p, l); }
                });
            }
        }
        }
        return !bad;
    }

    // ---- code-blocks to wavelet coefficients, then the inverse transform
    void decodeBlock(const CodeBlock& cb, const Band& B, const CompParams& q, std::vector<float>& arr, int stride, T1<Mq>& t1) {
        const int w = cb.x1 - cb.x0, h = cb.y1 - cb.y0;
        if (w <= 0 || h <= 0 || cb.passes == 0) return;
        const int nbps = B.mb - cb.zbp;
        if (nbps <= 0 || nbps > 30) return;
        t1.w = w; t1.h = h; t1.s = w + 2; t1.orient = B.orient; t1.vcausal = (q.style & 8) != 0;
        t1.fl.assign((size_t)(w + 2) * (h + 2), 0);
        t1.mag.assign((size_t)w * h, 0);
        t1.mq.resetCtx();
        const bool resetCtx = q.style & 2, segsym = q.style & 32;
        size_t dataOff = 0;
        int pass = 0, type = 2, bp = nbps - 1;   // the first pass is a cleanup
        bool first = true;
        int lastBp = nbps - 1;
        for (const auto& sg : cb.segs) {
            const size_t len = std::min<size_t>((size_t)sg.second, cb.data.size() - std::min(dataOff, cb.data.size()));
            t1.mq.init(cb.data.data() + dataOff, len);
            dataOff += (size_t)sg.second;
            for (int k = 0; k < sg.first; k++) {
                if (bp < 0) break;
                if (type == 0) t1.sigProp(bp);
                else if (type == 1) t1.magRef(bp);
                else { t1.cleanup(bp, segsym); }
                lastBp = bp;
                if (resetCtx) t1.mq.resetCtx();
                pass++;
                if (type == 2) { type = 0; bp--; } else type++;
                first = false;
            }
        }
        // store as float with the sign; the part below the last decoded plane is taken as its half
        const bool full = lastBp == 0 && true;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t f = t1.fl[(size_t)((y + 1) * t1.s + x + 1)];
                if (!(f & 1)) continue;
                float v = (float)t1.mag[(size_t)(y * w + x)];
                if (!full || q.transform == 0) v += lastBp > 0 ? (float)(1 << (lastBp - 1)) : (q.transform == 0 ? 0.5f : 0.f);
                if (f & 2) v = -v;
                arr[(size_t)((B.yoff + (cb.y0 - B.y0) + y) * stride + B.xoff + (cb.x0 - B.x0) + x)] = v * B.step;
            }
    }

    static void lift53(float* a, int n, int i0, float* tmp) {
        // inverse 5/3 on a[0..n) whose first sample has coordinate i0 (parity decides low / high)
        if (n == 1) { if (i0 & 1) a[0] = std::floor(a[0] / 2); return; }
        const int par = i0 & 1;
        auto at = [&](int i) -> float {      // mirrored access, i relative to the first sample
            if (i < 0) i = -i;
            if (i >= n) i = 2 * (n - 1) - i;
            if (i < 0) i = 0;
            return a[i];
        };
        (void)tmp;
        // even coordinates (low): i with (i + par) even
        for (int i = (par ? 1 : 0); i < n; i += 2) a[i] = a[i] - std::floor((at(i - 1) + at(i + 1) + 2.f) / 4.f);
        for (int i = (par ? 0 : 1); i < n; i += 2) a[i] = a[i] + std::floor((at(i - 1) + at(i + 1)) / 2.f);
    }
    static void lift97(float* a, int n, int i0) {
        if (n == 1) { if (i0 & 1) a[0] *= 0.5f; return; }
        const float alpha = -1.586134342059924f, beta = -0.052980118572961f, gamma = 0.882911075530934f, delta = 0.443506852043971f, K = 1.230174104914001f;
        const int par = i0 & 1;
        auto at = [&](int i) -> float { if (i < 0) i = -i; if (i >= n) i = 2 * (n - 1) - i; if (i < 0) i = 0; return a[i]; };
        const int e0 = par ? 1 : 0, o0 = par ? 0 : 1;
        for (int i = e0; i < n; i += 2) a[i] *= K;
        for (int i = o0; i < n; i += 2) a[i] *= 1.f / K;
        for (int i = e0; i < n; i += 2) a[i] -= delta * (at(i - 1) + at(i + 1));
        for (int i = o0; i < n; i += 2) a[i] -= gamma * (at(i - 1) + at(i + 1));
        for (int i = e0; i < n; i += 2) a[i] -= beta * (at(i - 1) + at(i + 1));
        for (int i = o0; i < n; i += 2) a[i] -= alpha * (at(i - 1) + at(i + 1));
    }

    void reconstruct(Tile& t, int ci) {
        const CompParams& q = t.cp[(size_t)ci];
        TileComp& tc = t.tc[(size_t)ci];
        T1<Mq> t1;
        std::vector<float> cur;              // the LL image of the resolution below
        int cw = 0, ch = 0;
        for (int r = 0; r <= q.levels; r++) {
            Resolution& R = tc.res[(size_t)r];
            const int rw = R.x1 - R.x0, rh = R.y1 - R.y0;
            std::vector<float> arr((size_t)std::max(0, rw) * std::max(0, rh), 0.f);
            if (rw <= 0 || rh <= 0) { cur.swap(arr); cw = rw; ch = rh; continue; }
            if (r == 0) {
                Band& B = R.bands[0];
                B.xoff = 0; B.yoff = 0;
                for (auto& P : R.precs) for (auto& PB : P.pb) for (auto& cb : PB.cbs) decodeBlock(cb, B, q, arr, rw, t1);
                cur.swap(arr); cw = rw; ch = rh;
                continue;
            }
            // interleave: LL from `cur`, the three bands decoded straight into place
            // band arrays first (each in its own buffer), then scattered
            std::vector<std::vector<float>> bufs(3);
            for (int b = 0; b < 3; b++) {
                Band& B = R.bands[(size_t)b];
                const int bw = B.x1 - B.x0, bh = B.y1 - B.y0;
                bufs[(size_t)b].assign((size_t)std::max(0, bw) * std::max(0, bh), 0.f);
                B.xoff = 0; B.yoff = 0;
                if (bw > 0 && bh > 0)
                    for (auto& P : R.precs) { PrecBand& PB = P.pb[(size_t)b]; for (auto& cb : PB.cbs) decodeBlock(cb, B, q, bufs[(size_t)b], bw, t1); }
            }
            const Resolution& Rl = tc.res[(size_t)r - 1];
            for (int y = R.y0; y < R.y1; y++)
                for (int x = R.x0; x < R.x1; x++) {
                    float v;
                    const bool xo = x & 1, yo = y & 1;
                    if (!xo && !yo) v = cur[(size_t)((y / 2 - Rl.y0) * cw + (x / 2 - Rl.x0))];
                    else {
                        const int b = xo && !yo ? 0 : !xo && yo ? 1 : 2;
                        const Band& B = R.bands[(size_t)b];
                        const int bx = (xo ? (x - 1) / 2 : x / 2) - B.x0, by = (yo ? (y - 1) / 2 : y / 2) - B.y0;
                        const int bw = B.x1 - B.x0;
                        v = bufs[(size_t)b][(size_t)(by * bw + bx)];
                    }
                    arr[(size_t)((y - R.y0) * rw + (x - R.x0))] = v;
                }
            std::vector<float> col((size_t)std::max(rw, rh));
            for (int y = 0; y < rh; y++) { if (q.transform == 1) lift53(arr.data() + (size_t)y * rw, rw, R.x0, nullptr); else lift97(arr.data() + (size_t)y * rw, rw, R.x0); }
            for (int x = 0; x < rw; x++) {
                for (int y = 0; y < rh; y++) col[(size_t)y] = arr[(size_t)(y * rw + x)];
                if (q.transform == 1) lift53(col.data(), rh, R.y0, nullptr); else lift97(col.data(), rh, R.y0);
                for (int y = 0; y < rh; y++) arr[(size_t)(y * rw + x)] = col[(size_t)y];
            }
            cur.swap(arr); cw = rw; ch = rh;
        }
        tc.samples.swap(cur);
        (void)ch;
    }

    Image run(const uint8_t* d, size_t n) {
        size_t pos = 0;
        if (!mainHeader(d, n, pos)) return img;
        const int W = c.xsiz - c.xo, H = c.ysiz - c.yo;
        ntx = ceilDiv(c.xsiz - c.xto, c.xt); nty = ceilDiv(c.ysiz - c.yto, c.yt);
        if ((int64_t)ntx * nty > 65535) { fail("too many tiles"); return img; }
        tiles.assign((size_t)ntx * nty, Tile());
        for (int i = 0; i < ntx * nty; i++) {
            Tile& t = tiles[(size_t)i];
            t.idx = i;
            const int tx = i % ntx, ty = i / ntx;
            t.x0 = std::max(c.xto + tx * c.xt, c.xo); t.y0 = std::max(c.yto + ty * c.yt, c.yo);
            t.x1 = std::min(c.xto + (tx + 1) * c.xt, c.xsiz); t.y1 = std::min(c.yto + (ty + 1) * c.yt, c.ysiz);
            t.cp = c.cp; t.cod = c.cod;
        }
        // tile parts
        while (pos + 12 <= n && d[pos] == 0xFF && d[pos + 1] == 0x90) {
            const size_t start = pos;
            const int isot = (int)u16(d + pos + 4);
            size_t psot = u32(d + pos + 6);
            if (isot >= (int)tiles.size()) break;
            Tile& t = tiles[(size_t)isot];
            pos += 12;
            // tile part header markers up to SOD
            while (pos + 4 <= n && !(d[pos] == 0xFF && d[pos + 1] == 0x93)) {
                const int m = d[pos] << 8 | d[pos + 1];
                const size_t L = u16(d + pos + 2);
                if (L < 2 || pos + 2 + L > n) { fail("truncated tile header"); break; }
                if (m == 0xFF52) parseCod(d + pos + 4, L - 2, t.cod, t.cp, -1, false);
                else if (m == 0xFF5C) parseQcd(d + pos + 4, L - 2, t.cp, -1);
                pos += 2 + L;
            }
            if (pos + 2 > n) break;
            pos += 2;   // SOD
            size_t end = psot ? start + psot : n;
            if (end > n || end < pos) end = n;
            if (end >= 2 && !psot && d[end - 2] == 0xFF && d[end - 1] == 0xD9) end -= 2;
            t.data.insert(t.data.end(), d + pos, d + end);
            t.seen = true;
            pos = end;
        }
        img.width = W; img.height = H; img.precision = c.cp[0].prec; img.components = c.ncomp;
        img.levels = c.cp[0].levels; img.layers = c.cod.layers; img.reversible = c.cp[0].transform == 1;
        if (!img.error.empty()) return img;
        for (const CompParams& q : c.cp) if (q.style & 1) { fail("arithmetic-coding bypass style not supported"); return img; }
        img.pix.assign((size_t)W * H, 0);
        const int prec = c.cp[0].prec;
        const float shift = c.cp[0].sgnd ? 0.f : (float)(1 << (prec - 1)), maxv = (float)((1 << prec) - 1);
        bool anyBad = false;
        for (Tile& t : tiles) {
            if (!t.seen) { anyBad = true; continue; }
            buildTile(t);
            if (!readPackets(t)) anyBad = true;
            reconstruct(t, 0);
            const TileComp& tc = t.tc[0];
            const int tw = tc.x1 - tc.x0;
            for (int y = tc.y0; y < tc.y1; y++)
                for (int x = tc.x0; x < tc.x1; x++) {
                    const int ox = x * c.cp[0].xr - c.xo, oy = y * c.cp[0].yr - c.yo;
                    if (ox < 0 || oy < 0 || ox >= W || oy >= H) continue;
                    float v = tc.samples[(size_t)((y - tc.y0) * tw + (x - tc.x0))] + shift;
                    v = std::floor(v + 0.5f);
                    img.pix[(size_t)oy * W + ox] = (uint16_t)std::max(0.f, std::min(maxv, v));
                }
            t.tc.clear(); t.data.clear(); t.data.shrink_to_fit();
        }
        img.ok = !anyBad && img.error.empty();
        if (anyBad && img.error.empty()) img.error = "damaged or incomplete codestream";
        return img;
    }
};

} // namespace

bool findCodestream(const uint8_t* d, size_t n, size_t& off) {
    for (size_t i = 0; i + 4 <= n && i < 4096; i++)
        if (d[i] == 0xFF && d[i + 1] == 0x4F && d[i + 2] == 0xFF && d[i + 3] == 0x51) { off = i; return true; }
    return false;
}

Image decode(const uint8_t* d, size_t n) {
    Image bad;
    size_t off = 0;
    if (!findCodestream(d, n, off)) { bad.error = "no JPEG 2000 codestream"; return bad; }
    auto dec = std::make_unique<Decoder>();
    return dec->run(d + off, n - off);
}


// ================================================================================================ encoder (test signals)
namespace {

struct MqEnc {
    std::vector<uint8_t> out;       // out[0] is a dummy byte before the stream
    size_t bp = 0;
    uint32_t a = 0x8000, c = 0;
    int ct = 12;
    uint8_t st[19], mps[19];
    MqEnc() { out.assign(1, 0); resetCtx(); }
    void resetCtx() { memset(st, 0, sizeof st); memset(mps, 0, sizeof mps); st[0] = 4; st[17] = 3; st[18] = 46; }
    void byteout() {
        auto put = [&](uint8_t v) { bp++; if (bp >= out.size()) out.resize(bp + 1); out[bp] = v; };
        if (out[bp] == 0xFF) { put((uint8_t)(c >> 20)); c &= 0xFFFFF; ct = 7; }
        else if ((c & 0x8000000) == 0) { put((uint8_t)(c >> 19)); c &= 0x7FFFF; ct = 8; }
        else {
            out[bp]++;
            if (out[bp] == 0xFF) { c &= 0x7FFFFFF; put((uint8_t)(c >> 20)); c &= 0xFFFFF; ct = 7; }
            else { put((uint8_t)(c >> 19)); c &= 0x7FFFF; ct = 8; }
        }
    }
    void renorm() { do { a <<= 1; c <<= 1; ct--; if (ct == 0) byteout(); } while ((a & 0x8000) == 0); }
    int code(int cx, int d) {
        const QeRow& q = kQe[st[cx]];
        if (d == mps[cx]) {
            a -= q.qe;
            if ((a & 0x8000) == 0) { if (a < q.qe) a = q.qe; else c += q.qe; st[cx] = q.nmps; renorm(); }
            else c += q.qe;
        } else {
            a -= q.qe;
            if (a < q.qe) c += q.qe; else a = q.qe;
            if (q.sw) mps[cx] ^= 1;
            st[cx] = q.nlps;
            renorm();
        }
        return d;
    }
    // ends the codeword and returns its bytes
    std::vector<uint8_t> flush() {
        const uint32_t tempc = c + a;
        c |= 0xFFFF;
        if (c >= tempc) c -= 0x8000;
        c <<= ct; byteout();
        c <<= ct; byteout();
        std::vector<uint8_t> r(out.begin() + 1, out.begin() + (std::ptrdiff_t)(out[bp] == 0xFF ? bp : bp + 1));
        return r;
    }
};

struct BitWriter {
    std::vector<uint8_t> out;
    int cur = 0, nbits = 0, limit = 8;
    void bit(int b) {
        cur = cur << 1 | (b & 1);
        if (++nbits == limit) { out.push_back((uint8_t)cur); limit = cur == 0xFF ? 7 : 8; cur = 0; nbits = 0; }
    }
    void bits(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) bit((int)(v >> i) & 1); }
    void flush() {
        if (nbits) { cur <<= (limit - nbits); out.push_back((uint8_t)cur); limit = cur == 0xFF ? 7 : 8; cur = 0; nbits = 0; }
        if (!out.empty() && out.back() == 0xFF) out.push_back(0);
    }
};

struct TagEnc {
    struct Node { int value = 0x7fffffff, low = 0, parent = -1; bool known = false; };
    std::vector<Node> n;
    int w = 0, h = 0;
    void init(int W, int H) {
        w = W; h = H;
        std::vector<int> lw{W}, lh{H}, off{0};
        while (lw.back() > 1 || lh.back() > 1) { lw.push_back((lw.back() + 1) / 2); lh.push_back((lh.back() + 1) / 2); }
        int tot = 0;
        off.assign(lw.size(), 0);
        for (size_t i = 0; i < lw.size(); i++) { off[i] = tot; tot += lw[i] * lh[i]; }
        n.assign((size_t)tot, Node());
        for (size_t l = 0; l + 1 < lw.size(); l++)
            for (int y = 0; y < lh[l]; y++) for (int x = 0; x < lw[l]; x++) n[(size_t)(off[l] + y * lw[l] + x)].parent = off[l + 1] + (y / 2) * lw[l + 1] + x / 2;
    }
    void setValue(int leaf, int v) { for (int i = leaf; i >= 0 && n[(size_t)i].value > v; i = n[(size_t)i].parent) n[(size_t)i].value = v; }
    void encode(BitWriter& bw, int leaf, int threshold) {
        int stk[32], k = 0;
        for (int i = leaf; i >= 0 && k < 32; i = n[(size_t)i].parent) stk[k++] = i;
        int low = 0;
        while (k > 0) {
            Node& nd = n[(size_t)stk[--k]];
            if (low > nd.low) nd.low = low; else low = nd.low;
            while (low < threshold) {
                if (low >= nd.value) { if (!nd.known) { bw.bit(1); nd.known = true; } break; }
                bw.bit(0); low++;
            }
            nd.low = low;
        }
    }
};

void fwd53(float* a, int n) {          // forward 5/3 of a row that starts at coordinate 0 (low samples first in the interleaved array)
    if (n == 1) return;
    auto at = [&](int i) { if (i < 0) i = -i; if (i >= n) i = 2 * (n - 1) - i; return a[i]; };
    for (int i = 1; i < n; i += 2) a[i] -= std::floor((at(i - 1) + at(i + 1)) / 2.f);
    for (int i = 0; i < n; i += 2) a[i] += std::floor((at(i - 1) + at(i + 1) + 2.f) / 4.f);
}

} // namespace

std::vector<uint8_t> encodeLossless(const uint16_t* pix, int W, int H, int prec, int levels) {
    std::vector<uint8_t> out;
    if (W <= 0 || H <= 0 || prec < 1 || prec > 16 || levels < 0 || levels > 8) return out;
    const int NL = levels;
    // forward transform in a float array; level l works on the top left (cw x ch)
    std::vector<float> img((size_t)W * H);
    const int shift = 1 << (prec - 1);
    for (size_t i = 0; i < img.size(); i++) img[i] = (float)((int)pix[i] - shift);
    struct Sb { int orient, x0, y0, w, h; std::vector<int32_t> v; };
    std::vector<std::vector<Sb>> res((size_t)NL + 1);
    int cw = W, ch = H;
    std::vector<float> col((size_t)std::max(W, H));
    for (int l = 0; l < NL; l++) {
        for (int x = 0; x < cw; x++) { for (int y = 0; y < ch; y++) col[(size_t)y] = img[(size_t)y * W + x]; fwd53(col.data(), ch); for (int y = 0; y < ch; y++) img[(size_t)y * W + x] = col[(size_t)y]; }
        for (int y = 0; y < ch; y++) fwd53(img.data() + (size_t)y * W, cw);
        const int lw = (cw + 1) / 2, lh = (ch + 1) / 2, hw = cw / 2, hh = ch / 2;
        std::vector<float> tmp((size_t)cw * ch);
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) tmp[(size_t)y * cw + x] = img[(size_t)y * W + x];
        // bands of this level (resolution NL - l)
        std::vector<Sb>& bands = res[(size_t)(NL - l)];
        bands.assign(3, Sb());
        const int dims[3][2] = {{hw, lh}, {lw, hh}, {hw, hh}};
        for (int b = 0; b < 3; b++) { bands[(size_t)b].orient = b + 1; bands[(size_t)b].w = dims[b][0]; bands[(size_t)b].h = dims[b][1]; bands[(size_t)b].v.assign((size_t)dims[b][0] * dims[b][1], 0); }
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                const int v = (int)tmp[(size_t)y * cw + x];
                const bool xo = x & 1, yo = y & 1;
                if (!xo && !yo) img[(size_t)(y / 2) * W + x / 2] = (float)v;     // LL, kept for the next level (written into the top left)
                else { const int b = xo && !yo ? 0 : !xo && yo ? 1 : 2; bands[(size_t)b].v[(size_t)((yo ? (y - 1) / 2 : y / 2) * bands[(size_t)b].w + (xo ? (x - 1) / 2 : x / 2))] = v; }
            }
        cw = lw; ch = lh;
    }
    {
        Sb ll; ll.orient = 0; ll.w = cw; ll.h = ch; ll.v.resize((size_t)cw * ch);
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) ll.v[(size_t)y * cw + x] = (int)img[(size_t)y * W + x];
        res[0].assign(1, std::move(ll));
    }
    const int guard = 2;
    auto expOf = [&](int orient) { return prec + (orient == 0 ? 0 : orient == 3 ? 2 : 1); };
    // packets, one per resolution (one precinct, one layer)
    std::vector<uint8_t> body;
    for (int r = 0; r <= NL; r++) {
        BitWriter bw;
        std::vector<uint8_t> data;
        bw.bit(1);
        for (Sb& sb : res[(size_t)r]) {
            const int mb = guard + expOf(sb.orient) - 1;
            const int ncw = (sb.w + 63) / 64, nch = (sb.h + 63) / 64;
            if (ncw == 0 || nch == 0) continue;
            struct Blk { int x0, y0, x1, y1, nbps, passes; std::vector<uint8_t> bytes; };
            std::vector<Blk> blks((size_t)ncw * nch);
            TagEnc inc, zb;
            inc.init(ncw, nch); zb.init(ncw, nch);
            for (int by = 0; by < nch; by++)
                for (int bx = 0; bx < ncw; bx++) {
                    Blk& b = blks[(size_t)(by * ncw + bx)];
                    b.x0 = bx * 64; b.y0 = by * 64; b.x1 = std::min(sb.w, b.x0 + 64); b.y1 = std::min(sb.h, b.y0 + 64);
                    const int bw2 = b.x1 - b.x0, bh = b.y1 - b.y0;
                    std::vector<int32_t> blk((size_t)bw2 * bh);
                    int mx = 0;
                    for (int y = 0; y < bh; y++) for (int x = 0; x < bw2; x++) { const int v = sb.v[(size_t)(b.y0 + y) * sb.w + b.x0 + x]; blk[(size_t)y * bw2 + x] = v; mx = std::max(mx, std::abs(v)); }
                    b.nbps = 0; while ((mx >> b.nbps) != 0) b.nbps++;
                    if (b.nbps > mb) return {};      // does not fit the guard bits
                    if (b.nbps == 0) { inc.setValue(by * ncw + bx, 1); b.passes = 0; continue; }
                    inc.setValue(by * ncw + bx, 0);
                    zb.setValue(by * ncw + bx, mb - b.nbps);
                    T1<MqEnc> t1;
                    t1.w = bw2; t1.h = bh; t1.s = bw2 + 2; t1.orient = sb.orient; t1.vcausal = false; t1.src = blk.data();
                    t1.fl.assign((size_t)(bw2 + 2) * (bh + 2), 0);
                    t1.mag.assign((size_t)bw2 * bh, 0);
                    t1.cleanup(b.nbps - 1, false);
                    for (int bp = b.nbps - 2; bp >= 0; bp--) { t1.sigProp(bp); t1.magRef(bp); t1.cleanup(bp, false); }
                    b.passes = 3 * b.nbps - 2;
                    b.bytes = t1.mq.flush();
                }
            for (int by = 0; by < nch; by++)
                for (int bx = 0; bx < ncw; bx++) {
                    Blk& b = blks[(size_t)(by * ncw + bx)];
                    const int k = by * ncw + bx;
                    inc.encode(bw, k, 1);
                    if (b.passes == 0) continue;
                    zb.encode(bw, k, 999);
                    const int np = b.passes;
                    if (np == 1) bw.bit(0);
                    else if (np == 2) bw.bits(2, 2);
                    else if (np <= 5) { bw.bits(3, 2); bw.bits((uint32_t)(np - 3), 2); }
                    else if (np <= 36) { bw.bits(15, 4); bw.bits((uint32_t)(np - 6), 5); }
                    else { bw.bits(0x1FF, 9); bw.bits((uint32_t)(np - 37), 7); }
                    int lg = 0; while ((2 << lg) <= np) lg++;
                    int need = 0; while (((uint32_t)b.bytes.size() >> need) != 0) need++;
                    int lblock = 3;
                    while (lblock + lg < need) { bw.bit(1); lblock++; }
                    bw.bit(0);
                    bw.bits((uint32_t)b.bytes.size(), lblock + lg);
                    data.insert(data.end(), b.bytes.begin(), b.bytes.end());
                }
        }
        bw.flush();
        body.insert(body.end(), bw.out.begin(), bw.out.end());
        body.insert(body.end(), data.begin(), data.end());
    }
    auto p16 = [&](std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); };
    auto p32 = [&](std::vector<uint8_t>& v, uint32_t x) { p16(v, x >> 16); p16(v, x & 0xFFFF); };
    p16(out, 0xFF4F);
    p16(out, 0xFF51); p16(out, 38 + 3); p16(out, 0); p32(out, (uint32_t)W); p32(out, (uint32_t)H); p32(out, 0); p32(out, 0); p32(out, (uint32_t)W); p32(out, (uint32_t)H); p32(out, 0); p32(out, 0);
    p16(out, 1); out.push_back((uint8_t)(prec - 1)); out.push_back(1); out.push_back(1);
    p16(out, 0xFF52); p16(out, 12); out.push_back(0); out.push_back(0); p16(out, 1); out.push_back(0); out.push_back((uint8_t)NL); out.push_back(4); out.push_back(4); out.push_back(0); out.push_back(1);
    p16(out, 0xFF5C); p16(out, 3 + 3 * NL + 1); out.push_back((uint8_t)(guard << 5));
    for (int r = 0; r <= NL; r++) for (const Sb& sb : res[(size_t)r]) out.push_back((uint8_t)(expOf(sb.orient) << 3));
    p16(out, 0xFF90); p16(out, 10); p16(out, 0); p32(out, (uint32_t)(12 + 2 + body.size())); out.push_back(0); out.push_back(1);
    p16(out, 0xFF93);
    out.insert(out.end(), body.begin(), body.end());
    p16(out, 0xFFD9);
    return out;
}

} // namespace dect2::j2k
